#include <thor-internal/arch/asm.h>

# executor struct fields
.set .L_executorImagePtr, 0x00

# fields of the state structs
.set .L_imageRax, 0x0
.set .L_imageRbx, 0x8
.set .L_imageRcx, 0x10
.set .L_imageRdx, 0x18
.set .L_imageRsi, 0x20
.set .L_imageRdi, 0x28
.set .L_imageRbp, 0x30

.set .L_imageR8, 0x38
.set .L_imageR9, 0x40
.set .L_imageR10, 0x48
.set .L_imageR11, 0x50
.set .L_imageR12, 0x58
.set .L_imageR13, 0x60
.set .L_imageR14, 0x68
.set .L_imageR15, 0x70

.set .L_imageRip, 0x78
.set .L_imageCs, 0x80
.set .L_imageRflags, 0x88
.set .L_imageRsp, 0x90
.set .L_imageSs, 0x98
.set .L_imageClientFs, 0xA0
.set .L_imageClientGs, 0xA8
.set .L_imageIplState, 0xB0

.set .L_imageFxSave, 0xC0

# Offset of the error code / IRQ slot index within the Frame struct.
.set .L_frameCode, 0x80

.set .L_msrIndexFsBase, 0xC0000100
.set .L_msrIndexGsBase, 0xC0000101
.set .L_msrIndexKernelGsBase, 0xC0000102

# Emit debugging information + unwind tables.
.cfi_sections .eh_frame, .debug_frame

# ---------------------------------------------------------
# Fault stubs
# ---------------------------------------------------------

.set .L_typeFaultNoCode, 1
.set .L_typeFaultWithCode, 2
.set .L_typeCall, 3

.macro MAKE_FAULT_STUB type, name, number=0
.section .text.stubs
.global \name
\name:
	# This is a "signal frame", i.e., the return address must *not* be adjust to find
	# a call instruction that invoked this function.
	.cfi_startproc simple
	.cfi_signal_frame
	.cfi_def_cfa %rsp, 0
	.cfi_offset %rip, 0
	.cfi_offset %rsp, 24

	# If there is no error code we push a fake one to keep the image struct intact.
.if \type != .L_typeFaultWithCode
	push $0
.endif
	# Account for the error code (even if it is pushed by the CPU).
	.cfi_adjust_cfa_offset 8

	# Swap GS if we interrupted user-space.
	testl $3, 16(%rsp)
	jz 1f
	swapgs
1:

	push %rbp
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %rbp, 0
	push %r15
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %r15, 0
	push %r14
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %r14, 0
	push %r13
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %r13, 0
	push %r12
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %r12, 0
	push %r10
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %r10, 0
	push %r9
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %r9, 0
	push %r8
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %r8, 0
	push %rsi
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %rsi, 0
	push %rdi
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %rdi, 0
	push %rdx
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %rdx, 0
	push %rax
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %rax, 0
	push %r11
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %r11, 0
	push %rcx
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %rcx, 0
	push %rbx
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %rbx, 0

	# Reserve space for iplState.
	sub $8, %rsp
	.cfi_adjust_cfa_offset 8

	mov %rsp, %rdi
	mov $\number, %rsi
	xor %ebp, %ebp
	cld
	call onPlatformFault

	add $8, %rsp
	.cfi_adjust_cfa_offset -8

	pop %rbx
	.cfi_adjust_cfa_offset -8
	.cfi_restore %rbx
	pop %rcx
	.cfi_adjust_cfa_offset -8
	.cfi_restore %rcx
	pop %r11
	.cfi_adjust_cfa_offset -8
	.cfi_restore %r11
	pop %rax
	.cfi_adjust_cfa_offset -8
	.cfi_restore %rax
	pop %rdx
	.cfi_adjust_cfa_offset -8
	.cfi_restore %rdx
	pop %rdi
	.cfi_adjust_cfa_offset -8
	.cfi_restore %rdi
	pop %rsi
	.cfi_adjust_cfa_offset -8
	.cfi_restore %rsi
	pop %r8
	.cfi_adjust_cfa_offset -8
	.cfi_restore %r8
	pop %r9
	.cfi_adjust_cfa_offset -8
	.cfi_restore %r9
	pop %r10
	.cfi_adjust_cfa_offset -8
	.cfi_restore %r10
	pop %r12
	.cfi_adjust_cfa_offset -8
	.cfi_restore %r12
	pop %r13
	.cfi_adjust_cfa_offset -8
	.cfi_restore %r13
	pop %r14
	.cfi_adjust_cfa_offset -8
	.cfi_restore %r14
	pop %r15
	.cfi_adjust_cfa_offset -8
	.cfi_restore %r15
	pop %rbp
	.cfi_adjust_cfa_offset -8
	.cfi_restore %rbp

	# Restore GS.
	testl $3, 16(%rsp)
	jz 1f
	swapgs
