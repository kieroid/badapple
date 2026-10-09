/* SPDX-License-Identifier: GPL-2.0+ OR MIT
 * Copyright (c) 2026 kiki <kieran@lavato.net>
 *
 * Entry shim so PongoOS can be started by the SecureROM on an A12.
 *
 * Since PongoOS 2.6.0 it runs from SRAM and relocates itself there out of
 * whatever buffer it was loaded into. Its entry takes the SRAM base in SP,
 * and falls back to 0x180000000 when that is zero, which its own comment
 * says is right for every pre-A12 device and nothing later:
 *
 *     x30     = next-stage binary (iBoot/KPP/XNU)
 *      x0     = argument to next stage
 *      sp_el1 = sram base (inbound only, nullable and fully-optional pre-A12)
 *
 * The jig firmware used to hand PongoOS that from a payload running inside
 * iBoot. We start it straight from the pwned bootrom instead, and the ROM
 * sets up none of it, so PongoOS copied itself to the A11 address and the
 * watchdog reset the phone back to DFU with a black screen.
 *
 * On T8020 the SecureROM's SRAM is at 0x19c000000: its stack, trampoline and
 * page tables all live at 0x19c0xxxxx, which is where the exploit's own
 * offsets point.
 *
 * x0 has to be non-zero. PongoOS reads it as the next stage argument, and
 * skips zeroing its own bss when it is zero. Coming from the bootrom that bss
 * is whatever was left in SRAM, so the zeroing has to happen. x30 is the next
 * stage to hand control back to, and there is none: the whole point is that
 * iBoot never runs. PongoOS loads m1n1 over USB and bootm jumps to it.
 *
 * Prepend to Pongo.bin, keeping this a multiple of 16 bytes so PongoOS's own
 * copy loop stays aligned, and the result is what the ROM is told to boot.
 */

#define T8020_SRAM_BASE_HI	0x9c00	/* bits 31:16 of 0x19c000000 */
#define T8020_SRAM_BASE_TOP	0x1	/* bits 47:32 */

.text
.globl _start
_start:
	msr	spsel, #1
	movz	x5, #T8020_SRAM_BASE_HI, lsl #16
	movk	x5, #T8020_SRAM_BASE_TOP, lsl #32
	mov	sp, x5

	/*
	 * With SPSel set the write above lands in the SP of whatever exception
	 * level the ROM entered at, so at EL2 or EL3 it is not the SP_EL1 that
	 * PongoOS reads. Write that one too. The register is only reachable
	 * from EL2 and up, so skip it when we are already at EL1.
	 */
	mrs	x6, CurrentEL
	cmp	x6, #(1 << 2)
	b.eq	Lsp_done
	msr	sp_el1, x5
Lsp_done:
	mov	x0, #1
	mov	x30, #0
	b	Lpongo

	.balign 16
Lpongo:
