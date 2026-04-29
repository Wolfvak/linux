/*
 * IS-SPI-USB-ADAPTER driver.
 */

#include <linux/bits.h>
#include <linux/mutex.h>

#include <linux/of.h>
#include <linux/spi/spi.h>
#include <linux/module.h>

#include <linux/serial_core.h>
#include <linux/tty_flip.h>
#include <linux/units.h>

/* maximum baudrate that's supported by the device */
#define ADAPTER_BAUDRATE_MAX	(2000000)

/* total FIFO size that can be held in the device */
#define ADAPTER_FIFO_SIZE	(1024)

/* maximum transfer size for read/write transactions */
#define ADAPTER_XFER_SIZE	(16)

#define CMD_ECHO1(n)	(0x60 | (n))
#define CMD_ECHO2(n)	(0xA0 | (n))
#define CMD_NOP		(0x80)
#define CMD_RESET	(0x90)
#define CMD_TX_AVAIL	(0xB0)
#define CMD_RX_AVAIL	(0xC0)
#define CMD_TX_LEN(n)	(0xD0 | ((n) - 1))	// valid for 0 < n <= ADAPTER_XFER_SIZE
#define CMD_RX_LEN(n)	(0xE0 | ((n) - 1))	// valid for 0 < n <= ADAPTER_XFER_SIZE
#define CMD_INITIALIZE	(0xF0)

#define FLAGS_RX_ENABLE	BIT(0)
#define FLAGS_TX_ENABLE	BIT(1)

struct is_spi_usb_adapter_port {
	/* uart/serial/SPI driver components */
	struct uart_port	port;
	struct spi_device	*spi;

	/* driver config and state */
	struct mutex	lock;
	unsigned	flags;
	u8		nopbuf[ADAPTER_XFER_SIZE];

	/* polling helpers */
	struct kthread_worker		kworker;
	struct kthread_delayed_work	poll;
	struct task_struct		*kworker_task;
};

#define to_is_spi_usb_adapter_port(port)	container_of(port, struct is_spi_usb_adapter_port, port)

static struct uart_driver is_spi_usb_adapter_uart_driver = {
	.owner		= THIS_MODULE,
	.driver_name	= KBUILD_MODNAME,
	.dev_name	= "ttyIS",
	.nr		= 1,
};

/**
 * is_spi_usb_adapter_send_cmd - send a command to the adapter with optional data
 * @is: IS-SPI-USB-ADAPTER device pointer
 * @cmd: command to send
 * @cmd_resp: pointer to where the command response will be stored
 * @tx_data: pointer to the data to be transmitted
 * @rx_data: pointer to where the data response will be stored
 * @data_len: length of the data transmission in bytes (can be 0 to disable it)
 *
 * Synchronously sends the @cmd command and optionally the data stored in @tx_data
 * if @data_len is > 0.
 * Returns the same error codes as @spi_sync.
 */
static int is_spi_usb_adapter_send_cmd(struct is_spi_usb_adapter_port *is,
					u8 cmd, u8 *cmd_resp,
					const void *tx_data, void *rx_data, size_t data_len)
{
	struct spi_message msg;
	struct spi_transfer xfer[2];
	int ret;

	spi_message_init(&msg);

	memset(&xfer[0], 0, sizeof(xfer[0]));
	xfer[0].tx_buf = &cmd;
	xfer[0].rx_buf = cmd_resp;
	xfer[0].len = 1;
	spi_message_add_tail(&xfer[0], &msg);

	if (data_len) {
		memset(&xfer[1], 0, sizeof(xfer[1]));
		xfer[1].tx_buf = tx_data;
		xfer[1].rx_buf = rx_data;
		xfer[1].len = data_len;
		spi_message_add_tail(&xfer[1], &msg);
	}

	ret = spi_sync(is->spi, &msg);
	if (ret) {
		pr_err("is_spi_usb_adapter_send_cmd(%02x, %d): failed to spi_sync (%d)\n",
			cmd, data_len, ret);
	}
	return ret;
}