1:

	add $8, %rsp
	.cfi_adjust_cfa_offset -8
	iretq
	.cfi_endproc
.endm

MAKE_FAULT_STUB .L_typeFaultNoCode, faultStubDivideByZero, 0
MAKE_FAULT_STUB .L_typeFaultNoCode, faultStubDebug, 1
MAKE_FAULT_STUB .L_typeFaultNoCode, faultStubBreakpoint, 3
MAKE_FAULT_STUB .L_typeFaultNoCode, faultStubOverflow, 4
MAKE_FAULT_STUB .L_typeFaultNoCode, faultStubBound, 5
MAKE_FAULT_STUB .L_typeFaultNoCode, faultStubOpcode, 6
MAKE_FAULT_STUB .L_typeFaultNoCode, faultStubNoFpu, 7
MAKE_FAULT_STUB .L_typeFaultWithCode, faultStubDouble, 8
MAKE_FAULT_STUB .L_typeFaultNoCode, faultStub9, 9
MAKE_FAULT_STUB .L_typeFaultWithCode, faultStubInvalidTss, 10
MAKE_FAULT_STUB .L_typeFaultWithCode, faultStubSegment, 11
MAKE_FAULT_STUB .L_typeFaultWithCode, faultStubStack, 12
MAKE_FAULT_STUB .L_typeFaultWithCode, faultStubProtection, 13
MAKE_FAULT_STUB .L_typeFaultWithCode, faultStubPage, 14
MAKE_FAULT_STUB .L_typeFaultNoCode, faultStub15, 15
MAKE_FAULT_STUB .L_typeFaultNoCode, faultStubFpuException, 16
MAKE_FAULT_STUB .L_typeFaultWithCode, faultStubAlignment, 17
MAKE_FAULT_STUB .L_typeFaultNoCode, faultStubMachineCheck, 18
MAKE_FAULT_STUB .L_typeFaultNoCode, faultStubSimdException, 19

# ---------------------------------------------------------
# IRQ stubs
# ---------------------------------------------------------

# Each IRQ slot gets a fixed-size stub that pushes its slot index in place of
# the error code and continues at the common IRQ entry.
.section .text.stubs
.balign THOR_IRQ_STUB_SIZE
.global thorRtIrqStubs
thorRtIrqStubs:
.rept THOR_NUM_IRQ_SLOTS
0:
	pushq $\+
	jmp thorRtIrqCommon
	# Pad with int3 so that a stray jump into the padding traps.
	# The fill count also errors out if a stub ever outgrows the stride.
	.fill 0b + THOR_IRQ_STUB_SIZE - ., 1, 0xCC
.endr

thorRtIrqCommon:
	# Swap GS if we interrupted user-space.
	testl $3, 16(%rsp)
	jz 1f
	swapgs
1:

	push %rbp
	push %r15
	push %r14
	push %r13
	push %r12
	push %r10
	push %r9
	push %r8
	push %rsi
	push %rdi
	push %rdx
	push %rax
	push %r11
	push %rcx
	push %rbx

	# Reserve space for iplState.
	sub $8, %rsp

	mov %rsp, %rdi
	mov .L_frameCode(%rsp), %rsi
	xor %ebp, %ebp
	cld
	call onPlatformIrq

	add $8, %rsp

	pop %rbx
	pop %rcx
	pop %r11
	pop %rax
	pop %rdx
	pop %rdi
	pop %rsi
	pop %r8
	pop %r9
	pop %r10
	pop %r12
	pop %r13
	pop %r14
	pop %r15
	pop %rbp

	# Restore GS.
	testl $3, 16(%rsp)
	jz 1f
	swapgs
1:

	add $8, %rsp
	iretq

.macro MAKE_LEGACY_IRQ_STUB name, number
.section .text.stubs
.global \name
\name:
	# Swap GS if we interrupted user-space.
	testl $3, 8(%rsp)
	jz 1f
	swapgs
1:

	push $0 # Dummy error code.
	push %rbp
	push %r15
	push %r14
	push %r13
	push %r12
	push %r10
	push %r9
	push %r8
	push %rsi
	push %rdi
	push %rdx
	push %rax
	push %r11
	push %rcx
	push %rbx

	# Reserve space for iplState.
	sub $8, %rsp

	mov %rsp, %rdi
	mov $\number, %rsi
	xor %ebp, %ebp
	cld
	call onPlatformLegacyIrq

	add $8, %rsp

	pop %rbx
	pop %rcx
	pop %r11
	pop %rax
	pop %rdx
	pop %rdi
	pop %rsi
	pop %r8
	pop %r9
	pop %r10
	pop %r12
	pop %r13
	pop %r14
	pop %r15
	pop %rbp

	# Restore GS.
	testl $3, 16(%rsp)
	jz 1f
	swapgs
