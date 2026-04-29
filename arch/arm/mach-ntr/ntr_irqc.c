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
	struct irq_chip_generic *gc;
} ntr_irqc;


static asmlinkage
void __exception_irq_entry ntr_irqc_handle_irq(struct pt_regs *regs)
{
	int irq;
	u32 pending;
	struct irq_domain *irqd = ntr_irqc.irqd;
	void __iomem *reg_if = ntr_irqc.io + REG_IF;

	do {
		pending = ioread32(reg_if);

		while(pending != 0) {
			irq = ffs(pending) - 1;
			pending &= ~BIT(irq);
			generic_handle_domain_irq(irqd, irq);
		}
	} while(pending != 0);
}

static int __init ntr_irqc_of_init(struct device_node *node,
				struct device_node *parent)
{
	int irq_base;
	struct irq_chip_generic *gc;

	pr_info("starting nds irq controller driver...\n");

	ntr_irqc.io = of_iomap(node, 0);
	BUG_ON(ntr_irqc.io == NULL);

	pr_debug("mapped registers @ %px\n", ntr_irqc.io);

	/* mask IME, acknowledge & mask interrupts, unmask IME */
	iowrite32(0, ntr_irqc.io + REG_IME);
	iowrite32(0, ntr_irqc.io + REG_IE);
	iowrite32(~0, ntr_irqc.io + REG_IF);
	iowrite32(1, ntr_irqc.io + REG_IME);

	irq_base = irq_alloc_descs(-1, 0, NTR_IRQC_NR_IRQS, numa_node_id());

	/* allocate a generic irqchip for the controller */
	gc = irq_alloc_generic_chip(KBUILD_MODNAME, 2, irq_base, ntr_irqc.io, NULL);
	ntr_irqc.gc = gc;
	BUG_ON(gc == NULL);

	gc->private = &ntr_irqc;
	gc->chip_types[0].handler = handle_level_irq;
	gc->chip_types[0].type = IRQ_TYPE_LEVEL_HIGH;
	gc->chip_types[1].handler = handle_edge_irq;
	gc->chip_types[1].type = IRQ_TYPE_EDGE_RISING;

	/* configure the chiptypes for LEVEL and EDGE interrupts */
	for (unsigned i = 0; i < 2; i++) {
		struct irq_chip_type *ct = &gc->chip_types[i];
		ct->regs.mask = REG_IE;
		ct->regs.ack = REG_IF;
		ct->chip.irq_ack = irq_gc_ack_set_bit;
		ct->chip.irq_mask = irq_gc_mask_clr_bit;
		ct->chip.irq_unmask = irq_gc_mask_set_bit;
	}

	irq_setup_generic_chip(gc, IRQ_MSK(NTR_IRQC_NR_IRQS), IRQ_GC_INIT_MASK_CACHE, IRQ_NOREQUEST, 0);

	ntr_irqc.irqd = irq_domain_create_legacy(of_fwnode_handle(node),
						 NTR_IRQC_NR_IRQS, irq_base, 0,
						 &irq_domain_simple_ops, &ntr_irqc);

	pr_debug("mapped %d interrupts\n", ntr_irqc.irqd->mapcount);

	set_handle_irq(ntr_irqc_handle_irq);
	pr_info("ready!\n");
	return 0;
}

IRQCHIP_DECLARE(nintendo_ntr_irqc, "nintendo,ntr-irqc", ntr_irqc_of_init);