static int is_spi_usb_adapter_echo_test(struct is_spi_usb_adapter_port *is)
{
	u8 cmd, cmd_resp;
	int ret;

	for (unsigned i = 0; i < 16; i++) {
		cmd = CMD_ECHO1(i);
		cmd_resp = 0;

		ret = is_spi_usb_adapter_send_cmd(is, cmd, &cmd_resp, NULL, NULL, 0);
		if (ret) return ret;

		if (cmd != cmd_resp) {
			pr_err("is_spi_usb_adapter_echo_test(%d): echo test failed, "
				"sent %02x and got back %02x\n", i, cmd, cmd_resp);
			return -ENODEV;
		}

		pr_debug("is_spi_usb_adapter_echo_test(%d): passed", i);
	}

	return 0;
}

static int is_spi_usb_adapter_fifo_avail(struct is_spi_usb_adapter_port *is, u8 cmd, u16 *avail)
{
	u16 rx_avail;
	u8 cmd_resp;
	int ret;

	ret = is_spi_usb_adapter_send_cmd(is, cmd, &cmd_resp, is->nopbuf, &rx_avail, 2);
	if (ret) return ret;

	/** response should be { 0x90, LSB, MSB } */
	if (cmd_resp != CMD_RESET) {
		pr_err("is_spi_usb_adapter_fifo_avail(%02x): command response is %02x\n",
			cmd, cmd_resp);
		return -EINVAL;
	}

	*avail = le16_to_cpu(rx_avail);
	if (*avail > ADAPTER_FIFO_SIZE)
		pr_warn("is_spi_usb_adapter_fifo_avail(%02x): %d bytes available"
			", larger than the fifo size\n", cmd, *avail);
	return 0;
}

static int is_spi_usb_adapter_fifo_tx(struct is_spi_usb_adapter_port *is, const u8 *tx, size_t len)
{
	u8 tx_resp[ADAPTER_XFER_SIZE], cmd, cmd_resp;
	int ret;

	if (!len || len > ADAPTER_FIFO_SIZE) {
		pr_err("is_spi_usb_adapter_fifo_tx(): invalid xfer size of %d bytes\n", len);
		return -EINVAL;
	}

	do {
		size_t xfer_len = min(len, ADAPTER_XFER_SIZE);

		cmd = CMD_TX_LEN(xfer_len);
		ret = is_spi_usb_adapter_send_cmd(is, cmd, &cmd_resp, tx, tx_resp, xfer_len);
		if (ret) return ret;

		/* response should be { 0x90, 0x90 repeated len times } */
		if (cmd_resp != CMD_RESET) {
			pr_err("is_spi_usb_adapter_fifo_tx(): command response to %02x is %02x\n",
				cmd, cmd_resp);
			return -EINVAL;
		}

		/* todo: add option to disable this check */
		for (size_t i = 0; i < xfer_len; i++) {
			if (tx_resp[i] != CMD_RESET) {
				pr_err("is_spi_usb_adapter_fifo_tx(): data response @ %d is %02x\n",
					i, tx_resp[i]);
				return -EINVAL;
			}
		}

		tx += xfer_len;
		len -= xfer_len;
	} while(len > 0);
	return 0;
}

static int is_spi_usb_adapter_fifo_rx(struct is_spi_usb_adapter_port *is, u8 *rx, size_t len)
{
	u8 cmd, cmd_resp;
	int ret;

	if (!len || len > ADAPTER_FIFO_SIZE) {
		pr_err("is_spi_usb_adapter_fifo_rx(): invalid xfer size of %d bytes\n", len);
		return -EINVAL;
	}

	do {
		size_t xfer_len = min(len, ADAPTER_XFER_SIZE);

		cmd = CMD_RX_LEN(xfer_len);
		ret = is_spi_usb_adapter_send_cmd(is, cmd, &cmd_resp, is->nopbuf, rx, xfer_len);
		if (ret) return ret;

		/** response should be { 0x90, data... } */
		if (cmd_resp != CMD_RESET) {
			pr_err("is_spi_usb_adapter_fifo_tx(): command response to %02x is %02x\n",
				cmd, cmd_resp);
			return -EINVAL;
		}

		rx += xfer_len;
		len -= xfer_len;
	} while(len > 0);
	return 0;
}

