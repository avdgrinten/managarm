.section .bss
.align 16
.global eirStackTop
eirStackBottom:
	.skip 0x100000 // reserve 1 MiB for the stack
eirStackTop:

.section .text
.global eirEnterKernel
eirEnterKernel:
	// x0 -> Thor entry pointer
	// x1 -> Thor stack pointer
	mov sp, x1
	br x0

#ifndef __clang__
	.section .note.GNU-stack,"",%progbits
#endif
