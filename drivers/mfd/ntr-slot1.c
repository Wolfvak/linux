// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#define DRIVER_NAME "ntr_slot1"

#include <linux/io.h>
#include <linux/of.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/mfd/core.h>

#define NTR_SLOT1_CNT_SPIAUX    BIT(13) /* bit13=1 <- select Serial/SPI, bit14=0 <- disable IRQs */
#define NTR_SLOT1_CNT_CARTROM   BIT(14) /* bit13=0 <- select Parallel/ROM, bit14=1 <- enable IRQs */
#define NTR_SLOT1_CNT_ENABLE    BIT(15) /* enable interface */

struct ntr_slot1 {
    void __iomem *io;
    struct mutex lock;
};

static const struct mfd_cell ntr_slot1_cells[] = {
    MFD_CELL_OF("ntr_slot1_auxspi", NULL, NULL, 0, 0, "nintendo,ntr-slot1-auxspi"),
    MFD_CELL_OF("ntr_slot1_cart", NULL, NULL, 0, 0, "nintendo,ntr-slot1-cart"),
};

void __iomem *ntr_slot1_configure_spi(struct ntr_slot1 *slot1)
{
    if (!slot1)
        return ERR_PTR(-EINVAL);

    if (!mutex_trylock(&slot1->lock))
        return ERR_PTR(-EBUSY);
    iowrite16(NTR_SLOT1_CNT_SPIAUX | NTR_SLOT1_CNT_ENABLE, slot1->io);
    return slot1->io;
}

int ntr_slot1_configure_cart(struct ntr_slot1 *slot1)
{
    if (!slot1)
        return -EINVAL;

    if (!mutex_trylock(&slot1->lock))
        return -EBUSY;
    iowrite16(NTR_SLOT1_CNT_CARTROM | NTR_SLOT1_CNT_ENABLE, slot1->io);
    return 0;
}

int ntr_slot1_release(struct ntr_slot1 *slot1)
{
    if (!slot1)
        return -EINVAL;

    /* disable any active interface */
    iowrite16(0, slot1->io);
    mutex_unlock(&slot1->lock);
    return 0;
}

static int ntr_slot1_probe(struct platform_device *pdev)
{
    struct ntr_slot1 *slot1;

    pr_info("starting up Slot1 driver\n");

    slot1 = devm_kmalloc(&pdev->dev, sizeof(*slot1), GFP_KERNEL);
    if (!slot1) {
        pr_err("failed to allocate driver memory\n");
        return -ENOMEM;
    }

    slot1->io = devm_platform_ioremap_resource(pdev, 0);
    if (IS_ERR(slot1->io)) {
        pr_err("failed to remap IO memory\n");
        return -ENOMEM;
    }

    mutex_init(&slot1->lock);
    iowrite16(0, slot1->io + 0);

    platform_set_drvdata(pdev, slot1);

    pr_info("registering devices...\n");
    return devm_mfd_add_devices(&pdev->dev, PLATFORM_DEVID_AUTO, ntr_slot1_cells, 2, NULL, 0, NULL);
}

static const struct of_device_id ntr_slot1_of_match[] = {
    { .compatible = "nintendo,ntr-slot1" },
};
MODULE_DEVICE_TABLE(of, ntr_slot1_of_match);

static struct platform_driver ntr_slot1_mfd = {
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = ntr_slot1_of_match,
    },
    .probe = ntr_slot1_probe,
};
module_platform_driver(ntr_slot1_mfd);
