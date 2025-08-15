// SPDX-License-Identifier: GPL-2.0+
/** Copyright 2024-2025 */

#include <linux/module.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/serial_core.h>
#include <linux/serial.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/io.h>

#define NTR_NOCASH_DEBUG_PUTC	((void*)0x4fffa1c)

static void
ntr_nocash_debugport_early_putc(struct uart_port *port, unsigned char ch)
{
	iowrite8(ch, NTR_NOCASH_DEBUG_PUTC);
}

static void
ntr_nocash_debugport_early_write(struct console *con, const char *s,  unsigned count)
{
	struct earlycon_device *dev = con->data;
	uart_console_write(&dev->port, s, count, ntr_nocash_debugport_early_putc);
}

static int __init
ntr_nocash_debugport_early_setup(struct earlycon_device *dev, const char *opt)
{
	dev->con->write = ntr_nocash_debugport_early_write;
	return 0;
}
EARLYCON_DECLARE(ntr_nocash, ntr_nocash_debugport_early_setup);

MODULE_DESCRIPTION("NO$GBA earlycon driver for NTR/TWL");
MODULE_LICENSE("GPL");
