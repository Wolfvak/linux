// SPDX-License-Identifier: GPL-2.0-or-later

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bitops.h>
#include <linux/irq.h>
#include <linux/io.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/cpu.h>

#include <asm/exception.h>
#include <asm/mach/irq.h>

#define NTR_IRQC_NR_IRQS	32

#define REG_IME	(0x00)
#define REG_IE	(0x08)
#define REG_IF	(0x0C)

static struct {
	void __iomem *io;
	struct irq_domain *irqd;
	// TODO: should we do locking here?
} ntr_irqc;

static inline void ntr_irqc_rmw_enabled(void __iomem *io, u32 set, u32 clr)
{
	u32 en = ioread32(io + REG_IE);
	en = (en | set) & ~clr;
	iowrite32(en, io + REG_IE);
}

static void ntr_irqc_ack(struct irq_data *d)
{
	void __iomem *io = irq_data_get_irq_chip_data(d);
	iowrite32(BIT(irqd_to_hwirq(d)), io + REG_IF);
}

static void ntr_irqc_mask(struct irq_data *d)
{
	void __iomem *io = irq_data_get_irq_chip_data(d);
	ntr_irqc_rmw_enabled(io, 0, BIT(irqd_to_hwirq(d)));
}

static void ntr_irqc_unmask(struct irq_data *d)
{
	void __iomem *io = irq_data_get_irq_chip_data(d);
	ntr_irqc_rmw_enabled(io, BIT(irqd_to_hwirq(d)), 0);
}

static void ntr_irqc_mask_ack(struct irq_data *d)
{
	u32 mask = BIT(irqd_to_hwirq(d));
	void __iomem *io = irq_data_get_irq_chip_data(d);

	iowrite32(ioread32(io + REG_IE) & ~mask, io + REG_IE);
	iowrite32(mask, io + REG_IF);
}

static struct irq_chip ntr_irqc_chip = {
	.name		= KBUILD_MODNAME,
	.irq_ack	= ntr_irqc_ack,
	.irq_mask	= ntr_irqc_mask,
	.irq_unmask	= ntr_irqc_unmask,
	.irq_mask_ack	= ntr_irqc_mask_ack,
};

static asmlinkage
void __exception_irq_entry ntr_irqc_handle_irq(struct pt_regs *regs)
{
	int irq;
	u32 pending;
	void __iomem *reg_if;
	struct irq_domain *irqd;

	reg_if = ntr_irqc.io + REG_IF;
	irqd = ntr_irqc.irqd;
	pending = ioread32(reg_if);

	do {
		while(pending != 0) {
			irq = ffs(pending) - 1;
			pending &= ~BIT(irq);

			if (generic_handle_domain_irq(irqd, irq) != 0) {
				pr_err("spurious interrupt %d", irq);
				iowrite32(1u << irq, reg_if);
			}
		}

		pending = ioread32(reg_if);
	} while(pending != 0);
}

static int ntr_irqc_domain_map(struct irq_domain *d, unsigned int irq,
				irq_hw_number_t hwirq)
{
	irq_set_chip_data(irq, ntr_irqc.io);
	irq_set_chip_and_handler(irq, &ntr_irqc_chip, handle_edge_irq);
	irq_set_probe(irq);
	return 0;
}

static void ntr_irqc_domain_unmap(struct irq_domain *d, unsigned int irq)
{
	irq_set_chip_and_handler(irq, NULL, handle_bad_irq);
	irq_set_chip_data(irq, NULL);
}

static const struct irq_domain_ops ntr_irqc_domain_ops = {
	.map = ntr_irqc_domain_map,
	.unmap = ntr_irqc_domain_unmap,
	.xlate = irq_domain_xlate_onecell,
};

static int __init ntr_irqc_of_init(struct device_node *node,
				struct device_node *parent)
{
	pr_info("starting nds irq controller driver...\n");

	ntr_irqc.io = of_iomap(node, 0);
	BUG_ON(ntr_irqc.io == NULL);

	pr_debug("mapped registers @ %px\n", ntr_irqc.io);

	// mask IME, acknowledge & mask interrupts, unmask IME
	iowrite32(0, ntr_irqc.io + REG_IME);

	iowrite32(0, ntr_irqc.io + REG_IE);
	iowrite32(~0, ntr_irqc.io + REG_IF);

	iowrite32(1, ntr_irqc.io + REG_IME);

	ntr_irqc.irqd = irq_domain_create_simple(of_fwnode_handle(node),
						 NTR_IRQC_NR_IRQS, 0,
						 &ntr_irqc_domain_ops, NULL);
	pr_debug("mapped %d interrupts\n", ntr_irqc.irqd->mapcount);

	set_handle_irq(ntr_irqc_handle_irq);
	pr_info("ready!\n");
	return 0;
}

IRQCHIP_DECLARE(nintendo_ntr_irqc, "nintendo,ntr-irqc", ntr_irqc_of_init);
