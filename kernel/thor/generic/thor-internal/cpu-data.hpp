#pragma once

#include <eir/interface.hpp>
#include <thor-internal/elf-notes.hpp>
#include <thor-internal/arch-generic/cpu-data.hpp>

#include <new>
#include <tuple>
#include <utility>

namespace thor {

extern ManagarmElfNote<CpuConfig> cpuConfigNote;

// Forward defined for pointers that are part of CpuData.
struct ExecutorContext;
struct Thread;
struct KernelFiber;
struct SingleContextRecordRing;
struct SelfIntCallBase;
struct WorkQueue;

enum class ProfileMechanism {
	none,
	intelPmc,
	amdPmc
};

// How far a CPU has progressed through its own initialization.
// This only ever advances, i.e., CPUs are never taken offline again.
enum class CpuState {
	// The CPU has not reached its C++ entry point yet.
	offline,
	// The CPU is bringing itself up; its IRQ controller and work queue are not usable yet.
	booting,
	// The CPU is fully brought up: it can send and receive IPIs and it runs its work queue.
	online
};

// "Interrupt priority level". This is our version of the IRQL that the NT kernel uses.
// Note that this is a software concept that does *not* correspond to hardware IRQ priorities.
// Code running at IPL L can safely access thread-local data structures
// if these data structures are only ever accessed at IPL <= L.
using Ipl = short;

using IplMask = uint32_t;

namespace ipl {
// Sentinel / invalid value.
inline constexpr Ipl bad = -1;
// Level that threads run at (unless they raise IPL).
inline constexpr Ipl passive = 0;
// Level that work queues which can be entered from currentIpl() == ipl::passive run at.
// Threads can only block on such work queues while running at currentIpl == ipl::passive.
inline constexpr Ipl passiveWork = 1;
// Level that page faults run at.
// Accessing lower-half memory is only allowed at currentIpl() < ipl::exceptional.
inline constexpr Ipl exceptional = 2;
// Level that work queues which can be entered from currentIpl() <= ipl::exceptional run at.
// Threads can only block on such work queues while running at currentIpl <= ipl::exceptional.
inline constexpr Ipl exceptionalWork = 3;
// Threads that are scheduled out at currentIpl() >= ipl::noMigration (by preemption or by blocking)
// continue on the same CPU.
inline constexpr Ipl noMigration = 4;
// Preemption is only allowed at currentIpl() < ipl::noPreemption.
inline constexpr Ipl noPreemption = 5;
// Blocking is only allowed at currentIpl() < ipl::noSchedule,
// i.e., threads may only be scheduled out if Executor::iplState()->current < ipl::noSchedule.
inline constexpr Ipl noSchedule = 6;
// Level that interrupts run at.
// Also, level that the scheduler itself runs at.
inline constexpr Ipl interrupt = 7;
// Level that exceptions and NMIs run at.
// This is the only level that can be entered multiple times
// (i.e., ipl::maximal -> ipl::maximal entries are allowed).
inline constexpr Ipl maximal = 8;
} // namespace ipl

struct IplState {
	// Level of the current context.
	Ipl context{ipl::passive};
	// Level of the currenly executing code path. This is always above the context level.
	Ipl current{ipl::passive};
};

struct alignas(8) IntState {
	std::atomic<unsigned int> nesting{0};
	Ipl outerIpl{ipl::bad};
};

struct CpuData : public PlatformCpuData {
	CpuData();

	CpuData(const CpuData &) = delete;

	CpuData &operator= (const CpuData &) = delete;

	Ipl contextIpl{ipl::passive};
	// CPUs boot with interrupts disabled, so we initialize to ipl::interrupt.
	std::atomic<Ipl> currentIpl{ipl::interrupt};
	std::atomic<uint32_t> iplDeferred{0};
	// Used by IrqMutex.
	IntState intState;
	UniqueKernelStack detachedStack;
	UniqueKernelStack idleStack;
	bool haveVirtualization;

	int cpuIndex;

	ExecutorContext *executorContext{nullptr};
	Thread *activeThread{nullptr};
	KernelFiber *activeFiber{nullptr};
	KernelFiber *criticalFiber{nullptr};
	KernelFiber *memoryFiber{nullptr};
	KernelFiber *generalFiber{nullptr};
	std::atomic<SelfIntCallBase *> selfIntCallPtr{nullptr};
	// See WorkQueue::criticalQueue() for what may run here.
	smarter::shared_ptr<WorkQueue> criticalWorkQueue;
	// See WorkQueue::memoryQueue() for what may run here.
	smarter::shared_ptr<WorkQueue> memoryWorkQueue;
	// See WorkQueue::generalQueue() for what may run here.
	smarter::shared_ptr<WorkQueue> generalWorkQueue;
	std::atomic<uint64_t> heartbeat;
	// Set to true to indicate that the CPU is in an explicit quiescent state.
	// If set, the RCU engine does not schedule a memory barrier on this CPU's WQ.
	// This is set while the CPU is halted in the idle loop, see rcuSetQuiescent() and rcuClearQuiescent().
	std::atomic<bool> rcuQuiescent{false};

