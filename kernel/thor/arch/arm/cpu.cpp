#include <thor-internal/arch/system.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/cpu-state.hpp>
#include <thor-internal/ipl.hpp>
#include <thor-internal/kcov.hpp>
#include <frg/manual_box.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/kasan.hpp>
#include <thor-internal/fiber.hpp>

namespace thor {

extern "C" void saveFpSimdRegisters(FpRegisters *frame);
extern "C" void restoreFpSimdRegisters(FpRegisters *frame);

uint32_t affinityFromMpidr(uint64_t mpidr) {
	return (mpidr & 0xFFFFFF) | ((mpidr >> 32) & 0xFF) << 24;
}

bool FaultImageAccessor::allowUserPages() {
	return true;
}

void UserContext::deactivate() { }
UserContext::UserContext()
: kernelStack(UniqueKernelStack::make()) { }

void UserContext::migrate(CpuData *) {
	assert(!intsAreEnabled());
}

FiberContext::FiberContext(UniqueKernelStack stack)
: stack{std::move(stack)} { }

extern "C" [[ noreturn ]] void _restoreExecutorRegisters(void *pointer, void *el1Stack);

[[noreturn]] void restoreExecutor(Executor *executor) {
	getCpuData()->activeExecutor = executor;
	restoreFpSimdRegisters(executor->fp());

	iplLeaveContext(executor->general()->iplState);

	void *el1Stack = nullptr;
	if ((executor->general()->spsr & 0b1111) == 0b0000) {
		el1Stack = executor->_exceptionStack;
		asm volatile ("msr sp_el0, %0" :: "r"(executor->general()->sp) : "memory");
	} else {
		el1Stack = reinterpret_cast<void *>(executor->general()->sp);
	}

	_restoreExecutorRegisters(executor->general(), el1Stack);
}

Executor::Executor()
: _pointer{nullptr}, _exceptionStack{nullptr} {  }

Executor::Executor(UserContext *context) {
	_pointer = static_cast<char *>(kernelAlloc->allocate(determineSize()));
	memset(_pointer, 0, determineSize());

	_exceptionStack = context->kernelStack.basePtr();
}

Executor::Executor(UserContext *context, void (*launch)())
: Executor{context} {
	general()->elr = reinterpret_cast<uintptr_t>(launch);
	general()->sp = reinterpret_cast<uintptr_t>(_exceptionStack);
	general()->spsr = isKernelInEl2() ? 9 : 5;
}

Executor::Executor(UserContext *context, AbiParameters abi)
: Executor{context} {
	general()->elr = abi.ip;
	general()->sp = abi.sp;
	general()->spsr = 0;
}

Executor::Executor(FiberContext *context, AbiParameters abi)
: _exceptionStack{nullptr} {
	_pointer = static_cast<char *>(kernelAlloc->allocate(determineSize()));
	memset(_pointer, 0, determineSize());

	general()->elr = abi.ip;
	general()->sp = (uintptr_t)context->stack.basePtr();
	general()->x[0] = abi.argument;
	general()->spsr = isKernelInEl2() ? 9 : 5;
}

Executor::~Executor() {
	kernelAlloc->free(_pointer);
}

void saveExecutor(Executor *executor, FaultImageAccessor accessor) {
	for (int i = 0; i < 31; i++)
		executor->general()->x[i] = accessor._frame()->x[i];

	executor->general()->elr = accessor._frame()->elr;
	executor->general()->spsr = accessor._frame()->spsr;
	executor->general()->sp = accessor._frame()->sp;
	executor->general()->tpidr_el0 = accessor._frame()->tpidr_el0;
	executor->general()->iplState = accessor._frame()->iplState;

	saveFpSimdRegisters(executor->fp());
}

void saveExecutor(Executor *executor, IrqImageAccessor accessor) {
	for (int i = 0; i < 31; i++)
		executor->general()->x[i] = accessor._frame()->x[i];

	executor->general()->elr = accessor._frame()->elr;
	executor->general()->spsr = accessor._frame()->spsr;
	executor->general()->sp = accessor._frame()->sp;
	executor->general()->tpidr_el0 = accessor._frame()->tpidr_el0;
	executor->general()->iplState = accessor._frame()->iplState;

	saveFpSimdRegisters(executor->fp());
}

void saveExecutor(Executor *executor, SyscallImageAccessor accessor) {
	for (int i = 0; i < 31; i++)
		executor->general()->x[i] = accessor._frame()->x[i];

	executor->general()->elr = accessor._frame()->elr;
	executor->general()->spsr = accessor._frame()->spsr;
	executor->general()->sp = accessor._frame()->sp;
	executor->general()->tpidr_el0 = accessor._frame()->tpidr_el0;
	executor->general()->iplState = accessor._frame()->iplState;

	saveFpSimdRegisters(executor->fp());
}

extern "C" void forkExecutorRegisters(Executor *executor, void (*functor)(void *), void *context);

void doForkExecutor(Executor *executor, void (*functor)(void *), void *context) {
	iplSave(executor->general()->iplState);

	forkExecutorRegisters(executor, functor, context);
}

void scrubStack(FaultImageAccessor accessor, Continuation cont) {
	scrubStackFrom(reinterpret_cast<uintptr_t>(accessor.frameBase()), cont);;
}

void scrubStack(IrqImageAccessor accessor, Continuation cont) {
	scrubStackFrom(reinterpret_cast<uintptr_t>(accessor.frameBase()), cont);;
}

void scrubStack(SyscallImageAccessor accessor, Continuation cont) {
	scrubStackFrom(reinterpret_cast<uintptr_t>(accessor.frameBase()), cont);;
}

void scrubStack(Executor *executor, Continuation cont) {
	scrubStackFrom(reinterpret_cast<uintptr_t>(*executor->sp()), cont);
}

PlatformCpuData::PlatformCpuData() {
}

// TODO: support PAN?
void enableUserAccess() { }
void disableUserAccess() { }

bool iseqStore64(uint64_t *p, uint64_t v) {
	// TODO: This is a shim. A proper implementation is needed for NMIs on ARM.
	std::atomic_ref{*p}.store(v, std::memory_order_relaxed);
	return true;
}

bool iseqCopyWeak(void *dst, const void *src, size_t size) {
	// TODO: This is a shim. A proper implementation is needed for NMIs on ARM.
	memcpy(dst, src, size);
	return true;
}

void doRunOnStack(void (*function) (void *, void *), void *sp, void *argument) {
	assert(!intsAreEnabled());

	cleanKasanShadow(reinterpret_cast<std::byte *>(sp) - UniqueKernelStack::kSize,
			UniqueKernelStack::kSize);

	asm volatile (
			"\tmov x28, sp\n"
			"\tmov x1, sp\n"
			"\tmov x0, %0\n"
			"\tmov sp, %2\n"
			"\tblr %1\n"
			"\tmov sp, x28\n"
			:
			: "r" (argument), "r" (function), "r" (sp)
			: "x30", "x28", "x1", "x0", "memory");
}

Error getEntropyFromCpu(void *, size_t) { return Error::noHardwareSupport; }

THOR_KCOV_DISABLED void setupCpuContext(AssemblyCpuData *context) {
	asm volatile("msr tpidr_el1, %0" :: "r"(context));
}

void prepareCpuDataFor(CpuData *context, int cpu) {
	cpuData.initialize(context);
	heapSlabPool.initialize(context, HeapSlabPolicy{.shard = static_cast<uint32_t>(cpu)});

	context->selfPointer = context;
	context->cpuIndex = cpu;
}

void setupBootCpuContext() {
	for (size_t c = 0; c < getCpuCount(); ++c)
		prepareCpuDataFor(getCpuData(c), c);

	auto context = getCpuData(0);
	setupCpuContext(context);

}

static initgraph::Task initBootProcessorTask{&globalInitEngine, "arm.init-boot-processor",
	initgraph::Entails{getBootProcessorReadyStage()},
	[] {
		infoLogger() << "Booting on CPU #0" << frg::endlog;

		if (isKernelInEl2()) {
			infoLogger() << "Booting in EL2" << frg::endlog;
		} else {
			infoLogger() << "Booting in EL1" << frg::endlog;
		}

		initializeThisProcessor();
	}
};

initgraph::Stage *getBootProcessorReadyStage() {
	static initgraph::Stage s{&globalInitEngine, "arm.boot-processor-ready"};
	return &s;
}

initgraph::Edge bootProcessorReadyEdge{
	getBootProcessorReadyStage(),
	getFibersAvailableStage()
};

void initializeThisProcessor() {
	auto cpu_data = getCpuData();

	setCpuState(cpu_data, CpuState::booting);

	// Enable FPU
	asm volatile ("msr cpacr_el1, %0" :: "r"(uint64_t(0b11 << 20)));

	// Enable access to cache info register and cache maintenance instructions
	uint64_t sctlr;
	asm volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));

	sctlr |= (uint64_t(1) << 14);
	sctlr |= (uint64_t(1) << 15);
	sctlr |= (uint64_t(1) << 26);

	asm volatile ("msr sctlr_el1, %0" :: "r"(sctlr));

	// Allow EL0 to read the counter that backs the monotonic clock.
	// In EL2, the register is redirected to CNTHCTL_EL2, which eir and the SMP trampoline
	// already program. Never preserve the other bits: the reset value is UNKNOWN.
	if (!isKernelInEl2()) {
		uint64_t cntkctl = uint64_t(1) << 1; // EL0VCTEN
		asm volatile ("msr cntkctl_el1, %0" :: "r"(cntkctl));
	}

	uint64_t mpidr;
	asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
	cpu_data->affinity = affinityFromMpidr(mpidr);

	cpu_data->detachedStack = UniqueKernelStack::make();
	cpu_data->idleStack = UniqueKernelStack::make();

	postWorkQueueFibers(cpu_data);
}

} // namespace thor
