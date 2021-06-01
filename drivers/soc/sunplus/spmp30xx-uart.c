// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright 2021 Santiago Herrera
 */

#include <linux/io.h>
#include <linux/of.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/serial.h>
#include <linux/serial_core.h>

#define REG_UART_FIFO	(0x02)
#define REG_UART_STATUS	(0x0A)

#define UART_TX_BUSY	(0x02)

static void spmp30xx_console_early_putchar(struct uart_port *port, int ch)
{
	while(ioread8(port->membase + REG_UART_STATUS) & UART_TX_BUSY)
		cpu_relax();

	iowrite8(ch, port->membase + REG_UART_FIFO);
}

static void spmp30xx_console_early_write(struct console *con, const char *s,
					unsigned count)
{
	struct earlycon_device *edev = con->data;
	uart_console_write(&edev->port, s, count,
			spmp30xx_console_early_putchar);
}

static int __init spmp30xx_earlycon_setup(struct earlycon_device *edev,
					const char *opt)
{
	if (!edev->port.membase)
		return -ENODEV;
	edev->con->write = spmp30xx_console_early_write;
	return 0;
}

OF_EARLYCON_DECLARE(ec_spmp30xx,
		"sunplus,spmp30xx-uart", spmp30xx_earlycon_setup);

MODULE_AUTHOR("Santiago Herrera");
MODULE_DESCRIPTION("Sunplus SPMP30XX earlycon driver");
MODULE_LICENSE("GPL");
