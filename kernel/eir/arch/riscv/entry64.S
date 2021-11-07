.global _start
_start:
	la sp, eirStackTop
	j eirMain

.section .bss
eirStackBase:
	.space 0x10000
eirStackTop:
