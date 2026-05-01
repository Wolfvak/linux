// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#define DRIVER_NAME "ntr_slot1"

#include <linux/io.h>
#include <linux/of.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>

#include <mach/tcm.h>
#include <mach/slot1.h>

struct ntr_slot1 {
    void __iomem *io;
    struct mutex lock;
    enum ntr_slot1_mode mode;
};

struct platform_device *ntr_slot1_find_pdev(void)
{
    struct device_node *slot1_node = of_find_compatible_node(NULL, NULL, "nintendo,ntr-slot1");
    if (IS_ERR_OR_NULL(slot1_node)) {
        pr_err("ntr_slot1_find_pdev: failed to find compatible node (%px)", slot1_node);
        return NULL;
    }

    struct platform_device *pdev = of_find_device_by_node(slot1_node);
    if (IS_ERR_OR_NULL(pdev)) {
        pr_err("ntr_slot1_find_pdev: failed to find pdev of node %px (%px)", slot1_node, pdev);
        return NULL;
    }

    return pdev;
}

void __iomem *ntr_slot1_lock(struct platform_device *pdev, enum ntr_slot1_mode mode)
{
    unsigned control;
    struct ntr_slot1 *slot1;

    if (IS_ERR_OR_NULL(pdev)) {
        pr_err_ratelimited("ntr_slot1_lock: invalid pdev %p", pdev);
        return ERR_PTR(-ENODEV);
    }

    switch(mode) {
        case NTR_SLOT1_DISABLED:
            control = 0; /* fully disabled */
            break;

        case NTR_SLOT1_SPI:
            control = BIT(15) | BIT(13); /* enabled in SPI mode, IRQs disabled */
            break;

        case NTR_SLOT1_ROM:
            control = BIT(15) | BIT(14); /* enabled in parallel ROM mode, IRQs enabled */
            break;

        default:
            pr_err_ratelimited("ntr_slot1_lock: invalid mode %d", mode);
            return ERR_PTR(-EINVAL);
    }

    slot1 = platform_get_drvdata(pdev);

    /* acquire the mutex and update the mode */
    mutex_lock(&slot1->lock);
    slot1->mode = mode;
    iowrite16(control, slot1->io);
    return slot1->io;
}

int ntr_slot1_release(struct platform_device *pdev)
{
    struct ntr_slot1 *slot1;

    if (IS_ERR_OR_NULL(pdev)) {
        pr_err_ratelimited("ntr_slot1_release: invalid pdev %p", pdev);
        return -ENODEV;
    }

    slot1 = platform_get_drvdata(pdev);

    /* disable all interfaces and release the mutex */
    iowrite16(0, slot1->io);
    slot1->mode = NTR_SLOT1_DISABLED;
    mutex_unlock(&slot1->lock);
    return 0;
}

static int ntr_slot1_probe(struct platform_device *pdev)
{
    void __iomem *io;
    struct ntr_slot1 *slot1;

    io = devm_platform_ioremap_resource(pdev, 0);
    if (IS_ERR_OR_NULL(io)) {
        pr_err("ntr_slot1_probe: failed to request memory resources");
        return -EINVAL;
    }

    slot1 = ntr_alloc_dtcm(sizeof(*slot1));
    slot1->io = io;
    mutex_init(&slot1->lock);

    /* initialize the slot as DISABLED */
    iowrite16(0, io);
    slot1->mode = NTR_SLOT1_DISABLED;

	platform_set_drvdata(pdev, slot1);
    pr_info("Started NTR Slot-1 driver");
	return 0;
}

static const struct of_device_id ntr_slot1_of_match[] = {
	{ .compatible = "nintendo,ntr-slot1" },
	{}
};
MODULE_DEVICE_TABLE(of, ntr_slot1_of_match);

static struct platform_driver ntr_slot1_driver = {
	.probe	= ntr_slot1_probe,

	.driver = {
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
        .of_match_table = ntr_slot1_of_match,
	},
};
module_platform_driver(ntr_slot1_driver);

MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" DRIVER_NAME);
