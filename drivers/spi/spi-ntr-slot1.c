// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#define DRIVER_NAME "ntr_slot1_auxspi"

#include <linux/io.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/spi/spi.h>

struct ntr_slot1;
extern int ntr_slot1_configure_spi(struct ntr_slot1 *slot1);
extern int ntr_slot1_release(struct ntr_slot1 *slot1);
extern void __iomem *ntr_slot1_iomem(struct ntr_slot1 *slot1);

struct ntr_spi {
	struct ntr_slot1 *slot1;
	void __iomem *io;
};

static void ntr_spi_wait_busy(void __iomem *io)
{
	while(ioread16(io + 0) & BIT(7))
		usleep_range(1, 4);
}

static u8 ntr_spi_txrx_byte(void __iomem *io, u8 v)
{
	iowrite16(v, io + 2);
	ntr_spi_wait_busy(io);
	u8 r = ioread16(io + 2);
	pr_info("tx = 0x%02x, rx = 0x%02x\n", v, r);
	return r;
}

static int ntr_spi_xfer(struct ntr_spi *spi,
			struct spi_device *dev,
			struct spi_transfer *xfer,
			int last)
{
	void __iomem *io = spi->io;

	u8 *rx = xfer->rx_buf;
	const u8 *tx = xfer->tx_buf;

	if (!rx) {
		pr_err("NULL RX pointer\n");
		return -EINVAL;
	}

	if (!tx) {
		pr_err("NULL TX pointer\n");
		return -EINVAL;
	}

	static const u16 cnt = 3 | BIT(13) | BIT(15);
	ntr_spi_wait_busy(io); // make sure there are no pending transactions

	for (unsigned i = 0; i < xfer->len; i++) {
		if (last && (i == (xfer->len - 1))) {
			iowrite16(cnt, io); // deselect CS if this is the last byte
			pr_info("deselect CS");
			ntr_spi_wait_busy(io);
		}

		rx[i] = ntr_spi_txrx_byte(io, tx[i]);
	}

	return 0;
}

static int ntr_spi_transfer_one_message(struct spi_controller *ctrl,
					struct spi_message *msg)
{
	struct ntr_spi *spi = spi_controller_get_devdata(ctrl);
	struct spi_device *dev = msg->spi;
	struct spi_transfer *xfer = NULL;

	int status = 0;

	status = ntr_slot1_configure_spi(spi->slot1);
	if (status != 0) {
		pr_err("failed to configure Slot-1 to be in SPI mode (%d)\n", status);
		return status;
	}

	// u16 cnt = ioread16(io);
	void __iomem *io = spi->io;

	static const u16 cnt = 3 | BIT(13) | BIT(15);
	iowrite16(cnt | BIT(6), io); // hold CS (bit6)
	pr_info("hold CS");
	ntr_spi_wait_busy(io);

	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		status = ntr_spi_xfer(spi, dev, xfer, spi_transfer_is_last(ctrl, xfer) ? 1 : 0);
		if (status != 0) break;

		msg->actual_length += xfer->len;
		spi_transfer_delay_exec(xfer);
	}

	msg->status = status;
	spi_finalize_current_message(ctrl);
	ntr_slot1_release(spi->slot1);
	return 0;
}

static int ntr_spi_probe(struct platform_device *pdev)
{
	int err;
	struct ntr_spi *spi;
	struct spi_controller *ctrl;

	struct device *parent_dev;
	struct platform_device *parent_pdev;

	pr_info("starting driver\n");

	parent_dev = pdev->dev.parent;
	if (!parent_dev) {
		pr_err("device has no registered parent\n");
		return -EINVAL;
	}

	parent_pdev = to_platform_device(parent_dev);

	ctrl = devm_spi_alloc_host(&pdev->dev, sizeof(struct ntr_spi));
	if (!ctrl) {
		pr_err("failed to allocate SPI controller\n");
		return -ENOMEM;
	}

	spi = spi_controller_get_devdata(ctrl);
	spi->slot1 = platform_get_drvdata(parent_pdev);
	if (!spi->slot1) {
		pr_err("failed to get the Slot-1 handle\n");
		return -EINVAL;
	}

	spi->io = ntr_slot1_iomem(spi->slot1);
	pr_debug("mapped SPI control registers @ %px\n", spi->io);

	ctrl->dev.of_node = pdev->dev.of_node;
	ctrl->mode_bits = SPI_MODE_0 | SPI_NO_CS;
	ctrl->flags = SPI_CONTROLLER_MUST_RX | SPI_CONTROLLER_MUST_TX;
	ctrl->bits_per_word_mask = SPI_BPW_MASK(8);
	ctrl->transfer_one_message = ntr_spi_transfer_one_message;
	ctrl->max_speed_hz = 1u << 22;
	ctrl->min_speed_hz = 1u << 19;
	ctrl->num_chipselect = 1;
	platform_set_drvdata(pdev, ctrl);

	err = devm_spi_register_controller(&pdev->dev, ctrl);
	if (err) {
		pr_err("failed to register SPI controller (%d)\n", err);
		return -ENODEV;
	}

	pr_info("loaded!\n");
	return 0;
}

static struct platform_driver ntr_spi_driver = {
	.probe	= ntr_spi_probe,

	.driver = {
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
	},
};
module_platform_driver(ntr_spi_driver);

MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" DRIVER_NAME);
