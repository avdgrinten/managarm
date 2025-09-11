.global eirExcVectors
.align 11
eirExcVectors:
	// EL1t
	// Synchronous
.align 7
	mov x0, #0
	b eirCommonHandler

	// IRQ
.align 7
	mov x0, #1
	b eirCommonHandler

	// FIQ
.align 7
	mov x0, #2
	b eirCommonHandler

	// SError
.align 7
	mov x0, #3
	b eirCommonHandler


	// EL1h
	// Synchronous
.align 7
	mov x0, #0
	b eirCommonHandler

	// IRQ
.align 7
	mov x0, #1
	b eirCommonHandler

	// FIQ
.align 7
	mov x0, #2
	b eirCommonHandler

	// SError
.align 7
	mov x0, #3
	b eirCommonHandler

eirCommonHandler:
	// For VHE, it does not really matter if we read EL1 or EL2 registers.
	// However, we may want to install exception handlers in early boot code in EL2.
	// Hence, switch based on the current EL.
	mrs x1, currentel
	ubfx x1, x1, #2, #2
	cmp x1, #1
	b.eq 1f

	// EL2
	// Disable MMU
	mrs x1, sctlr_el2
	and x1, x1, #0xFFFFFFFFFFFFFFFE
	msr sctlr_el2, x1
	isb

	adrp x1, eirStackTop
	add x1, x1, :lo12:eirStackTop
	mov sp, x1
	mrs x1, esr_el2
	mrs x2, elr_el2
	mrs x3, spsr_el2
	mrs x4, far_el2
	b eirExceptionHandler

1:
	// EL1
	// Disable MMU
	mrs x1, sctlr_el1
	and x1, x1, #0xFFFFFFFFFFFFFFFE
	msr sctlr_el1, x1
	isb

	adrp x1, eirStackTop
	add x1, x1, :lo12:eirStackTop
	mov sp, x1
	mrs x1, esr_el1
	mrs x2, elr_el1
	mrs x3, spsr_el1
	mrs x4, far_el1
	b eirExceptionHandler

#ifndef __clang__
	.section .note.GNU-stack,"",%progbits
#endif
