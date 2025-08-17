// SPDX-License-Identifier: GPL-2.0-or-later

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/clk.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/sched_clock.h>

#include <asm/mach/time.h>

/**
 * The Nintendo DS has 4 16bit timers running at ~33MHz.
 * Each can be independently controlled to be count-up/down,
 * prescale its counting by /64, /256 or /1024, and enable IRQs.
 * They also have a carry-over feature, where an overflow in
 * timer N will cause timer N+1 to increment.
 * This way wider (32/48/64bit) counters can be constructed.
 *
 * We use TIMER0 as an IRQ-capable clocksource, used for clk events.
 * TIMER1-3 are chained to build a software-only 48bit count-up clocksource.
 */

#define NTR_TIMER_FREQ	(33513982)

#define NTR_CLKEVT_FREQ	(NTR_TIMER_FREQ / 1024)
#define NTR_CLKSRC_FREQ	(NTR_TIMER_FREQ / 256)

#define REG_TICKVAL_OFFSET(n)	(4 * (n))
#define REG_CONFIG_OFFSET(n)	((4 * (n)) + 2)

#define NTR_TIMER_CONFIG_START	((3u << 0) |	/* prescaler = 1024 */ \
				 BIT(6) |	/* IRQ enabled */ \
				 BIT(7))	/* start counting*/

#define NTR_CLKSRC_CONFIG_START	((2u << 0) |	/* prescaler = 256 */ \
				 BIT(7))	/* start counting */

#define NTR_CLKSRC_CONFIG_CHAIN	(BIT(2) |	/* count-up */ \
				 BIT(7))	/* start counting */

static struct {
	void __iomem *io;
	spinlock_t clkevt_lock;
	spinlock_t clksrc_lock;
} ntr_timer;

static void ntr_timer_stop(void)
{
	iowrite16(0, ntr_timer.io + REG_CONFIG_OFFSET(0));
}

static void ntr_timer_start(u16 ticks)
{
	unsigned long flags;
	void __iomem *io = ntr_timer.io;
	spin_lock_irqsave(&ntr_timer.clkevt_lock, flags);
	iowrite32(0, io);				/* disable the timer */
	iowrite16(0xFFFF - ticks, io);			/* reload the new counter value */
	iowrite16(NTR_TIMER_CONFIG_START, io + 2);	/* restart the timer */
	spin_unlock_irqrestore(&ntr_timer.clkevt_lock, flags);
}

static inline void ntr_clksrc_reset(void)
{
	unsigned long flags;
	void __iomem *io = ntr_timer.io + 4;
	spin_lock_irqsave(&ntr_timer.clksrc_lock, flags);
	iowrite32(0, io + 0); /* fully disable the 3 sources */
	iowrite32(0, io + 4);
	iowrite32(0, io + 8);
	iowrite16(NTR_CLKSRC_CONFIG_CHAIN, io + REG_CONFIG_OFFSET(1));	/* configure the chained timers */
	iowrite16(NTR_CLKSRC_CONFIG_CHAIN, io + REG_CONFIG_OFFSET(2));
	iowrite16(NTR_CLKSRC_CONFIG_START, io + REG_CONFIG_OFFSET(0));	/* configure the low timer last */
	spin_unlock_irqrestore(&ntr_timer.clksrc_lock, flags);
}

static inline u64 ntr_sched_clock_read(void)
{
	unsigned long flags;
	u16 lo1, lo2;
	u32 hi;

	spin_lock_irqsave(&ntr_timer.clksrc_lock, flags);
	void __iomem *io = ntr_timer.io + 4;
	do {
		lo1	= ioread16(io + REG_TICKVAL_OFFSET(0));
		hi	= (u32)ioread16(io + REG_TICKVAL_OFFSET(1)) |
			  (u32)ioread16(io + REG_TICKVAL_OFFSET(2)) << 16;
		lo2	= ioread16(io + REG_TICKVAL_OFFSET(0));
	} while(lo2 < lo1);
	spin_unlock_irqrestore(&ntr_timer.clksrc_lock, flags);

	return (u64)hi << 16 | lo2;
}

static u64 ntr_clksrc_read(struct clocksource *c)
{
	return ntr_sched_clock_read();
}

static struct clocksource ntr_clksrc = {
	.name	= KBUILD_MODNAME "_clksrc",
	.rating	= 200,
	.read	= ntr_clksrc_read,
	.mask	= CLOCKSOURCE_MASK(48),
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS,
};

static int ntr_clkevt_set_next_event(unsigned long next, struct clock_event_device *evt)
{
	ntr_timer_start(next);
	return 0;
}

static int ntr_clkevt_set_state_shutdown(struct clock_event_device *evt)
{
	ntr_timer_stop();
	return 0;
}

static int ntr_clkevt_set_state_periodic(struct clock_event_device *evt)
{
	ntr_timer_start(NTR_CLKEVT_FREQ / HZ);
	return 0;
}

static struct clock_event_device ntr_clkevt = {
	.name			= KBUILD_MODNAME "_clkevt",
	.features		= CLOCK_EVT_FEAT_PERIODIC,
	.rating			= 300,
	.set_next_event		= ntr_clkevt_set_next_event,
	.set_state_shutdown	= ntr_clkevt_set_state_shutdown,
	.set_state_periodic	= ntr_clkevt_set_state_periodic,
};

static irqreturn_t ntr_timer_irq(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;
	evt->event_handler(evt);
	return IRQ_HANDLED;
}

static int __init ntr_timer_of_init(struct device_node *np)
{
	int irq, err;

	pr_info("starting NDS TIMER driver...\n");

	spin_lock_init(&ntr_timer.clkevt_lock);
	spin_lock_init(&ntr_timer.clksrc_lock);

	irq = irq_of_parse_and_map(np, 0);
	if (irq <= 0) {
		pr_err("failed to parse irq (%d)!\n", irq);
		return -EINVAL;
	}

	ntr_timer.io = of_iomap(np, 0);
	BUG_ON(IS_ERR_OR_NULL(ntr_timer.io));

	pr_debug("mapped registers @ %px\n", ntr_timer.io);

	// Reset TIMER0
	ntr_timer_stop();

	// Set up TIMER1-3 as a clocksource
	ntr_clksrc_reset();
	clocksource_register_hz(&ntr_clksrc, NTR_CLKSRC_FREQ);
	sched_clock_register(ntr_sched_clock_read, 48, NTR_CLKSRC_FREQ);

	// Set up TIMER0 as a clockevent
	err = request_irq(irq, ntr_timer_irq, IRQF_TIMER | IRQF_IRQPOLL,
			  KBUILD_MODNAME "_clkevt", &ntr_clkevt);
	if (err) {
		pr_err("failed to request irq %d (%d)\n", irq, err);
	} else {
		clockevents_config_and_register(&ntr_clkevt,
			NTR_CLKEVT_FREQ, 0, 0xFFFF);
	}

	pr_info("ready!\n");
	return err;
}

TIMER_OF_DECLARE(nintendo_ntr_timer, "nintendo,ntr-timer", ntr_timer_of_init);
