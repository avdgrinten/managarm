#include <thor-internal/arch/hpet.hpp>
#include <thor-internal/arch/vmx.hpp>
#include <thor-internal/arch/svm.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/kasan.hpp>
#include <thor-internal/load-balancing.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/rcu.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/cpu-state.hpp>
#include <thor-internal/ipl.hpp>
#include <thor-internal/kcov.hpp>
#include <thor-internal/arch/pic.hpp>
#include <frg/cmdline.hpp>
#include <x86/machine.hpp>

namespace thor {

namespace {
	constexpr bool disableSmp = false;
}

namespace {
	void activateTss(common::x86::Tss64 *tss) {
		common::x86::makeGdtTss64Descriptor(cpuDescriptorTables.get().gdt, kGdtIndexTask,
				tss, sizeof(common::x86::Tss64));
		asm volatile ("ltr %w0" : : "r"(kSelTask) : "memory");
	}
}

// --------------------------------------------------------
// FaultImageAccessor
// --------------------------------------------------------

bool FaultImageAccessor::allowUserPages() {
	assert(!inUserMode());
	if(!getCpuData()->haveSmap)
		return true;
	return *rflags() & (uint32_t(1) << 18);
}

// --------------------------------------------------------
// Executor
// --------------------------------------------------------

static constexpr uint16_t fcwInitializer =
	(1 << 0) |    // IM
	(1 << 1) |    // DM
	(1 << 2) |    // ZM
	(1 << 3) |    // OM
	(1 << 4) |    // UM
	(1 << 5) |    // PM
	(0b11 << 8);  // PC

static constexpr uint32_t mxcsrInitializer = 0b1111110000000;


size_t Executor::determineSimdSize() {
	assert(cpuFeaturesKnown);
	if(getGlobalCpuFeatures()->haveXsave){
		return getGlobalCpuFeatures()->xsaveRegionSize;
	}else{
		return sizeof(FxState);
	}
}

size_t Executor::determineSize() {
	// fxState is offset from General by 0x10 bytes to make it 64byte aligned for xsave
	return sizeof(General) + 0x10 + determineSimdSize();
}

Executor::Executor()
: _pointer{nullptr}, _syscallStack{nullptr}, _tss{nullptr} { }

Executor::Executor(UserContext *context) {
	_pointer = (char *)kernelAlloc->allocate(determineSize());
	memset(_pointer, 0, determineSize());

	// Assert assumptions about xsave.
	assert(!((uintptr_t)_pointer & 0x3F));
	assert(!((uintptr_t)this->_fxState() & 0x3F));

	_fxState()->mxcsr |= mxcsrInitializer;
	_fxState()->fcw |= fcwInitializer;

	_tss = &context->tss;
	_syscallStack = context->kernelStack.basePtr();
}

Executor::Executor(UserContext *context, void (*launch)())
: Executor{context} {
	general()->rip = reinterpret_cast<uintptr_t>(launch);
	general()->rflags = 0x202;
	general()->rsp = reinterpret_cast<uintptr_t>(_syscallStack);
	general()->cs = kSelKernelCode;
	general()->ss = kSelKernelData;
}

Executor::Executor(UserContext *context, AbiParameters abi)
: Executor{context} {
	general()->rip = abi.ip;
	general()->rflags = 0x202;
	general()->rsp = abi.sp;
	general()->cs = kSelUserCode;
	general()->ss = kSelUserData;
}

Executor::Executor(FiberContext *context, AbiParameters abi)
: _syscallStack{nullptr}, _tss{nullptr} {
	_pointer = (char *)kernelAlloc->allocate(determineSize());
	memset(_pointer, 0, determineSize());

	// Assert assumptions about xsave
	assert(!((uintptr_t)_pointer & 0x3F));
	assert(!((uintptr_t)this->_fxState() & 0x3F));

	_fxState()->mxcsr |= mxcsrInitializer;
	_fxState()->fcw |= fcwInitializer;

	general()->rip = abi.ip;
	general()->rflags = 0x202;
	general()->rsp = (uintptr_t)context->stack.basePtr();
	general()->rdi = abi.argument;
	general()->cs = kSelKernelCode;
	general()->ss = kSelKernelData;
}

Executor::~Executor() {
	kernelAlloc->free(_pointer);
}

void saveExecutor(Executor *executor, FaultImageAccessor accessor) {
	executor->general()->rax = accessor._frame()->rax;
	executor->general()->rbx = accessor._frame()->rbx;
	executor->general()->rcx = accessor._frame()->rcx;
	executor->general()->rdx = accessor._frame()->rdx;
	executor->general()->rdi = accessor._frame()->rdi;
	executor->general()->rsi = accessor._frame()->rsi;
	executor->general()->rbp = accessor._frame()->rbp;

	executor->general()->r8 = accessor._frame()->r8;
	executor->general()->r9 = accessor._frame()->r9;
	executor->general()->r10 = accessor._frame()->r10;
	executor->general()->r11 = accessor._frame()->r11;
	executor->general()->r12 = accessor._frame()->r12;
	executor->general()->r13 = accessor._frame()->r13;
	executor->general()->r14 = accessor._frame()->r14;
	executor->general()->r15 = accessor._frame()->r15;

	executor->general()->rip = accessor._frame()->rip;
	executor->general()->cs = accessor._frame()->cs;
	executor->general()->rflags = accessor._frame()->rflags;
	executor->general()->rsp = accessor._frame()->rsp;
	executor->general()->ss = accessor._frame()->ss;
	executor->general()->clientFs = common::x86::rdmsr(common::x86::kMsrIndexFsBase);
	executor->general()->clientGs = common::x86::rdmsr(common::x86::kMsrIndexKernelGsBase);
	executor->general()->iplState = accessor._frame()->iplState;

	if(getGlobalCpuFeatures()->haveXsave){
		common::x86::xsave((uint8_t*)executor->_fxState(), ~0);
	} else {
		asm volatile ("fxsaveq %0" : : "m" (*executor->_fxState()));
	}
}

void saveExecutor(Executor *executor, IrqImageAccessor accessor) {
	executor->general()->rax = accessor._frame()->rax;
	executor->general()->rbx = accessor._frame()->rbx;
	executor->general()->rcx = accessor._frame()->rcx;
	executor->general()->rdx = accessor._frame()->rdx;
	executor->general()->rdi = accessor._frame()->rdi;
	executor->general()->rsi = accessor._frame()->rsi;
	executor->general()->rbp = accessor._frame()->rbp;

	executor->general()->r8 = accessor._frame()->r8;
	executor->general()->r9 = accessor._frame()->r9;
	executor->general()->r10 = accessor._frame()->r10;
	executor->general()->r11 = accessor._frame()->r11;
	executor->general()->r12 = accessor._frame()->r12;
	executor->general()->r13 = accessor._frame()->r13;
	executor->general()->r14 = accessor._frame()->r14;
	executor->general()->r15 = accessor._frame()->r15;

	executor->general()->rip = accessor._frame()->rip;
	executor->general()->cs = accessor._frame()->cs;
	executor->general()->rflags = accessor._frame()->rflags;
	executor->general()->rsp = accessor._frame()->rsp;
	executor->general()->ss = accessor._frame()->ss;
	executor->general()->clientFs = common::x86::rdmsr(common::x86::kMsrIndexFsBase);
	executor->general()->clientGs = common::x86::rdmsr(common::x86::kMsrIndexKernelGsBase);
	executor->general()->iplState = accessor._frame()->iplState;

	if(getGlobalCpuFeatures()->haveXsave){
		common::x86::xsave((uint8_t*)executor->_fxState(), ~0);
	}else{
		asm volatile ("fxsaveq %0" : : "m" (*executor->_fxState()));
	}
}

void saveExecutor(Executor *executor, SyscallImageAccessor accessor) {
	// Note that rbx, rcx and r11 are used internally by the syscall mechanism.
	executor->general()->rax = accessor._frame()->rax;
	executor->general()->rdx = accessor._frame()->rdx;
	executor->general()->rdi = accessor._frame()->rdi;
	executor->general()->rsi = accessor._frame()->rsi;
	executor->general()->rbp = accessor._frame()->rbp;

	executor->general()->r8 = accessor._frame()->r8;
	executor->general()->r9 = accessor._frame()->r9;
	executor->general()->r10 = accessor._frame()->r10;
	executor->general()->r12 = accessor._frame()->r12;
	executor->general()->r13 = accessor._frame()->r13;
	executor->general()->r14 = accessor._frame()->r14;
	executor->general()->r15 = accessor._frame()->r15;

	// Note that we do not save cs and ss on syscall.
	// We just assume that these registers have their usual values.
	executor->general()->rip = accessor._frame()->rip;
	executor->general()->cs = kSelUserCode;
	executor->general()->rflags = accessor._frame()->rflags;
	executor->general()->rsp = accessor._frame()->rsp;
	executor->general()->ss = kSelUserData;
	executor->general()->clientFs = common::x86::rdmsr(common::x86::kMsrIndexFsBase);
	executor->general()->clientGs = common::x86::rdmsr(common::x86::kMsrIndexKernelGsBase);
	executor->general()->iplState = accessor._frame()->iplState;

	if(getGlobalCpuFeatures()->haveXsave){
		common::x86::xsave((uint8_t*)executor->_fxState(), ~0);
	}else{
		asm volatile ("fxsaveq %0" : : "m" (*executor->_fxState()));
	}
}

extern "C" void forkExecutorRegisters(Executor *executor, void (*functor)(void *), void *context);

void doForkExecutor(Executor *executor, void (*functor)(void *), void *context) {
	iplSave(executor->general()->iplState);

	forkExecutorRegisters(executor, functor, context);
}

extern "C" [[ noreturn ]] void _restoreExecutorRegisters(void *pointer);
extern "C" bool thorFredEnabled;

[[ gnu::section(".text.stubs") ]] THOR_KCOV_DISABLED void restoreExecutor(Executor *executor) {
	if(executor->_tss) {
		activateTss(executor->_tss);
	}else{
		activateTss(&cpuDescriptorTables.get().tss);
	}

	getCpuData()->activeExecutor = executor;
	getCpuData()->syscallStack = executor->_syscallStack;

	// ensure FRED RSP0 is in sync with the syscall stack
	if (thorFredEnabled)
		common::x86::wrmsr(common::x86::kMsrFredRSP0, (uint64_t)executor->_syscallStack);

	// TODO: use wr{fs,gs}base if it is available
	common::x86::wrmsr(common::x86::kMsrIndexFsBase, executor->general()->clientFs);
	common::x86::wrmsr(common::x86::kMsrIndexKernelGsBase, executor->general()->clientGs);

	if(getGlobalCpuFeatures()->haveXsave){
		common::x86::xrstor((uint8_t*)executor->_fxState(), ~0);
	}else{
		asm volatile ("fxrstorq %0" : : "m" (*executor->_fxState()));
	}

	iplLeaveContext(executor->general()->iplState);

	uint16_t cs = executor->general()->cs;
	assert(cs == kSelKernelCode || cs == kSelUserCode);
	if(cs == kSelUserCode && !thorFredEnabled)
		asm volatile ( "swapgs" : : : "memory" );

	_restoreExecutorRegisters(executor->general());
}

// --------------------------------------------------------
// Stack scrubbing.
// --------------------------------------------------------

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

// --------------------------------------------------------
// UserContext
// --------------------------------------------------------

void UserContext::deactivate() {
	activateTss(&cpuDescriptorTables.get().tss);
}

UserContext::UserContext()
: kernelStack{UniqueKernelStack::make()} {
	memset(&tss, 0, sizeof(common::x86::Tss64));
	common::x86::initializeTss64(&tss);
	tss.rsp0 = (Word)kernelStack.basePtr();
}

void UserContext::enableIoPort(uintptr_t port) {
	tss.ioBitmap[port / 8] &= ~(1 << (port % 8));
}

void UserContext::migrate(CpuData *cpu_data) {
	assert(!intsAreEnabled());
	tss.ist2 = (Word)cpu_data->dfStack.basePtr();
	tss.ist3 = (Word)cpu_data->nmiStack.basePtr();
}

// --------------------------------------------------------
// FiberContext
// --------------------------------------------------------

FiberContext::FiberContext(UniqueKernelStack stack)
: stack{std::move(stack)} { }

// --------------------------------------------------------
// PlatformCpuData
// --------------------------------------------------------

PlatformCpuData::PlatformCpuData() { }

void enableUserAccess() {
	if(getCpuData()->haveSmap)
		asm volatile ("stac" : : : "memory");
}
void disableUserAccess() {
	if(getCpuData()->haveSmap)
		asm volatile ("clac" : : : "memory");
}

THOR_DEFINE_PERCPU(cpuDescriptorTables);

CpuDescriptorTables::CpuDescriptorTables() {
	// Setup the GDT.
	// Note: the TSS requires two slots in the GDT.
	common::x86::makeGdtNullSegment(gdt, kGdtIndexNull);
	common::x86::makeGdtNullSegment(gdt, kGdtIndexPadding);
	common::x86::makeGdtTss64Descriptor(gdt, kGdtIndexTask, nullptr, 0);
	common::x86::makeGdtCode64SystemSegment(gdt, kGdtIndexKernelCode);
	common::x86::makeGdtFlatData32SystemSegment(gdt, kGdtIndexKernelData);
	common::x86::makeGdtNullSegment(gdt, kGdtIndexUserCompat);
	common::x86::makeGdtFlatData32UserSegment(gdt, kGdtIndexUserData);
	common::x86::makeGdtCode64UserSegment(gdt, kGdtIndexUserCode);

	// Setup the per-CPU TSS. This TSS is used by system code.
	memset(&tss, 0, sizeof(common::x86::Tss64));
	common::x86::initializeTss64(&tss);
}

// --------------------------------------------------------
// Namespace scope functions
// --------------------------------------------------------

constinit bool cpuFeaturesKnown = false;
constinit CpuFeatures globalCpuFeatures{};

initgraph::Stage *getCpuFeaturesKnownStage() {
	static initgraph::Stage s{&globalInitEngine, "x86.cpu-features-known"};
	return &s;
}

namespace {

// Determine the XSAVE region size from the enabled components:
// while cpuid can report the size of *all* components, that may be much larger than what we actually enable.
size_t xsaveRegionSizeFor(uint64_t xcr0Mask) {
	// Legacy FXSAVE area plus the XSAVE header. Components 0 (x87) and 1 (SSE) live in there.
	size_t size = 576;

	for(int i = 2; i < 64; ++i) {
		if(!(xcr0Mask & (uint64_t(1) << i)))
			continue;
		// EBX is the offset of the component, EAX its size. Take the maximum over all components.
		auto leaf = common::x86::cpuid(0xD, i);
		auto end = size_t{leaf[1]} + size_t{leaf[0]};
		if(end > size)
			size = end;
	}

	return size;
}

} // namespace

static initgraph::Task enumerateCpuFeaturesTask{&globalInitEngine, "x86.enumerate-cpu-features",
	initgraph::Entails{getCpuFeaturesKnownStage()},
	[] {
		if(common::x86::cpuid(common::x86::kCpuIndexStructuredExtendedFeaturesEnum,1)[0] & common::x86::kCpuFred) {
			debugLogger() << "thor: CPUs support FRED" << frg::endlog;
			globalCpuFeatures.haveFred = true;
		}

		// Enable the XSAVE instruction set and child features
		if(common::x86::cpuid(0x1)[2] & (uint32_t(1) << 26)) {
			debugLogger() << "thor: CPUs support XSAVE" << frg::endlog;
			globalCpuFeatures.haveXsave = true;
		}else{
			debugLogger() << "thor: CPUs do not support XSAVE!" << frg::endlog;
		}

		if(globalCpuFeatures.haveXsave) {
			if(common::x86::cpuid(0x1)[2] & (uint32_t(1) << 28)) {
				debugLogger() << "thor: CPUs support AVX" << frg::endlog;
				globalCpuFeatures.haveAvx = true;
			}else{
				debugLogger() << "thor: CPUs do not support AVX!" << frg::endlog;
			}

			if(common::x86::cpuid(0x07)[1] & (uint32_t(1) << 16)) {
				debugLogger() << "thor: CPUs support AVX-512" << frg::endlog;
				globalCpuFeatures.haveZmm = true;
			}else{
				debugLogger() << "thor: CPUs do not support AVX-512!" << frg::endlog;
			}

			// Compute supported xcr0 features.
			uint64_t xcr0Mask = (uint64_t(1) << 0) // x87 feature set.
					| (uint64_t(1) << 1); // SSE feature set.

			if(globalCpuFeatures.haveAvx)
				xcr0Mask |= uint64_t(1) << 2; // AVX feature set.

			if(globalCpuFeatures.haveZmm)
				xcr0Mask |= (uint64_t(1) << 5)  // AVX-512 opmask registers.
						| (uint64_t(1) << 6)  // ZMM{0 -> 15}.
						| (uint64_t(1) << 7); // ZMM{16 -> 31}.

			globalCpuFeatures.xcr0Mask = xcr0Mask;
			globalCpuFeatures.xsaveRegionSize = xsaveRegionSizeFor(xcr0Mask);
			debugLogger() << "thor: XSAVE region size is "
					<< globalCpuFeatures.xsaveRegionSize << " bytes" << frg::endlog;
		}

		if(common::x86::cpuid(0x80000007)[3] & (1 << 8)) {
			debugLogger() << "thor: CPUs support invariant TSC"
					<< frg::endlog;
			globalCpuFeatures.haveInvariantTsc = true;
		}else{
			debugLogger() << "thor: CPUs do not support invariant TSC!" << frg::endlog;
		}

		if(common::x86::cpuid(0x01)[2] & (1 << 24)) {
			debugLogger() << "thor: CPUs support TSC deadline mode"
					<< frg::endlog;
			globalCpuFeatures.haveTscDeadline = true;
		}else{
			debugLogger() << "thor: CPUs do not support TSC deadline mode!"
					<< frg::endlog;
		}

		bool noHwp = false;
		frg::array args = {
			frg::option{"thor.no-hwp", frg::store_true(noHwp)},
		};
		frg::parse_arguments(getKernelCmdline(), args);

		auto thermalPowerLeaf = common::x86::cpuid(0x6)[0];
		if(thermalPowerLeaf & (1 << 7)) {
			// IA32_PM_ENABLE sticks until reset, so firmware may already have turned HWP on.
			if(common::x86::rdmsr(common::x86::kMsrIa32PmEnable) & 1)
				infoLogger() << "thor: HWP was already enabled by firmware" << frg::endlog;

			if(noHwp) {
				infoLogger() << "thor: CPUs support HWP but it is disabled on the command line"
						<< frg::endlog;
			}else{
				debugLogger() << "thor: CPUs support HWP" << frg::endlog;
				globalCpuFeatures.haveHwp = true;
				globalCpuFeatures.haveHwpEpp = thermalPowerLeaf & (1 << 10);
			}
		}else{
			debugLogger() << "thor: CPUs do not support HWP!" << frg::endlog;
		}

		auto intelPmLeaf = common::x86::cpuid(0xA)[0];
		if(intelPmLeaf & 0xFF) {
			debugLogger() << "thor: CPUs support Intel performance counters"
					<< frg::endlog;
			globalCpuFeatures.profileFlags |= CpuFeatures::profileIntelSupported;
		}
		auto amdPmLeaf = common::x86::cpuid(0x8000'0001)[2];
		if(amdPmLeaf & (1 << 23)) {
			debugLogger() << "thor: CPUs support AMD performance counters"
					<< frg::endlog;
			globalCpuFeatures.profileFlags |= CpuFeatures::profileAmdSupported;
		}

		// Check that both VMX and EPT are supported.
		bool vmxSupported = [] () -> bool {
			// Test for VMX.
			if(!(common::x86::cpuid(0x1)[2] & (1 << 5)))
				return false;
			// Test for secondary processor-based controls.
			auto procBased = common::x86::rdmsr(0x482);
			if(!((procBased >> 32) & (1 << 31)))
				return false;
			// Test for EPT support and unrestricted guests.
			auto procBased2 = common::x86::rdmsr(0x48B);
			if(!((procBased2 >> 32) & (1 << 1)))
				return false;
			if(!((procBased2 >> 32) & (1 << 7)))
				return false;
			// Test if page walks of length 4 are supported by EPT.
			if(!(common::x86::rdmsr(0x48C) & (1 << 6)))
				return false;
			return true;
		}(); // Immediately invoked.

		bool svmSupported = [] () -> bool {
			auto leaf = common::x86::cpuid(common::x86::kCpuIndexExtendedFeatures);
			if(!(leaf[2] & (1 << 2)))
				return false; // SVM not supported

			auto svmFeatures = common::x86::cpuid(common::x86::kCpuIndexSvmFeatures)[3];
			auto vmCr = common::x86::rdmsr(common::x86::kMsrIndexVmCr);
			if(vmCr & (1 << 4)) { // VM_CR.SVMDIS
				if(svmFeatures & (1 << 2)) // CPUID.8000_000A.EDX[SVML]
					debugLogger() << "thor: SVM disabled by firmware (SVM Lock)" << frg::endlog;
				else
					debugLogger() << "thor: SVM disabled by firmware" << frg::endlog;
				return false;
			}

			if(!(svmFeatures & (1 << 0))) { // CPUID.8000_000A.EDX[NP]
				debugLogger() << "thor: NPT support is required for SVM" << frg::endlog;
				return false;
			}
			// SVM does not report the length of instructions that cause exits.
			// With NRIPS, the CPU saves the RIP of the next instruction instead.
			if(!(svmFeatures & (1 << 3))) { // CPUID.8000_000A.EDX[NRIPS]
				debugLogger() << "thor: NRIPS support is required for SVM" << frg::endlog;
				return false;
			}

			return true;
		}();

		if(vmxSupported) {
			debugLogger() << "thor: CPUs support VMX"
					<< frg::endlog;
			globalCpuFeatures.haveVmx = true;
		}else{
			debugLogger() << "thor: CPUs do not support VMX!" << frg::endlog;
		}

		if(svmSupported) {
			debugLogger() << "thor: CPUs support SVM"
					<< frg::endlog;
			globalCpuFeatures.haveSvm = true;
		}else{
			debugLogger() << "thor: CPUs do not support SVM!" << frg::endlog;
		}

		cpuFeaturesKnown = true;
	}
};

void doRunOnStack(void (*function) (void *, void *), void *sp, void *argument) {
	assert(!intsAreEnabled());

	cleanKasanShadow(reinterpret_cast<std::byte *>(sp) - UniqueKernelStack::kSize,
			UniqueKernelStack::kSize);
	asm volatile (
			"xor %%rbp, %%rbp\n"
			"mov %%rsp, %%rsi\n"
			"\tmov %2, %%rsp\n"
			"\tcall *%1\n"
			"\tud2"
			:
			: "D" (argument), "r" (function), "r" (sp)
			: "rbp", "rsi", "memory");
}

extern "C" void syscallStub();

// Set up the kernel GS segment.
THOR_KCOV_DISABLED void setupCpuContext(AssemblyCpuData *context) {
	common::x86::wrmsr(common::x86::kMsrIndexGsBase,
			reinterpret_cast<uint64_t>(context));
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

static initgraph::Task initBootProcessorTask{&globalInitEngine, "x86.init-boot-processor",
	initgraph::Requires{getCpuFeaturesKnownStage(),
		getApicDiscoveryStage(),
		// HPET is needed for local APIC timer calibration.
		getHpetInitializedStage()},
	initgraph::Entails{getFibersAvailableStage()},
	[] {
		// We need to fill in the boot APIC ID.
		// This cannot be done in setupBootCpuContext() as we need the APIC base first.
		cpuData.get().localApicId = getLocalApicId();
		debugLogger() << "Booting on CPU #" << cpuData.get().localApicId
				<< frg::endlog;

		initializeThisProcessor();
	}
};

void initializeThisProcessor() {
	auto cpuData = getCpuData();

	setCpuState(cpuData, CpuState::booting);

	// Allocate per-CPU areas.
	cpuData->dfStack = UniqueKernelStack::make();
	cpuData->nmiStack = UniqueKernelStack::make();
	cpuData->detachedStack = UniqueKernelStack::make();
	cpuData->idleStack = UniqueKernelStack::make();

	// if fred is supported / will be enabled do not embed data into the NMI stack
	if(!globalCpuFeatures.haveFred) {
		// We embed some data at the top of the NMI stack.
		// The NMI handler needs this data to enter a consistent kernel state.
		struct Embedded {
			AssemblyCpuData *expectedGs;
			uint64_t padding;
		} embedded{cpuData, 0};

		cpuData->nmiStack.embed<Embedded>(embedded);
	}

	// Setup our IST after the did the embedding.
	auto *tss = &cpuDescriptorTables.get().tss;
	tss->ist2 = (uintptr_t)cpuData->dfStack.basePtr();
	tss->ist3 = (uintptr_t)cpuData->nmiStack.basePtr();

	common::x86::Gdtr gdtr;
	gdtr.limit = 9 * 8;
	gdtr.pointer = cpuDescriptorTables.get().gdt;
	asm volatile ( "lgdt (%0)" : : "r"( &gdtr ) );

	asm volatile ( "pushq %0\n"
			"\rpushq $.L_reloadCs\n"
			"\rlretq\n"
			".L_reloadCs:" : : "i" (kSelKernelCode) );

	// We need a valid TSS in case an NMI or fault happens here.
	activateTss(tss);

	// Setup the syscall interface; this needs to be done early
	// in the case of FRED due to it sharing the IA32_STAR MSR
	if((common::x86::cpuid(common::x86::kCpuIndexExtendedFeatures)[3]
		& common::x86::kCpuFlagSyscall) == 0)
		panicLogger() << "CPU does not support the syscall instruction"
		<< frg::endlog;

	uint64_t efer = common::x86::rdmsr(common::x86::kMsrEfer);
	common::x86::wrmsr(common::x86::kMsrEfer,
					   efer | common::x86::kMsrSyscallEnable);

	common::x86::wrmsr(common::x86::kMsrLstar, (uintptr_t)&syscallStub);
	// Set user mode rpl bits to work around a qemu bug.
	common::x86::wrmsr(common::x86::kMsrStar, (uint64_t(kSelUserCompat) << 48)
	| (uint64_t(kSelKernelCode) << 32));
	// Mask interrupt and trap flag.
	common::x86::wrmsr(
		common::x86::kMsrFmask,
		0x100   // TF.
		| 0x200 // IF.
		| 0x400 // DF.
		| 0x40000 // AC.
	);

	// Setup the IDT. (or FRED if supported by the CPU)
	auto *idt = cpuDescriptorTables.get().idt;
	for(int i = 0; i < 256; i++)
		common::x86::makeIdt64NullGate(idt, i);

	if (globalCpuFeatures.haveFred) {
		// ensure SS has the proper segment for erets
		asm volatile ( "movw %w0, %%ss" : : "r" (kSelKernelData));

		// set the FRED RSPx MSRs for fault stacks
		common::x86::wrmsr(common::x86::kMsrFredRSP2, (uintptr_t)cpuData->dfStack.basePtr());
		common::x86::wrmsr(common::x86::kMsrFredRSP3, (uintptr_t)cpuData->nmiStack.basePtr());

		setupFred();
	} else {
		setupIdt(idt);
	}

	common::x86::Idtr idtr;
	idtr.limit = 256 * 16;
	idtr.pointer = idt;
	asm volatile ( "lidt (%0)" : : "r"( &idtr ) );

	// Enable the global page feature.
	{
		uint64_t cr4;
		asm volatile ("mov %%cr4, %0" : "=r" (cr4));
		cr4 |= uint32_t(1) << 7;
		asm volatile ("mov %0, %%cr4" : : "r" (cr4));
	}

	// Enable the wr{fs,gs}base instructions.
	// FIXME: does not seem to work under qemu
//	if(!(common::x86::cpuid(common::x86::kCpuIndexStructuredExtendedFeaturesEnum)[1]
//			& common::x86::kCpuFlagFsGsBase))
//		panicLogger() << "CPU does not support wrfsbase / wrgsbase"
//				<< frg::endlog;

//	uint64_t cr4;
//	asm volatile ( "mov %%cr4, %0" : "=r" (cr4) );
//	cr4 |= 0x10000;
//	asm volatile ( "mov %0, %%cr4" : : "r" (cr4) );

	// Enable the XSAVE instruction set and child features
	if(getGlobalCpuFeatures()->haveXsave) {
		uint64_t cr4;
		asm volatile ("mov %%cr4, %0" : "=r" (cr4));
		cr4 |= uint32_t(1) << 18; // Enable XSAVE and x{get, set}bv
		asm volatile ("mov %0, %%cr4" : : "r" (cr4));

		common::x86::wrxcr(0, getGlobalCpuFeatures()->xcr0Mask);

		// Validate that the pre-computed XSAVE size matches the CPU-reported XSAVE size.
		// This must happen *after* the xcr0 write.
		assert(common::x86::cpuid(0xD)[1] == getGlobalCpuFeatures()->xsaveRegionSize);
	}

	// Enable the SMAP extension.
	if(common::x86::cpuid(0x07)[1] & (uint32_t(1) << 20)) {
		debugLogger() << "thor: CPU supports SMAP" << frg::endlog;

		uint64_t cr4;
		asm volatile ("mov %%cr4, %0" : "=r" (cr4));
		cr4 |= uint32_t(1) << 21;
		asm volatile ("mov %0, %%cr4" : : "r" (cr4));

		asm volatile ("clac" : : : "memory");

		cpuData->haveSmap = true;
	}else{
		debugLogger() << "thor: CPU does not support SMAP!" << frg::endlog;
	}

	// Enable the SMEP extension.
	if(common::x86::cpuid(0x07)[1] & (uint32_t(1) << 7)) {
		debugLogger() << "thor: CPU supports SMEP" << frg::endlog;

		uint64_t cr4;
		asm volatile ("mov %%cr4, %0" : "=r" (cr4));
		cr4 |= uint32_t(1) << 20;
		asm volatile ("mov %0, %%cr4" : : "r" (cr4));
	}else{
		debugLogger() << "thor: CPU does not support SMEP!" << frg::endlog;
	}

	// Enable the UMIP extension.
	if(common::x86::cpuid(0x07)[2] & (uint32_t(1) << 2)) {
		debugLogger() << "thor: CPU supports UMIP" << frg::endlog;

		uint64_t cr4;
		asm volatile ("mov %%cr4, %0" : "=r" (cr4));
		cr4 |= uint32_t(1) << 11;
		asm volatile ("mov %0, %%cr4" : : "r" (cr4));
	}else{
		debugLogger() << "thor: CPU does not support UMIP!" << frg::endlog;
	}

	// Enable the PCID extension.
	bool pcidBit = common::x86::cpuid(0x01)[2] & (uint32_t(1) << 17);
	bool invpcidBit = common::x86::cpuid(0x07)[1] & (uint32_t(1) << 10);
	if(pcidBit && invpcidBit) {
		debugLogger() << "thor: CPU supports PCIDs" << frg::endlog;

		uint64_t cr4;
		asm volatile ("mov %%cr4, %0" : "=r" (cr4));
		cr4 |= uint32_t(1) << 17;
		asm volatile ("mov %0, %%cr4" : : "r" (cr4));

		cpuData->havePcids = true;
	}else if(pcidBit) {
		debugLogger() << "thor: CPU supports PCIDs but no INVPCID;"
				" will not use PCIDs!" << frg::endlog;
	}else{
		debugLogger() << "thor: CPU does not support PCIDs!" << frg::endlog;
	}

	// Enable hardware P-states.
	// Until this is done, the core runs at whatever fixed ratio firmware left in IA32_PERF_CTL,
	// i.e. without turbo or frequency scaling.
	if(getGlobalCpuFeatures()->haveHwp) {
		// IA32_PM_ENABLE is package scoped. Writing 1 again on a sibling is harmless.
		common::x86::wrmsr(common::x86::kMsrIa32PmEnable, 1);

		// IA32_HWP_CAPABILITIES is only readable once HWP is enabled.
		auto caps = common::x86::rdmsr(common::x86::kMsrIa32HwpCapabilities);
		cpuData->hwpHighestPerf = caps & 0xFF;
		cpuData->hwpGuaranteedPerf = (caps >> 8) & 0xFF;
		cpuData->hwpMostEfficientPerf = (caps >> 16) & 0xFF;
		cpuData->hwpLowestPerf = (caps >> 24) & 0xFF;

		infoLogger() << "thor: CPU #" << cpuData->cpuIndex << " HWP performance levels:"
				<< " highest " << (int)cpuData->hwpHighestPerf
				<< ", guaranteed " << (int)cpuData->hwpGuaranteedPerf
				<< ", most efficient " << (int)cpuData->hwpMostEfficientPerf
				<< ", lowest " << (int)cpuData->hwpLowestPerf << frg::endlog;

		// Allow the hardware to use the full range of performance levels.
		uint64_t request = uint64_t{cpuData->hwpLowestPerf}
				| (uint64_t{cpuData->hwpHighestPerf} << 8);
		// EPP selects the efficiency/performance preference on a scale
		// from 0 (prefer performance) to 255 (prefer energy efficiency).
		// Linux uses: 0 (performance), 0x80 (balanced performance), 0xC0 (balanced power), 0xFF (power).
		// TODO: We hard-code 0x80 here for now, let userspace override it in the future.
		if(getGlobalCpuFeatures()->haveHwpEpp)
			request |= uint64_t{0x80} << 24;
		common::x86::wrmsr(common::x86::kMsrIa32HwpRequest, request);
	}

	// Enable SVM or VMX if it is supported.
	if(getGlobalCpuFeatures()->haveVmx)
		cpuData->haveVirtualization = thor::vmx::vmxon();

	if(getGlobalCpuFeatures()->haveSvm)
		cpuData->haveVirtualization = thor::svm::init();

	// Setup the per-CPU work queue.
	postWorkQueueFibers(cpuData);

	initializeIdleStates();

	initLocalApicPerCpu();
}

// Generated by objcopy.
extern "C" uint8_t _binary_kernel_thor_arch_x86_trampoline_bin_start[];
extern "C" uint8_t _binary_kernel_thor_arch_x86_trampoline_bin_end[];

struct StatusBlock {
	StatusBlock *self; // Pointer to this struct in the higher half.
	unsigned int targetStage;
	unsigned int initiatorStage;
	unsigned int pml4;
	uintptr_t stack;
	void (*main)(StatusBlock *);
	CpuData *cpuContext;
};

static_assert(sizeof(StatusBlock) == 48, "Bad sizeof(StatusBlock)");

THOR_KCOV_DISABLED void secondaryMain(StatusBlock *statusBlock) {
	auto cpuContext = statusBlock->cpuContext;

	setupCpuContext(cpuContext);
	initializeThisProcessor();
	__atomic_store_n(&statusBlock->targetStage, 2, __ATOMIC_RELEASE);

	debugLogger() << "Hello world from CPU #" << getLocalApicId() << frg::endlog;

	resumeWorkQueueFibers(cpuContext);

	LoadBalancer::singleton().setOnline(cpuContext);
	setRcuOnline(cpuContext);
	setCpuState(cpuContext, CpuState::online);
	auto scheduler = &localScheduler.get();
	scheduler->update();
	scheduler->forceReschedule(false);
	scheduler->commitReschedule();
}

void bootSecondary(unsigned int apic_id, size_t cpuIndex) {
	if(disableSmp)
		return;

	// Run this function with interrupts disabled to prevent other IPIs (e.g., broadcast IPIs for shootdown)
	// from interfering with CPUs that are not fully online yet.
	auto irqLock = frg::guard(&irqMutex());

	// TODO: Allocate a page in low physical memory instead of hard-coding it.
	uintptr_t pma = 0x10000;

	// Copy the trampoline code into low physical memory.
	auto image_size = (uintptr_t)_binary_kernel_thor_arch_x86_trampoline_bin_end
			- (uintptr_t)_binary_kernel_thor_arch_x86_trampoline_bin_start;
	assert(image_size <= kPageSize);
	PageAccessor accessor{pma};
	memcpy(accessor.get(), _binary_kernel_thor_arch_x86_trampoline_bin_start, image_size);

	// Allocate a stack for the initialization code.
	constexpr size_t stack_size = 0x10000;
	void *stack_ptr = kernelAlloc->allocate(stack_size);

	auto *context = getCpuData(cpuIndex);
	context->localApicId = apic_id;

	// Participate in global TLB invalidation *before* paging is used by the target CPU.
	initializeAsidContext(context);

	// Setup a status block to communicate information to the AP.
	auto statusBlock = reinterpret_cast<StatusBlock *>(reinterpret_cast<char *>(accessor.get())
			+ (kPageSize - sizeof(StatusBlock)));
	debugLogger() << "status block accessed via: " << statusBlock << frg::endlog;

	statusBlock->self = statusBlock;
	statusBlock->targetStage = 0;
	statusBlock->initiatorStage = 0;
	statusBlock->pml4 = KernelPageSpace::global().rootTable();
	statusBlock->stack = (uintptr_t)stack_ptr + stack_size;
	statusBlock->main = &secondaryMain;
	statusBlock->cpuContext = context;

	// Send the IPI sequence that starts up the AP.
	// On modern processors INIT lets the processor enter the wait-for-SIPI state.
	// The BIOS is not involved in this process at all.
	infoLogger() << "thor: Booting AP " << apic_id << "." << frg::endlog;
	raiseInitAssertIpi(apic_id);
	pollSleepNano(10'000'000); // Wait for 10ms.

	// De-assert the INIT IPI.
	raiseInitDeassertIpi(apic_id);
	pollSleepNano(200'000); // Wait for 200us.

	// SIPI causes the processor to resume execution and resets CS:IP.
	// Intel suggets to send two SIPIs (probably for redundancy reasons).
	raiseStartupIpi(apic_id, pma);
	pollSleepNano(200'000); // Wait for 200us.
	raiseStartupIpi(apic_id, pma);
	pollSleepNano(200'000); // Wait for 200us.

	// Wait until the AP wakes up.
	while(__atomic_load_n(&statusBlock->targetStage, __ATOMIC_ACQUIRE) < 1) {
		pause();
	}
	debugLogger() << "thor: AP did wake up." << frg::endlog;

	// We only let the AP proceed after all IPIs have been sent.
	// This ensures that the AP does not execute boot code twice (e.g. in case
	// it already wakes up after a single SIPI).
	__atomic_store_n(&statusBlock->initiatorStage, 1, __ATOMIC_RELEASE);

	// Wait until the AP exits the boot code.
	while(__atomic_load_n(&statusBlock->targetStage, __ATOMIC_ACQUIRE) < 2) {
		pause();
	}
	debugLogger() << "thor: AP finished booting." << frg::endlog;
}

Error getEntropyFromCpu(void *buffer, size_t size) {
	using word_type = uint32_t;
	auto p = reinterpret_cast<char *>(buffer);

	if(!(common::x86::cpuid(0x7)[1] & (uint32_t(1) << 18)))
		return Error::noHardwareSupport;

	word_type word;
	auto rdseed = [&] () -> bool {
		// Do a maximal number of tries before we give up (e.g., due to broken firmware).
		for(int k = 0; k < 512; ++k) {
			bool success;
			asm volatile ("rdseed %0" : "=r"(word), "=@ccc"(success));
			if(success)
				return true;
		}
		return false;
	};

	size_t n = 0;

	// Generate all full words.
	size_t size_words = size & ~(sizeof(word_type) - 1);
	while(n < size_words) {
		if(!rdseed())
			return Error::hardwareBroken;
		memcpy(p + n, &word, sizeof(word_type));
		n += sizeof(word_type);
	}

	// Generate the last word.
	if(n < size) {
		assert(size - n < sizeof(word_type));
		if(!rdseed())
			return Error::hardwareBroken;
		memcpy(p + n, &word, size - n);
	}

	return Error::success;
}

} // namespace thor