static void is_spi_usb_adapter_start_tx(struct uart_port *port)
{
	struct is_spi_usb_adapter_port *is = to_is_spi_usb_adapter_port(port);
	is->flags |= FLAGS_TX_ENABLE;
}

static void is_spi_usb_adapter_stop_tx(struct uart_port *port)
{
	struct is_spi_usb_adapter_port *is = to_is_spi_usb_adapter_port(port);
	is->flags &= ~FLAGS_TX_ENABLE;
}

static void is_spi_usb_adapter_start_rx(struct uart_port *port)
{
	struct is_spi_usb_adapter_port *is = to_is_spi_usb_adapter_port(port);
	is->flags |= FLAGS_RX_ENABLE;
}

static void is_spi_usb_adapter_stop_rx(struct uart_port *port)
{
	struct is_spi_usb_adapter_port *is = to_is_spi_usb_adapter_port(port);
	is->flags &= ~FLAGS_RX_ENABLE;
}

static int is_spi_usb_adapter_uart_rx(struct is_spi_usb_adapter_port *is)
{
	struct uart_port *port = &is->port;
	struct tty_port *tport = &port->state->port;
	u8 rx_buffer[ADAPTER_FIFO_SIZE / 4]; /* prevent large stack buffer usage */
	u16 rx_avail, rx_done = 0;
	int ret;

	while(!(ret = is_spi_usb_adapter_fifo_avail(is, CMD_RX_AVAIL, &rx_avail)) && rx_avail) {
		rx_avail = min(rx_avail, ADAPTER_FIFO_SIZE / 4);
		ret = is_spi_usb_adapter_fifo_rx(is, rx_buffer, rx_avail);
		if (ret)
			break;

		tty_insert_flip_string(tport, rx_buffer, rx_avail);
		port->icount.rx += rx_avail;
	}

	if (ret)
		pr_err("is_spi_usb_adapter_uart_rx(): %d\n", ret);

	tty_flip_buffer_push(tport);
	return ret < 0 ? ret : rx_done;
}

static int is_spi_usb_adapter_uart_tx(struct is_spi_usb_adapter_port *is)
{
	struct uart_port *port = &is->port;
	struct tty_port *tport = &port->state->port;
	u16 tx_avail, tx_done = 0;
	unsigned long flags;
	int ret = 0;

	if (kfifo_is_empty(&tport->xmit_fifo))
		return 0;

	while(!(ret = is_spi_usb_adapter_fifo_avail(is, CMD_TX_AVAIL, &tx_avail)) && tx_avail) {
		unsigned char *tail;

		size_t tx_len = kfifo_out_linear_ptr(&tport->xmit_fifo, &tail, tx_avail);
		if (!tx_len) break;

		ret = is_spi_usb_adapter_fifo_tx(is, tail, tx_len);
		if (ret) break;

		uart_xmit_advance(port, tx_len);
		tx_done += tx_len;
	}

	if (ret) {
		pr_err("is_spi_usb_adapter_uart_tx(): %d\n", ret);
	}

	uart_port_lock_irqsave(port, &flags);
	if (kfifo_len(&tport->xmit_fifo) < WAKEUP_CHARS)
		uart_write_wakeup(port);
	uart_port_unlock_irqrestore(port, flags);

	return ret < 0 ? ret : tx_done;
}

static void is_spi_usb_adapter_poll_work(struct kthread_work *work)
{
	struct is_spi_usb_adapter_port *is =
		container_of(work, struct is_spi_usb_adapter_port, poll.work);

	mutex_lock(&is->lock);
	if (is->flags & FLAGS_RX_ENABLE)
		is_spi_usb_adapter_uart_rx(is);
	if (is->flags & FLAGS_TX_ENABLE)
		is_spi_usb_adapter_uart_tx(is);

	kthread_queue_delayed_work(&is->kworker, &is->poll,
				uart_poll_timeout(&is->port));
	mutex_unlock(&is->lock);
}

static unsigned int is_spi_usb_adapter_tx_empty(struct uart_port *port)
{
	return TIOCSER_TEMT;
}

static void is_spi_usb_adapter_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	/* there's no MCR for this adapter */
}

