#pragma once

#include <thor-internal/arch/asm.h>
#include <thor-internal/kernel-stack.hpp>

namespace thor {

struct Executor;
struct IseqContext;
struct UserAccessRegion;

// Note: This struct is accessed from assembly.
// Do not change the field offsets!
struct AssemblyCpuData {
	AssemblyCpuData *selfPointer;
	Executor *activeExecutor{nullptr};
	// TODO: This is unused for now but required to be in PlatformCpuData by generic code.
	// 		 We need to make use of this once we use NMIs on ARM.
	IseqContext *iseqPtr{nullptr};
};

static_assert(offsetof(AssemblyCpuData, selfPointer) == THOR_TP_SELF);
static_assert(offsetof(AssemblyCpuData, activeExecutor) == THOR_TP_EXECUTOR);
static_assert(offsetof(AssemblyCpuData, iseqPtr) == THOR_TP_ISEQ_PTR);

struct GicCpuInterfaceV2;
struct Thread;

struct PlatformCpuData : public AssemblyCpuData {
	PlatformCpuData();

	int cpuIndex;
	int archCpuIndex;

	uint32_t profileFlags = 0;

	bool preemptionIsArmed = false;

	GicCpuInterfaceV2 *gicCpuInterfaceV2 = nullptr;
	uint32_t affinity;
};

// Get a pointer to this CPU's PlatformCpuData instance.
// Always inline as this might be called from code that disables coverage
[[gnu::always_inline]] inline PlatformCpuData *getPlatformCpuData() {
	AssemblyCpuData *cpu_data = nullptr;
	asm volatile ("mrs %0, tpidr_el1" : "=r"(cpu_data));
	return static_cast<PlatformCpuData *>(cpu_data);
}

} // namespace thor