1:

	add $8, %rsp
	iretq
.endm

.macro MAKE_IPI_STUB name, handler
.section .text.stubs
.global \name
\name:
	# Swap GS if we interrupted user-space.
	testl $3, 8(%rsp)
	jz 1f
	swapgs
1:

	push $0 # Dummy error code.
	push %rbp
	push %r15
	push %r14
	push %r13
	push %r12
	push %r10
	push %r9
	push %r8
	push %rsi
	push %rdi
	push %rdx
	push %rax
	push %r11
	push %rcx
	push %rbx

	# Reserve space for iplState.
	sub $8, %rsp

	mov %rsp, %rdi
	cld
	call \handler

	add $8, %rsp

	pop %rbx
	pop %rcx
	pop %r11
	pop %rax
	pop %rdx
	pop %rdi
	pop %rsi
	pop %r8
	pop %r9
	pop %r10
	pop %r12
	pop %r13
	pop %r14
	pop %r15
	pop %rbp

	# Restore GS.
	testl $3, 16(%rsp)
	jz 1f
	swapgs
1:

	add $8, %rsp
	iretq
.endm

MAKE_LEGACY_IRQ_STUB thorRtIsrLegacyIrq7, 7
MAKE_LEGACY_IRQ_STUB thorRtIsrLegacyIrq15, 15

MAKE_IPI_STUB thorRtIpiShootdown, onPlatformShootdown
MAKE_IPI_STUB thorRtIpiPing, onPlatformPing
MAKE_IPI_STUB thorRtIpiCall, onPlatformCall
MAKE_IPI_STUB thorRtPreemption, onPlatformPreemption
MAKE_IPI_STUB thorRtSpurious, onPlatformSpurious

# ---------------------------------------------------------
# Syscall stubs
# ---------------------------------------------------------

.section .text.stubs
.global syscallStub
syscallStub:
	# rsp still contains the user-space stack pointer
	# temporarily save it and switch to kernel-stack
	swapgs
	mov %rsp, %rbx
	mov %gs:THOR_GS_SYSCALL_STACK, %rsp

	# Construct iret frame.
	# syscall stores rip to rcx and rflags to r11.
	pushq $0 # ss
	push %rbx
	push %r11
	pushq $0 # cs
	push %rcx

	# Push error code.
	pushq $0

	# Push non-clobbered registers in Frame order.
	push %rbp
	push %r15
	push %r14
	push %r13
	push %r12
	push %r10
	push %r9
	push %r8
	push %rsi
	push %rdi
	push %rdx
	push %rax

	# Skip rbx, rcx, r11, iplState.
	sub $32, %rsp

	mov %rsp, %rdi
	xor %ebp, %ebp
	cld
	call onPlatformSyscall

	# Skip rbx, rcx, r11, iplState.
	add $32, %rsp
	xor %ebx, %ebx

	pop %rax
	pop %rdx
	pop %rdi
	pop %rsi
	pop %r8
	pop %r9
	pop %r10
	pop %r12
	pop %r13
	pop %r14
	pop %r15
	pop %rbp

	# Load rip, rflags, user rsp from the iret frame.
	mov 8(%rsp), %rcx
	mov 24(%rsp), %r11
	mov 32(%rsp), %rsp

	swapgs
	sysretq

.section .text.stubs
.global nmiStub
nmiStub:
	push $0
	push %rbp
	push %r15
	push %r14
	push %r13
	push %r12
	push %r10
	push %r9
	push %r8
	push %rsi
	push %rdi
	push %rdx
	push %rax
	push %r11
	push %rcx
	push %rbx

	# Reserve space for iplState.
	sub $8, %rsp

	mov %rsp, %rdi
	mov 176(%rsp), %rsi
	xor %ebp, %ebp
	cld
	call onPlatformNmi

	add $8, %rsp

	pop %rbx
	pop %rcx
	pop %r11
	pop %rax
	pop %rdx
	pop %rdi
	pop %rsi
	pop %r8
	pop %r9
	pop %r10
	pop %r12
	pop %r13
	pop %r14
	pop %r15
	pop %rbp

	add $8, %rsp
	iretq

# ---------------------------------------------------------
# Executor related functions
# ---------------------------------------------------------