static unsigned int is_spi_usb_adapter_get_mctrl(struct uart_port *port)
{
	return TIOCM_CTS | TIOCM_DSR | TIOCM_CAR;
}

static int is_spi_usb_adapter_startup(struct uart_port *port)
{
	struct is_spi_usb_adapter_port *is = to_is_spi_usb_adapter_port(port);

	/* issue a device init */
	static const u8 expected_resp[4] = { 'S', 'P', 'I', 'U' };
	u8 cmd_resp, data_resp[4];
	int ret;

	ret = is_spi_usb_adapter_send_cmd(is,
		CMD_INITIALIZE, &cmd_resp,
		is->nopbuf, data_resp, 4);
	if (ret)
		return ret;

	if (memcmp(data_resp, expected_resp, 4) != 0) {
		pr_err("is_spi_usb_adapter_startup(): unexpected magic "
			"response %c%c%c%c (%02x%02x%02x%02x)\n",
			data_resp[0], data_resp[1], data_resp[2], data_resp[3],
			data_resp[0], data_resp[1], data_resp[2], data_resp[3]);
		return -EINVAL;
	}

	kthread_queue_delayed_work(&is->kworker, &is->poll, 0);
	is->flags = FLAGS_RX_ENABLE | FLAGS_TX_ENABLE;

	return 0;
}

static void is_spi_usb_adapter_shutdown(struct uart_port *port)
{
	struct is_spi_usb_adapter_port *is = to_is_spi_usb_adapter_port(port);
	kthread_cancel_delayed_work_sync(&is->poll);
	kthread_flush_worker(&is->kworker);
}

static void is_spi_usb_adapter_set_termios(struct uart_port *port, struct ktermios *new,
				 const struct ktermios *old)
{
	struct is_spi_usb_adapter_port *is = to_is_spi_usb_adapter_port(port);
	unsigned long flags;
	unsigned int baud;

	kthread_cancel_delayed_work_sync(&is->poll);
	kthread_flush_worker(&is->kworker);

	/* update baudrate */
	baud = uart_get_baud_rate(port, new, old, 9600, ADAPTER_BAUDRATE_MAX);

	uart_port_lock_irqsave(port, &flags);
	uart_update_timeout(port, new->c_cflag, baud);
	kthread_queue_delayed_work(&is->kworker, &is->poll,
				uart_poll_timeout(port));
	uart_port_unlock_irqrestore(port, flags);
}

static const char *is_spi_usb_adapter_type(struct uart_port *port)
{
	return "IS-SPI-USB-ADAPTER";
}

static void is_spi_usb_adapter_config_port(struct uart_port *port, int flags)
{
	port->type = 1;
}

static const struct uart_ops is_spi_usb_adapter_ops = {
	.tx_empty	= is_spi_usb_adapter_tx_empty,
	.set_mctrl	= is_spi_usb_adapter_set_mctrl,
	.get_mctrl	= is_spi_usb_adapter_get_mctrl,
	.startup	= is_spi_usb_adapter_startup,
	.shutdown	= is_spi_usb_adapter_shutdown,
	.start_tx	= is_spi_usb_adapter_start_tx,
	.stop_tx	= is_spi_usb_adapter_stop_tx,
	.start_rx	= is_spi_usb_adapter_start_rx,
	.stop_rx	= is_spi_usb_adapter_stop_rx,
	.set_termios	= is_spi_usb_adapter_set_termios,
	.type		= is_spi_usb_adapter_type,
	.config_port	= is_spi_usb_adapter_config_port,
};

