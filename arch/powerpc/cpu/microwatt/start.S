/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2021 Paul Mackerras <paulus@ozlabs.org>,
 * IBM Corp.
 */

#include <config.h>
#include <ppc_asm.tmpl>
#include <version.h>

#include <asm/cache.h>
#include <asm/u-boot.h>

	.text
	.long	0x27051956		/* U-Boot Magic Number */
	.balign	4

	.globl	_start
_start:
	/* Get TOC pointer into r2 */
	bcl	20,31,$+4
0:	mflr	r2
	addis	r2, r2, (.TOC. - 0b)@ha
	addi	r2, r2, (.TOC. - 0b)@l

	/* Set up stack pointer and make initial stack frame */
	lis	r1, 0x0e00
	li	r0, 0
	stdu	r0, -32(r1)

	li	r9, -1
	clrldi	r9, r9, 33
	mtdec	r9

	bl	cpu_init_f
	nop

	li	r3, 0
	bl	board_init_f
	nop
	/* board_init_f doesn't return */
	b	.

_GLOBAL(reloc_and_go)
	mr	r1, r5
	li	r0, 0
	stdu	r0, -32(r1)
	bcl	20,31,$+4
1:	mflr	r9
	add	r9, r9, r6
	addi	r9, r9, 2f - 1b
	mtlr	r9
	blr			/* jump into the copy at 2f */
2:	add	r2, r2, r6
	mr	r31, r3
	mr	r30, r4
	mr	r3, r6
	bl	relocate
	nop
	mr	r3, r31
	mr	r4, r30
	bl	board_init_r
	nop
	b	.

#define PTREGS_OFFSET	(-288 - 48*8)

#define _NIP	(32 * 8)
#define _MSR	(33 * 8)
#define _CFAR	(34 * 8)
#define _CTR	(35 * 8)
#define _LR	(36 * 8)
#define _XER	(37 * 8)
#define _CR	(38 * 8)
#define _TRAP	(40 * 8)
#define _DAR	(41 * 8)
#define _DSISR	(42 * 8)

	/*
	 * This code gets copied to each interrupt vector
	 * by trap_init.
	 */
	.balign	32
	.globl	intr_handler
intr_handler:
	std	r0, PTREGS_OFFSET(r1)
	std	r3, PTREGS_OFFSET + 24(r1)
	mfctr	r3
	mfsprg0	r0
	mtctr	r0
	mfcfar	r0
	bctr		/* jump to handle_interrupt */
	nop

	.globl handle_interrupt
handle_interrupt:
	std	r1, PTREGS_OFFSET + 8(r1)
	std	r0, PTREGS_OFFSET + _CFAR(r1)
	std	r3, PTREGS_OFFSET + _CTR(r1)
	addi	r3, r1, PTREGS_OFFSET
	stdu	r1, PTREGS_OFFSET - 32(r1)
	std	r2, 16(r3)

	i = 4
	.rept	28
	std	i, i * 8(r3)
	i = i + 1
	.endr

	mfcr	r4
	std	r4, _CR(r3)
	mfsrr0	r5
	std	r5, _NIP(r3)
	mfsrr1	r6
	std	r6, _MSR(r3)
	mfcfar	r4
	clrrdi	r4, r4, 5
	std	r4, _TRAP(r3)
	mflr	r5
	std	r5, _LR(r3)
	mfxer	r6
	std	r6, _XER(r3)
	mfdar	r7
	std	r7, _DAR(r3)
	mfdsisr	r8
	std	r8, _DSISR(r3)

	bcl	20,31,$+4
0:	mflr	r2
	addis	r2, r2, (.TOC. - 0b)@ha
	addi	r2, r2, (.TOC. - 0b)@l

	bl	interrupt
	nop

	addi	r3, r1, 32
	ld	r4, _XER(r3)
	mtxer	r4
	ld	r5, _LR(r3)
	mtlr	r5
	ld	r6, _MSR(r3)
	mtsrr1	r6
	ld	r7, _NIP(r3)
	mtsrr0	r7
	ld	r8, _CR(r3)
	mtcr	r8
	ld	r9, _CTR(r3)
	mtctr	r9

	ld	r0, 0(r3)
	ld	r1, 8(r3)
	ld	r2, 16(r3)
	i = 4
	.rept	10
	ld	i, i * 8(r3)
	i = i + 1
	.endr

	ld	r3, 24(r3)
	rfid
