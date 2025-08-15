/* SPDX-License-Identifier: GPL-2.0-only */

.macro	addruart, rp, rv, tmp
	ldr	\rp, =CONFIG_DEBUG_UART_PHYS
	ldr	\rv, =CONFIG_DEBUG_UART_PHYS
.endm

.macro	senduart, rd, rx
	strb \rd, [\rx]
.endm

.macro	busyuart, rd, rx
.endm

.macro	waituartcts, rd, rx
.endm

.macro	waituarttxrdy, rd, rx
.endm
