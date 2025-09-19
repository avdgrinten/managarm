.global _start
.section .text.start
_start:
	# Note: We can only use "lla", not "la" before eirRelocate!

	lla sp, eirStackTop

	lla t0, eirDtbPtr
	sd a1, (t0)

#if defined(EIR_PIE)
	call eirRelocate
#endif
	call eirRunConstructors
	call eirMain
	unimp

#ifndef __clang__
	.section .note.GNU-stack,"",%progbits
#endif
