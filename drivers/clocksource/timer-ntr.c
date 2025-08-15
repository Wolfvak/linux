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

#define NTR_TIMER_FALLBACK_FREQ	(33513982)

#define REG_TICKVAL_OFFSET(n)	(4 * (n))
#define REG_CONFIG_OFFSET(n)	((4 * (n)) + 2)

/** Prescaler selection */
#define NTR_TIMER_CONFIG_PRESCALE_1	(0 << 0)
#define NTR_TIMER_CONFIG_PRESCALE_64	(1 << 0)
#define NTR_TIMER_CONFIG_PRESCALE_256	(2 << 0)
#define NTR_TIMER_CONFIG_PRESCALE_1024	(3 << 0)

/**
 * If this bit is set, the timer will only
 * increment when the previous one overflows
 */
#define NTR_TIMER_CONFIG_COUNT_UP	BIT(2)

// Enable IRQ on Timer overflow
#define NTR_TIMER_CONFIG_IRQ_ENABLE	BIT(6)

// 1=Start/Count, 0=Stopped
#define NTR_TIMER_CONFIG_COUNT_ENABLE	BIT(7)

static struct {
	void __iomem *io;

	u16 clkevt_ticks;
	spinlock_t clkevt_lock;
	spinlock_t counter_lock;
} ntr_timer;

static void ntr_timer_stop(void)
{
	iowrite16(0, ntr_timer.io + REG_CONFIG_OFFSET(0));
}

static void ntr_timer_start(u16 ticks)
{
	unsigned long flags;
	static const u16 timer_control =
		NTR_TIMER_CONFIG_PRESCALE_1024 |
		NTR_TIMER_CONFIG_IRQ_ENABLE |
		NTR_TIMER_CONFIG_COUNT_ENABLE;

	spin_lock_irqsave(&ntr_timer.clkevt_lock, flags);
	void __iomem *io = ntr_timer.io;

	// disable, reload with the new counter value, enable again
	iowrite16(0, io + REG_CONFIG_OFFSET(0));
	iowrite16(0xFFFF - ticks, io + REG_TICKVAL_OFFSET(0));
	iowrite16(timer_control, io + REG_CONFIG_OFFSET(0));
	spin_unlock_irqrestore(&ntr_timer.clkevt_lock, flags);
}

static inline void ntr_clksrc_reset(void)
{
	static const u16 base_flags = NTR_TIMER_CONFIG_PRESCALE_1024 | NTR_TIMER_CONFIG_COUNT_ENABLE;
	static const u16 chained_flags = NTR_TIMER_CONFIG_COUNT_UP | NTR_TIMER_CONFIG_COUNT_ENABLE;

	void __iomem *io = ntr_timer.io + REG_TICKVAL_OFFSET(1);
	for (unsigned i = 0; i < 3; i++) {			/* stop and reset all the counters */
		iowrite16(0, io + REG_CONFIG_OFFSET(i));
		iowrite16(0, io + REG_TICKVAL_OFFSET(i));
	}
	iowrite16(chained_flags, io + REG_CONFIG_OFFSET(1));	/* start the chained counters */
	iowrite16(chained_flags, io + REG_CONFIG_OFFSET(2));
	iowrite16(base_flags, io + REG_CONFIG_OFFSET(0));	/* start the base counter*/
}

static inline u64 ntr_sched_clock_read(void)
{
	unsigned long flags;
	u16 b15_0, b31_16, b47_32, lo2;

	spin_lock_irqsave(&ntr_timer.counter_lock, flags);
	void __iomem *io = ntr_timer.io + REG_TICKVAL_OFFSET(1);
	do {
		b15_0	= ioread16(io + REG_TICKVAL_OFFSET(0));
		b31_16	= ioread16(io + REG_TICKVAL_OFFSET(1));
		b47_32	= ioread16(io + REG_TICKVAL_OFFSET(2));
		lo2	= ioread16(io + REG_TICKVAL_OFFSET(0));
	} while(b15_0 > lo2);
	spin_unlock_irqrestore(&ntr_timer.counter_lock, flags);

	return ((u64)b47_32 << 32) | ((u64)b31_16 << 16) | b15_0;
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
	ntr_timer_start(ntr_timer.clkevt_ticks);
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
	struct clk *clk;
	unsigned long rate;

	pr_info("starting NDS TIMER driver...\n");

	spin_lock_init(&ntr_timer.clkevt_lock);
	spin_lock_init(&ntr_timer.counter_lock);

	irq = irq_of_parse_and_map(np, 0);
	if (irq <= 0) {
		pr_err("failed to parse irq (%d)!\n", irq);
		return -EINVAL;
	}

	ntr_timer.io = of_iomap(np, 0);
	BUG_ON(IS_ERR_OR_NULL(ntr_timer.io));

	pr_debug("mapped registers @ %px\n", ntr_timer.io);

	clk = of_clk_get(np, 0);
	BUG_ON(IS_ERR_OR_NULL(clk));

	err = clk_prepare_enable(clk);
	if (err) {
		rate = NTR_TIMER_FALLBACK_FREQ;
		pr_err("failed to prepare clk (%d), falling back to default freq %ld\n", err, rate);
	} else {
		rate = clk_get_rate(clk);
	}

	// All clocks are prescaled by 1024
	rate /= 1024;

	// Reset TIMER0
	ntr_timer.clkevt_ticks = DIV_ROUND_CLOSEST(rate, HZ);
	ntr_timer_stop();

	// Set up TIMER1-3 as a clocksource
	ntr_clksrc_reset();
	clocksource_register_hz(&ntr_clksrc, rate);
	sched_clock_register(ntr_sched_clock_read, 48, rate);

	// Set up TIMER0 as a clockevent
	err = request_irq(irq, ntr_timer_irq, IRQF_TIMER | IRQF_IRQPOLL,
			  KBUILD_MODNAME "_clkevt", &ntr_clkevt);
	if (err) {
		pr_err("failed to request irq %d (%d)\n", irq, err);
	} else {
		clockevents_config_and_register(&ntr_clkevt,
			rate, 1, 0xFFFF);
	}

	pr_info("ready!\n");
	return err;
}

TIMER_OF_DECLARE(nintendo_ntr_timer, "nintendo,ntr-timer", ntr_timer_of_init);