.text
.global forkExecutorRegisters
forkExecutorRegisters:
	mov .L_executorImagePtr(%rdi), %rdi
	mov %rdx, %r11
	
	# Save the fs segment.
	mov $.L_msrIndexFsBase, %rcx
	rdmsr
	shl $32, %rdx
	or %rdx, %rax
	mov %rax, .L_imageClientFs(%rdi)
	
	# Save the gs segment.
	# We are sure that this function is only called from system code
	# so that the client gs is currently swapped to the kernel MSR.
	mov $.L_msrIndexKernelGsBase, %rcx
	rdmsr
	shl $32, %rdx
	or %rdx, %rax
	mov %rax, .L_imageClientGs(%rdi)

	# Only save the registers that are callee-saved by System-V.
	mov %rbx, .L_imageRbx(%rdi)
	mov %rbp, .L_imageRbp(%rdi)
	mov %r12, .L_imageR12(%rdi)
	mov %r13, .L_imageR13(%rdi)
	mov %r14, .L_imageR14(%rdi)
	mov %r15, .L_imageR15(%rdi)

	# Setup the state for the return.
	mov (%rsp), %rdx
	mov %rdx, .L_imageRip(%rdi)
	mov %cs, %rax
	mov %rax, .L_imageCs(%rdi)
	pushfq
	popq .L_imageRflags(%rdi)
	leaq 8(%rsp), %rcx
	mov %rcx, .L_imageRsp(%rdi)
	mov %ss, %rax
	mov %rax, .L_imageSs(%rdi)

	mov %r11, %rdi
	call *%rsi
	ud2

# arguments: void *pointer
.section .text.stubs
.global _restoreExecutorRegisters
.extern thorFredEnabled
_restoreExecutorRegisters:
	# setup the IRET frame
	pushq .L_imageSs(%rdi)
	pushq .L_imageRsp(%rdi)
	pushq .L_imageRflags(%rdi)
	pushq .L_imageCs(%rdi)
	pushq .L_imageRip(%rdi)
	
	# restore the general purpose registers except for rdi
	mov .L_imageRax(%rdi), %rax
	mov .L_imageRbx(%rdi), %rbx
	mov .L_imageRcx(%rdi), %rcx
	mov .L_imageRdx(%rdi), %rdx
	mov .L_imageRsi(%rdi), %rsi
	mov .L_imageRbp(%rdi), %rbp

	mov .L_imageR8(%rdi), %r8
	mov .L_imageR9(%rdi), %r9
	mov .L_imageR10(%rdi), %r10
	mov .L_imageR11(%rdi), %r11
	mov .L_imageR12(%rdi), %r12
	mov .L_imageR13(%rdi), %r13
	mov .L_imageR14(%rdi), %r14
	mov .L_imageR15(%rdi), %r15

	## if fred is enabled dont do an iretq
	cmpb $1, thorFredEnabled(%rip)
	je .L_fred
	mov .L_imageRdi(%rdi), %rdi
	iretq
.L_fred:
	# fred needs a dummy error code
	pushq $0
	testb $3, .L_imageCs(%rdi)
	mov .L_imageRdi(%rdi), %rdi
	jz .L_erets
	eretu
.L_erets:
	erets

	.section .note.GNU-stack,"",%progbits

.global thorFredRing3Entry
.global thorFredRing0Entry
.extern onFredEvent
.section .text
.align 4096

thorFredRing3Entry:
	push %rbp
	push %r15
	push %r14
	push %r13
	push %r12
	push %r10
	push %r9
	push %r8
	push %rsi
	push %rdi
	push %rdx
	push %rax
	push %r11
	push %rcx
	push %rbx

	# Reserve space for iplState.
	sub $8, %rsp

	mov %rsp, %rdi
	xor %ebp, %ebp
	call onFredEvent

    # pop iplState.
	add $8, %rsp

	pop %rbx
	pop %rcx
	pop %r11
	pop %rax
	pop %rdx
	pop %rdi
	pop %rsi
	pop %r8
	pop %r9
	pop %r10
	pop %r12
	pop %r13
	pop %r14
	pop %r15
	pop %rbp
	eretu

.fill 256 - (. - thorFredRing3Entry), 1, 0

thorFredRing0Entry:
	push %rbp
	push %r15
	push %r14
	push %r13
	push %r12
	push %r10
	push %r9
	push %r8
	push %rsi
	push %rdi
	push %rdx
	push %rax
	push %r11
	push %rcx
	push %rbx

	# Reserve space for iplState.
	sub $8, %rsp

	mov %rsp, %rdi
	xor %ebp, %ebp
	call onFredEvent

	# pop iplState.
	add $8, %rsp

	pop %rbx
	pop %rcx
	pop %r11
	pop %rax
	pop %rdx
	pop %rdi
	pop %rsi
	pop %r8
	pop %r9
	pop %r10
	pop %r12
	pop %r13
	pop %r14
	pop %r15
	pop %rbp
	erets
