// SPDX-License-Identifier: GPL-2.0-or-later

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/clk.h>
#include <linux/init.h>
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
 * TIMER0 is configured as a /64 prescaled, IRQ-capable clock events
 * source, yielding a resolution of ~1.9us and a maximum idle time of ~125ms.
 *
 * TIMER1-3 are chained to build a /256 prescaled, software-only 48bit
 * clocksource counter with a resolution of ~7.63us and a wraparound every ~68 years.
 */

#define NTR_TIMER_FREQ	(33513982)

#define NTR_CLKEVT_FREQ	(NTR_TIMER_FREQ / 64)
#define NTR_CLKSRC_FREQ	(NTR_TIMER_FREQ / 256)

/* Clock event (timer + IRQ) config: prescaler = 64, IRQ enabled, start */
#define NTR_CLKEVT_CONFIG_START	((1u << 0) | BIT(6) | BIT(7))
/* Clock source (48bit counter) low config: prescaler = 256, IRQ disabled, start */
#define NTR_CLKSRC_CONFIG_START	((2u << 0) | BIT(7))
/* Clock source (48bit counter) chained config: enable count-up, IRQ disabled, start*/
#define NTR_CLKSRC_CONFIG_CHAIN	(BIT(2) | BIT(7))

#define TIMER_VAL_OFFS(tmr)	((tmr) * 4)
#define TIMER_CNT_OFFS(tmr)	(TIMER_VAL_OFFS(tmr) + 2)

static struct {
	void __iomem *io;
	unsigned next_ticks;
} ntr_timer;

static void __ntr_timer_set_control(void __iomem *io, unsigned timer, u16 control) {
	iowrite16(control, io + TIMER_CNT_OFFS(timer));
}

static void __ntr_timer_set_count(void __iomem *io, unsigned timer, u16 count) {
	iowrite16(count, io + TIMER_CNT_OFFS(timer));
}

static u16 __ntr_timer_get_count(void __iomem *io, unsigned timer) {
	return ioread16(io + TIMER_VAL_OFFS(timer));
}

static void __ntr_timer_reset(void __iomem *io, unsigned timer) {
	iowrite32(0, io + TIMER_VAL_OFFS(timer));
}

static inline u64 ntr_sched_clock_read(void) {
	u16 count[2][3];

	void __iomem *io = ntr_timer.io;
	while (true) {
		/**
		 * Atomic reads are impossible because each 16bit counter is in a separate 32bit word.
		 * To work around this issue and prevent rollbacks we read the timers twice and
		 * only accept the result if the second read is _strictly_ in the future:
		 * - the low 16bits of the second read are >= the low 16bits of the first read
		 * - bits 16-47 are the same in both reads
		 **/
		for (unsigned i = 0; i < 2; i++) {
			for (unsigned t = 0; t < 3; t++) {
				count[i][t] = __ntr_timer_get_count(io, t + 1);
			}
		}

		if ((count[0][0] <= count[1][0]) &&
		    (count[0][1] == count[1][1]) &&
		    (count[0][2] == count[1][2]))
			break; /* passes all conditions, break out and return the second read */
	}

	return ((u64)count[1][2] << 32) | ((u32)count[1][1] << 16) | count[1][0];
}

static u64 ntr_clksrc_read(struct clocksource *c) {
	return ntr_sched_clock_read();
}

static struct clocksource ntr_clksrc = {
	.name	= KBUILD_MODNAME,
	.rating	= 200,
	.read	= ntr_clksrc_read,
	.mask	= CLOCKSOURCE_MASK(48),
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS,
};

static int ntr_clkevt_set_next_event(unsigned long next, struct clock_event_device *evt) {
	__ntr_timer_set_count(ntr_timer.io, 0, 0xFFFF - next);
	return 0;
}

static int ntr_clkevt_set_state_shutdown(struct clock_event_device *evt) {
	__ntr_timer_set_control(ntr_timer.io, 0, 0);
	return 0;
}

static int ntr_clkevt_set_state_periodic(struct clock_event_device *evt) {
	__ntr_timer_set_control(ntr_timer.io, 0, 0);
	__ntr_timer_set_count(ntr_timer.io, 0, 0xFFFF - (NTR_CLKEVT_FREQ / HZ));
	__ntr_timer_set_control(ntr_timer.io, 0, NTR_CLKEVT_CONFIG_START);
	return 0;
}

static struct clock_event_device ntr_clkevt = {
	.name			= KBUILD_MODNAME,
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

	pr_info("Starting NDS TIMER driver...\n");

	irq = irq_of_parse_and_map(np, 0);
	if (irq <= 0) {
		pr_err("failed to parse irq (%d)!\n", irq);
		return -EINVAL;
	}

	ntr_timer.io = of_iomap(np, 0);
	BUG_ON(IS_ERR_OR_NULL(ntr_timer.io));

	/* Reset all the timers */
	for (unsigned i = 0; i < 4; i++)
		__ntr_timer_reset(ntr_timer.io, i);

	/* Set up and register TIMER1-3 as a clocksource */
	/* fully reset and reconfigure the 3 chained timers */
	for (unsigned i = 1; i < 4; i++)
		__ntr_timer_reset(ntr_timer.io, i);
	for (unsigned i = 3; i > 0; i--)
		__ntr_timer_set_control(ntr_timer.io, i, i == 1 ? NTR_CLKSRC_CONFIG_START : NTR_CLKSRC_CONFIG_CHAIN);

	clocksource_register_hz(&ntr_clksrc, NTR_CLKSRC_FREQ);
	sched_clock_register(ntr_sched_clock_read, 48, NTR_CLKSRC_FREQ);

	/* Register TIMER0 as a clockevent */
	err = request_irq(irq, ntr_timer_irq, IRQF_TIMER | IRQF_IRQPOLL,
			  KBUILD_MODNAME "_clkevt", &ntr_clkevt);
	if (err) {
		pr_err("failed to request irq %d (%d)\n", irq, err);
	} else {
		clockevents_config_and_register(&ntr_clkevt,
			NTR_CLKEVT_FREQ, 0, 0xFFFF);
		pr_info("loaded\n");
	}
	return err;
}

TIMER_OF_DECLARE(nintendo_ntr_timer, "nintendo,ntr-timer", ntr_timer_of_init);