static int is_spi_usb_adapter_spi_probe(struct spi_device *spi)
{
	struct is_spi_usb_adapter_port *is;
	struct uart_port *port;
	int ret;

	is = devm_kzalloc(&spi->dev, sizeof(struct is_spi_usb_adapter_port), GFP_KERNEL);
	if (!is) {
		pr_err("failed to allocate memory\n");
		return -ENOMEM;
	}

	mutex_init(&is->lock);
	is->spi = spi;
	is->flags = 0;
	memset(is->nopbuf, CMD_NOP, sizeof(is->nopbuf));

	port = &is->port;

	/* configure the runner thread with a single delayed worker for polling */
	kthread_init_worker(&is->kworker);
	is->kworker_task = kthread_run(kthread_worker_fn, &is->kworker, KBUILD_MODNAME);
	if (IS_ERR(is->kworker_task)) {
		pr_err("failed to initialize kworker (%lu)\n", PTR_ERR(is->kworker_task));
		return PTR_ERR(is->kworker_task);
	}
	sched_set_fifo(is->kworker_task);
	kthread_init_delayed_work(&is->poll, is_spi_usb_adapter_poll_work);

	/* set up the SPI bus */
	spi_set_drvdata(spi, port);
	spi->bits_per_word = 8;
	spi->max_speed_hz = ADAPTER_BAUDRATE_MAX;
	spi->cs_inactive = (struct spi_delay){.value = 90, .unit = SPI_DELAY_UNIT_USECS};
	ret = spi_setup(spi);
	if (ret) {
		pr_err("failed to set up SPI device (%d)\n", ret);
		goto stop_kthread;
	}

	/* do an echo test, make sure the device responds correctly at this freq */
	ret = is_spi_usb_adapter_echo_test(is);
	if (ret) {
		pr_err("echo test failed, aborting driver setup (%d)\n", ret);
		goto stop_kthread;
	}

	/* set up the UART port */
	port->dev	= &spi->dev;
	port->iotype	= UPIO_PORT;
	port->iobase	= 0;
	port->flags	= UPF_FIXED_TYPE | UPF_BOOT_AUTOCONF;
	port->ops	= &is_spi_usb_adapter_ops;
	port->fifosize	= ADAPTER_FIFO_SIZE;
	port->type	= PORT_UNKNOWN;
	port->line	= 0;	/* only 1 per system */
	port->membase	= (void __iomem *)~0;

	ret = uart_add_one_port(&is_spi_usb_adapter_uart_driver, port);
	if (ret) {
		pr_err("failed to add uart port (%d)\n", ret);
		goto stop_kthread;
	}

	pr_info("Registered IS-SPI-USB-ADAPTER driver");
	return 0;

stop_kthread:
	kthread_stop(is->kworker_task);
	return ret;
}

static void is_spi_usb_adapter_spi_remove(struct spi_device *spi)
{
	struct uart_port *port = spi_get_drvdata(spi);
	uart_remove_one_port(&is_spi_usb_adapter_uart_driver, port);
}

static const struct spi_device_id __maybe_unused is_spi_usb_adapter_spi_ids[] = {
	{ "spi-usb-adapter" },
	{ }
};
MODULE_DEVICE_TABLE(spi, is_spi_usb_adapter_spi_ids);

static const struct of_device_id __maybe_unused is_spi_usb_adapter_dt_ids[] = {
	{ .compatible = "is,spi-usb-adapter" },
	{ }
};
MODULE_DEVICE_TABLE(of, is_spi_usb_adapter_dt_ids);

static struct spi_driver is_spi_usb_adapter_spi_driver = {
	.driver = {
		.name		= KBUILD_MODNAME,
		.of_match_table	= is_spi_usb_adapter_dt_ids,
	},
	.probe		= is_spi_usb_adapter_spi_probe,
	.remove		= is_spi_usb_adapter_spi_remove,
	.id_table	= is_spi_usb_adapter_spi_ids,
};

static int __init is_spi_usb_adapter_init(void)
{
	int res = uart_register_driver(&is_spi_usb_adapter_uart_driver);
	if (res) return res;

	res = spi_register_driver(&is_spi_usb_adapter_spi_driver);
	if (res) uart_unregister_driver(&is_spi_usb_adapter_uart_driver);
	return res;
}
module_init(is_spi_usb_adapter_init);

static void __exit is_spi_usb_adapter_exit(void)
{
	uart_unregister_driver(&is_spi_usb_adapter_uart_driver);
	spi_unregister_driver(&is_spi_usb_adapter_spi_driver);
}
module_exit(is_spi_usb_adapter_exit);

MODULE_DESCRIPTION("IS-SPI-USB-ADAPTER serial port driver");
MODULE_ALIAS("spi:is_spi_usb_adapter");