	IseqContext regularIseq;

	// Advanced by setCpuState() as the CPU brings itself up.
	// This allows us to check whether various per-CPU data structures are initialized,
	// for example the CPU's interrupt controller for sending IPIs.
	std::atomic<CpuState> cpuState{CpuState::offline};

	unsigned int irqEntropySeq = 0;
	std::atomic<ProfileMechanism> profileMechanism{};
	// TODO: This should be a unique_ptr instead.
	SingleContextRecordRing *localProfileRing = nullptr;
};

inline CpuData *getCpuData() {
	return static_cast<CpuData *>(getPlatformCpuData());
}


extern "C" char percpuStart[], percpuEnd[];

template<typename T, typename... Args>
concept HasCpuDataConstructor = requires(CpuData *cpuData, Args &&... args) {
	{ T{cpuData, std::forward<Args>(args)...} };
};

// To add a new per-CPU variable, add a forward declaration like
// "extern PerCpu<Foo> foo;" in a header or source file, and then use
// THOR_DEFINE_PERCPU{,_UNINITIALIZED}(foo) in a source file to define it.
template <typename T>
struct PerCpu {
	T &get(CpuData *context) {
		auto offset =
			reinterpret_cast<uintptr_t>(&reservation)
			- reinterpret_cast<uintptr_t>(percpuStart);

		return *std::launder(reinterpret_cast<T *>(
					reinterpret_cast<uintptr_t>(context) + offset));
	}

	T &get() {
		return get(getCpuData());
	}

	T &getFor(size_t cpu) {
		auto size = percpuEnd - percpuStart;

		return *std::launder(reinterpret_cast<T *>(
					reinterpret_cast<uintptr_t>(&reservation) + size * cpu));
	}

	template<typename... Args>
	void initialize(CpuData *context, Args &&... args) {
		auto offset =
			reinterpret_cast<uintptr_t>(&reservation)
			- reinterpret_cast<uintptr_t>(percpuStart);

		auto ptr = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(context) + offset);

		if constexpr (HasCpuDataConstructor<T, Args...>) {
			new(ptr) T{context, std::forward<Args>(args)...};
		} else {
			new(ptr) T{std::forward<Args>(args)...}; // Value-initialize primitive types.
		}
	}

private:
	frg::aligned_storage<sizeof(T), alignof(T)> reservation;
};


using PerCpuInitializer = void(*)(CpuData *context);

template<auto *C>
void doInitializePerCpu(CpuData *context) {
	C->initialize(context);
}

#define THOR_DEFINE_PERCPU_INITIALIZER_PRIV(Name)			\
	[[gnu::section(".percpu_init"), gnu::used]]			\
	const constinit PerCpuInitializer Name ## _initializer_ = doInitializePerCpu<&Name>;

#define THOR_DEFINE_PERCPU_UNINITIALIZED_PRIV(Name, Suffix)	\
	[[gnu::section(".percpu" Suffix), gnu::used]]		\
	constinit decltype(Name) Name				\


// Define a per-CPU variable without an initializer. Care has to be
// taken to call Name.initialize(context) prior to accessing it from
// the given context. This is mainly intended for
// architecture-specific fields that have to be initialized prior to
// the allocator being available.
#define THOR_DEFINE_PERCPU_UNINITIALIZED(Name)		\
	THOR_DEFINE_PERCPU_UNINITIALIZED_PRIV(Name, "")	\

// Define a per-CPU variable that's initialized automatically. The
// initialization for the boot CPU happens after the kernel heap is
// available.
#define THOR_DEFINE_PERCPU(Name)			\
	THOR_DEFINE_PERCPU_UNINITIALIZED(Name);		\
	THOR_DEFINE_PERCPU_INITIALIZER_PRIV(Name)	\



extern PerCpu<CpuData> cpuData;

// Run initializers for the per-CPU variables of all CPUs.
void runCpuDataInitializers();


inline CpuData *getCpuData(size_t cpu) {
	return &cpuData.getFor(cpu);
}

size_t getCpuCount();

inline ExecutorContext *currentExecutorContext() {
	return getCpuData()->executorContext;
}

} // namespace thor
