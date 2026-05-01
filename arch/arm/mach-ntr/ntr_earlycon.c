// SPDX-License-Identifier: GPL-2.0+

#include <linux/io.h>
#include <linux/module.h>
#include <linux/serial_core.h>

/** no$gba specific, other emulators also support it, no-op in hardware */
#define NTR_DEBUG_PUTC_IO	((void*)0x4fffa1c)

static void
ntr_earlycon_putc(struct uart_port *port, unsigned char ch)
{
	iowrite8(ch, NTR_DEBUG_PUTC_IO);
}

static void
ntr_earlycon_write(struct console *con, const char *s,  unsigned count)
{
	struct earlycon_device *dev = con->data;
	uart_console_write(&dev->port, s, count, ntr_earlycon_putc);
}

static int __init
ntr_earlycon_init(struct earlycon_device *dev, const char *opt)
{
	dev->con->write = ntr_earlycon_write;
	return 0;
}
EARLYCON_DECLARE(ntr, ntr_earlycon_init);

MODULE_DESCRIPTION("NO$GBA earlycon driver for NTR/TWL");
MODULE_LICENSE("GPL");
