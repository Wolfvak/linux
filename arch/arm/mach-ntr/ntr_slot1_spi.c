// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#define DRIVER_NAME "ntr_slot1_spi"

#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/spi/spi.h>

#include <mach/slot1.h>

#define SPI_NTR_SLOT1_FREQUENCY	(4000000)

static void ntr_slot1_spi_wait_busy(void __iomem *io)
{
	u16 cnt;
	readw_poll_timeout(io + 0, cnt, !(cnt & BIT(7)), 0, 0);
}

static u8 ntr_slot1_spi_txrx_byte(void __iomem *io, u8 v)
{
	iowrite16(v, io + 2);
	ntr_slot1_spi_wait_busy(io);
	u8 r = ioread16(io + 2);
	return r;
}

static int ntr_slot1_spi_baudrate_div(const struct spi_transfer *xfer)
{
	/*
	 * our transfer rate must be <= xfer->speed_hz
	 * and we can only do /1, /2, /4 or /8
	 */
	unsigned freq = SPI_NTR_SLOT1_FREQUENCY;
	for (unsigned div = 0; div < 4; div++, freq >>= 1) {
		if (freq <= xfer->speed_hz)
			return div;
	}

	pr_warn_ratelimited(
		"requested transfer rate %d Hz is too low, expect glitches\n",
		xfer->speed_hz);
	return 3;
}

static int ntr_slot1_spi_xfer(void __iomem *io,
			struct spi_device *dev,
			struct spi_transfer *xfer,
			int last)
{
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

	u16 cnt = (ioread16(io) & ~3) | ntr_slot1_spi_baudrate_div(xfer);
	iowrite16(cnt | BIT(6), io);	// start and hold CS
	ntr_slot1_spi_wait_busy(io);		// wait until ready

	for (unsigned i = 0; i < xfer->len; i++) {
		if (last && (i == xfer->len-1)) {
			iowrite16(cnt & ~BIT(6), io); // deselect CS if this is the last byte
			ntr_slot1_spi_wait_busy(io);
		}

		rx[i] = ntr_slot1_spi_txrx_byte(io, tx[i]);
	}

	return 0;
}

static int ntr_slot1_spi_transfer_one_message(struct spi_controller *ctrl,
					struct spi_message *msg)
{
	struct platform_device *slot1 = spi_controller_get_devdata(ctrl);
	struct spi_device *dev = msg->spi;
	struct spi_transfer *xfer = NULL;

	int status = 0;
	void __iomem *io = ntr_slot1_lock(slot1, NTR_SLOT1_SPI);
	if (IS_ERR(io)) {
		pr_err("failed to configure Slot-1 to be in SPI mode (%ld)\n", PTR_ERR(io));
		return status;
	}

	ntr_slot1_spi_wait_busy(io);

	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		if (xfer->cs_change) {
			/**
			 * xfer->cs_change "affects chipselect after this transfer completes"
			 * should it deassert and reassert after the last byte of the xfer?
			 * if so we should treat it as the last xfer of the msg
			 */
			pr_warn_ratelimited("spi transfer requested an unsupported chipselect change");
		}

		status = ntr_slot1_spi_xfer(io, dev, xfer, spi_transfer_is_last(ctrl, xfer));
		if (status != 0) {
			break;
		}

		msg->actual_length += xfer->len;
		spi_transfer_delay_exec(xfer);
	}

	msg->status = status;
	spi_finalize_current_message(ctrl);
	ntr_slot1_release(slot1);
	return 0;
}

static int ntr_slot1_spi_probe(struct platform_device *pdev)
{
	int err;
	struct spi_controller *ctrl;
	struct platform_device *slot1;

	slot1 = ntr_slot1_find_pdev();
	if (IS_ERR_OR_NULL(slot1)) {
		pr_err("ntr_slot1_spi_probe: failed to get Slot-1 driver handle (%px)", slot1);
		return -ENODEV;
	}

	ctrl = devm_spi_alloc_host(&pdev->dev, 0);
	if (!ctrl) {
		pr_err("ntr_slot1_spi_probe: failed to allocate SPI controller\n");
		return -ENOMEM;
	}
	spi_controller_set_devdata(ctrl, slot1);

	ctrl->dev.of_node = pdev->dev.of_node;
	ctrl->mode_bits = SPI_MODE_0 | SPI_NO_CS;
	ctrl->flags = SPI_CONTROLLER_MUST_RX | SPI_CONTROLLER_MUST_TX;
	ctrl->bits_per_word_mask = SPI_BPW_MASK(8);
	ctrl->transfer_one_message = ntr_slot1_spi_transfer_one_message;
	ctrl->max_speed_hz = SPI_NTR_SLOT1_FREQUENCY;
	ctrl->min_speed_hz = SPI_NTR_SLOT1_FREQUENCY / 8;
	ctrl->num_chipselect = 1;
	platform_set_drvdata(pdev, ctrl);

	err = devm_spi_register_controller(&pdev->dev, ctrl);
	if (err) {
		pr_err("failed to register SPI controller (%d)\n", err);
		return -ENODEV;
	}

	pr_info("Started NTR Slot-1 SPI driver\n");
	return 0;
}

static const struct of_device_id ntr_slot1_spi_of_match[] = {
	{ .compatible = "nintendo,ntr-slot1-spi" },
	{}
};
MODULE_DEVICE_TABLE(of, ntr_slot1_spi_of_match);

static struct platform_driver ntr_slot1_spi_driver = {
	.probe	= ntr_slot1_spi_probe,

	.driver = {
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
		.of_match_table	= ntr_slot1_spi_of_match,
	},
};
module_platform_driver(ntr_slot1_spi_driver);

MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" DRIVER_NAME);
