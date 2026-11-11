#pragma once

#include <frg/optional.hpp>
#include <thor-internal/arch/asm.h>
#include <thor-internal/kernel-stack.hpp>

namespace thor {

struct IseqContext;
struct UserAccessRegion;
struct Executor;

// Note: This struct is accessed from assembly.
// Do not change the field offsets!
struct AssemblyCpuData {
	AssemblyCpuData *selfPointer;
	uint64_t currentDomain;
	Executor *activeExecutor{nullptr};
	void *exceptionStackPtr;
	void *irqStackPtr;
	uint64_t scratchSp;
	IseqContext *iseqPtr;
	uint64_t hscratchSp;
};

static_assert(offsetof(AssemblyCpuData, selfPointer) == THOR_TP_SELF);
static_assert(offsetof(AssemblyCpuData, currentDomain) == THOR_TP_DOMAIN);
static_assert(offsetof(AssemblyCpuData, activeExecutor) == THOR_TP_EXECUTOR);
static_assert(offsetof(AssemblyCpuData, exceptionStackPtr) == THOR_TP_EXCEPTION_STACK);
static_assert(offsetof(AssemblyCpuData, irqStackPtr) == THOR_TP_IRQ_STACK);
static_assert(offsetof(AssemblyCpuData, scratchSp) == THOR_TP_SCRATCH_SP);
static_assert(offsetof(AssemblyCpuData, hscratchSp) == THOR_TP_HSCRATCH_SP);
static_assert(offsetof(AssemblyCpuData, iseqPtr) == THOR_TP_ISEQ_PTR);

// Always inline as this might be called from code that disables coverage
[[gnu::always_inline]] inline void writeToTp(AssemblyCpuData *context) {
	asm volatile("mv tp, %0" : : "r"(context));
}

struct Thread;

struct PlatformCpuData : public AssemblyCpuData {
	// Bits of the pendingIpis field.
	// Since RISC-V only has a single IPI vector, we need to emulate multiple IPIs in software.
	static constexpr uint64_t ipiPing = UINT64_C(1) << 0;
	static constexpr uint64_t ipiShootdown = UINT64_C(1) << 1;
	static constexpr uint64_t ipiSelfCall = UINT64_C(1) << 2;
	static constexpr uint64_t ipiHypervisor = UINT64_C(1) << 3;

	uint64_t hartId{~UINT64_C(0)};

	// Actual value of the FS field in sstatus before it was cleared in the kernel.
	// Zero (= extOff) indicates that the current register state cannot be relied upon
	// (i.e., it has been saved to the executor or it is in control of U-mode).
	uint8_t stashedFs{0};

	std::atomic<uint64_t> pendingIpis;

	// Deadlines for global timers and preemption.
	// frg::null_opt is the respective deadline is not set (= infinite).
	frg::optional<uint64_t> timerDeadline;
	frg::optional<uint64_t> preemptionDeadline;
	// Current deadline programmed into the supervisor timer.
	uint64_t currentDeadline{~UINT64_C(0)};

	UniqueKernelStack irqStack;

	uint32_t profileFlags = 0;
};

// Get a pointer to this CPU's PlatformCpuData instance.
// Always inline as this might be called from code that disables coverage
[[gnu::always_inline]] inline PlatformCpuData *getPlatformCpuData() {
	AssemblyCpuData *result;
	asm volatile("mv %0, tp" : "=r"(result));
	return static_cast<PlatformCpuData *>(result);
}

} // namespace thor
