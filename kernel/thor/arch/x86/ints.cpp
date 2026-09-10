#include <cstdint>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/int-call.hpp>
#include <thor-internal/ipl.hpp>
#include <thor-internal/profile.hpp>
#include <thor-internal/thread.hpp>
#include <thor-internal/traps.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/arch/pmc-amd.hpp>
#include <thor-internal/arch/pmc-intel.hpp>
#include <thor-internal/arch/stack.hpp>
#include <thor-internal/arch/system.hpp>
#include <thor-internal/arch/pic.hpp>

extern char stubsPtr[], stubsLimit[];

extern "C" void earlyStubDivideByZero();
extern "C" void earlyStubOpcode();
extern "C" void earlyStubDouble();
extern "C" void earlyStubProtection();
extern "C" void earlyStubPage();

extern "C" void faultStubDivideByZero();
extern "C" void faultStubDebug();
extern "C" void faultStubBreakpoint();
extern "C" void faultStubOverflow();
extern "C" void faultStubBound();
extern "C" void faultStubOpcode();
extern "C" void faultStubNoFpu();
extern "C" void faultStubDouble();
extern "C" void faultStub9();
extern "C" void faultStubInvalidTss();
extern "C" void faultStubSegment();
extern "C" void faultStubStack();
extern "C" void faultStubProtection();
extern "C" void faultStubPage();
extern "C" void faultStub15();
extern "C" void faultStubFpuException();
extern "C" void faultStubAlignment();
extern "C" void faultStubMachineCheck();
extern "C" void faultStubSimdException();

extern "C" char thorRtIrqStubs[];

extern "C" void thorRtIsrLegacyIrq7();
extern "C" void thorRtIsrLegacyIrq15();

extern "C" void thorRtIpiShootdown();
extern "C" void thorRtIpiPing();
extern "C" void thorRtIpiCall();
extern "C" void thorRtPreemption();
extern "C" void thorRtSpurious();

extern "C" void nmiStub();

namespace thor {

static constexpr bool logEveryFault = false;
static constexpr bool logEveryPreemption = false;

uint32_t earlyGdt[3 * 2];
uint32_t earlyIdt[256 * 4];

extern "C" void handleEarlyDivideByZeroFault(void *rip) {
	panicLogger() << "Division by zero during boot\n"
			<< "Faulting IP: " << rip << frg::endlog;
}

extern "C" void handleEarlyOpcodeFault(void *rip) {
	panicLogger() << "Invalid opcode during boot\n"
			<< "Faulting IP: " << rip << frg::endlog;
}

extern "C" void handleEarlyDoubleFault(uint64_t errcode, void *rip) {
	(void)errcode;

	panicLogger() << "Double fault during boot\n"
			<< "Faulting IP: " << rip << frg::endlog;
}

extern "C" void handleEarlyProtectionFault(uint64_t errcode, void *rip) {
	panicLogger() << "Protection fault during boot\n"
			<< "Segment: " << errcode << "\n"
			<< "Faulting IP: " << rip << frg::endlog;
}

extern "C" void handleEarlyPageFault(uint64_t errcode, void *rip) {
	(void)errcode;
	uintptr_t pfAddress;
	asm volatile ("mov %%cr2, %0" : "=r" (pfAddress));

	panicLogger() << "Page fault at " << (void *)pfAddress << " during boot\n"
			<< "Faulting IP: " << rip << frg::endlog;
}

void setupEarlyInterruptHandlers() {
	// setup the gdt
	common::x86::makeGdtNullSegment(earlyGdt, 0);
	// for simplicity, match the layout with the "real" gdt we load later
	common::x86::makeGdtCode64SystemSegment(earlyGdt, 1);
	common::x86::makeGdtFlatData32SystemSegment(earlyGdt, 2);

	common::x86::Gdtr gdtr;
	gdtr.limit = 3 * 8;
	gdtr.pointer = earlyGdt;
	asm volatile ( "lgdt (%0)" : : "r"( &gdtr ) );

	asm volatile ( "pushq $0x8\n"
			"\rpushq $.L_reloadEarlyCs\n"
			"\rlretq\n"
			".L_reloadEarlyCs:" );

	// setup the idt
	common::x86::makeIdt64IntSystemGate(earlyIdt, 0, 0x8, (void *)&earlyStubDivideByZero, 0);
	common::x86::makeIdt64IntSystemGate(earlyIdt, 6, 0x8, (void *)&earlyStubOpcode, 0);
	common::x86::makeIdt64IntSystemGate(earlyIdt, 8, 0x8, (void *)&earlyStubDouble, 0);
	common::x86::makeIdt64IntSystemGate(earlyIdt, 13, 0x8, (void *)&earlyStubProtection, 0);
	common::x86::makeIdt64IntSystemGate(earlyIdt, 14, 0x8, (void *)&earlyStubPage, 0);

	common::x86::Idtr idtr;
	idtr.limit = 256 * 16;
	idtr.pointer = earlyIdt;
	asm volatile ( "lidt (%0)" : : "r"( &idtr ) : "memory" );
}

extern "C" bool thorFredEnabled = false;
extern "C" void thorFredRing3Entry();

void setupFred() {
	thorFredEnabled = true;

	// set FRED entry point
	common::x86::wrmsr(common::x86::kMsrFredConfig, (uint64_t)thorFredRing3Entry);

	// set stack levels for #DF & #NMI
	#define FRED_STKLVL(n, lvl)  ((uint64_t)(lvl) << ((n) * 2))
	common::x86::wrmsr(common::x86::kMsrFredStkLvl, FRED_STKLVL(8, 2) | FRED_STKLVL(2, 3));
	#undef FRED_STKLVL

	// enable CR4.FRED (bit 32)
	uint64_t cr4 = 0;
	asm volatile ("mov %%cr4, %0" : "=r" (cr4));
	cr4 |= 1ull << 32;
	asm volatile ("mov %0, %%cr4" : : "r" (cr4));

	infoLogger() << "thor: FRED enabled on CPU #" << getCpuData()->cpuIndex << frg::endlog;
}

void setupIdt(uint32_t *table) {
	using common::x86::makeIdt64IntSystemGate;
	using common::x86::makeIdt64IntUserGate;
	
	int fault_selector = kSelKernelCode;
	makeIdt64IntSystemGate(table, 0, fault_selector, (void *)&faultStubDivideByZero, 0);
	makeIdt64IntSystemGate(table, 1, fault_selector, (void *)&faultStubDebug, 0);
	makeIdt64IntUserGate(table, 3, fault_selector, (void *)&faultStubBreakpoint, 0);
	makeIdt64IntSystemGate(table, 4, fault_selector, (void *)&faultStubOverflow, 0);
	makeIdt64IntSystemGate(table, 5, fault_selector, (void *)&faultStubBound, 0);
	makeIdt64IntSystemGate(table, 6, fault_selector, (void *)&faultStubOpcode, 0);
	makeIdt64IntSystemGate(table, 7, fault_selector, (void *)&faultStubNoFpu, 0);
	makeIdt64IntSystemGate(table, 8, fault_selector, (void *)&faultStubDouble, 2);
	makeIdt64IntSystemGate(table, 9, fault_selector, (void *)&faultStub9, 0);
	makeIdt64IntSystemGate(table, 10, fault_selector, (void *)&faultStubInvalidTss, 0);
	makeIdt64IntSystemGate(table, 11, fault_selector, (void *)&faultStubSegment, 0);
	makeIdt64IntSystemGate(table, 12, fault_selector, (void *)&faultStubStack, 0);
	makeIdt64IntSystemGate(table, 13, fault_selector, (void *)&faultStubProtection, 0);
	makeIdt64IntSystemGate(table, 14, fault_selector, (void *)&faultStubPage, 0);
	makeIdt64IntSystemGate(table, 15, fault_selector, (void *)&faultStub15, 0);
	makeIdt64IntSystemGate(table, 16, fault_selector, (void *)&faultStubFpuException, 0);
	makeIdt64IntSystemGate(table, 17, fault_selector, (void *)&faultStubAlignment, 0);
	makeIdt64IntSystemGate(table, 18, fault_selector, (void *)&faultStubMachineCheck, 0);
	makeIdt64IntSystemGate(table, 19, fault_selector, (void *)&faultStubSimdException, 0);

	int irq_selector = kSelKernelCode;
	makeIdt64IntSystemGate(table, 39, irq_selector, (void *)&thorRtIsrLegacyIrq7, 0);
	makeIdt64IntSystemGate(table, 47, irq_selector, (void *)&thorRtIsrLegacyIrq15, 0);

	for(int i = 0; i < numIrqSlots; i++)
		makeIdt64IntSystemGate(table, irqSlotVectorBase + i, irq_selector,
				thorRtIrqStubs + i * irqStubSize, 0);

	makeIdt64IntSystemGate(table, 0xF0, irq_selector, (void *)&thorRtIpiShootdown, 0);
	makeIdt64IntSystemGate(table, 0xF1, irq_selector, (void *)&thorRtIpiPing, 0);
	makeIdt64IntSystemGate(table, 0xF2, irq_selector, (void *)&thorRtIpiCall, 0);
	makeIdt64IntSystemGate(table, lapicTimerVector, irq_selector, (void *)&thorRtPreemption, 0);
	makeIdt64IntSystemGate(table, lapicSpuriousVector, irq_selector,
			(void *)&thorRtSpurious, 0);

	int nmi_selector = kSelKernelCode;
	makeIdt64IntSystemGate(table, 2, nmi_selector, (void *)&nmiStub, 3);
}

bool inStub(uintptr_t ip) {
	return ip >= (uintptr_t)stubsPtr && ip < (uintptr_t)stubsLimit;
}

void handlePageFault(FaultImageAccessor image, uintptr_t address, Word errorCode);
void handleOtherFault(FaultImageAccessor image, Interrupt fault);
void handleIrq(IrqImageAccessor image, IrqPin *irq);
void handleSyscall(SyscallImageAccessor image);

void handleDebugFault(FaultImageAccessor image) {
	debugLogger() << "thor: Debug fault at ip: " << (void *)*image.ip() << frg::endlog;
}

extern "C" void onPlatformFault(FaultImageAccessor image, int number) {
	iplSave(*image.iplState());

	// For page faults: we need to get the address *before* re-enabling IRQs.
	uintptr_t pfAddress;
	if(number == 14)
		asm volatile ("mov %%cr2, %0" : "=r" (pfAddress));

	uint16_t cs = *image.cs();
	if(logEveryFault)
		infoLogger() << "Fault #" << number << ", from cs: 0x" << frg::hex_fmt(cs)
				<< ", ip: " << (void *)*image.ip() << frg::endlog;

	if(inStub(*image.ip()))
		panicLogger() << "Fault #" << number
				<< " in stub section, cs: 0x" << frg::hex_fmt(cs)
				<< ", ip: " << (void *)*image.ip() << frg::endlog;
	if(cs != kSelUserCode && cs != kSelKernelCode)
		panicLogger() << "Fault #" << number
				<< ", from unexpected cs: 0x" << frg::hex_fmt(cs)
				<< ", ip: " << (void *)*image.ip() << "\n"
				<< "Error code: 0x" << frg::hex_fmt(*image.code())
				<< ", SS: 0x" << frg::hex_fmt(*image.ss())
				<< ", RSP: " << (void *)*image.sp() << frg::endlog;
	if(!(*image.rflags() & 0x200))
		panicLogger() << "Fault #" << number
				<< ", with IF=0, cs: 0x" << frg::hex_fmt(cs)
				<< ", ip: " << (void *)*image.ip() << "\n"
				<< "Error code: 0x" << frg::hex_fmt(*image.code())
				<< ", SS: 0x" << frg::hex_fmt(*image.ss())
				<< ", RSP: " << (void *)*image.sp() << frg::endlog;

	disableUserAccess();
	assert(getCurrentThread());

	switch(number) {
	case 0: {
		iplEnterContext(ipl::exceptional, *image.iplState());
		enableInts();

		handleOtherFault(image, kIntrDivByZero);
	} break;
	case 1: {
		iplEnterContext(ipl::maximal, *image.iplState());

		handleDebugFault(image);
	} break;
	case 3: {
		iplEnterContext(ipl::exceptional, *image.iplState());
		enableInts();

		handleOtherFault(image, kIntrBreakpoint);
	} break;
	case 6: {
		iplEnterContext(ipl::exceptional, *image.iplState());
		enableInts();

		handleOtherFault(image, kIntrIllegalInstruction);
	} break;
	case 13: {
		iplEnterContext(ipl::exceptional, *image.iplState());
		enableInts();

		handleOtherFault(image, kIntrGeneralFault);
	} break;
	case 14: {
		iplEnterContext(ipl::exceptional, *image.iplState());
		enableInts();

		handlePageFault(image, pfAddress, *image.code());
	} break;
	default:
		panicLogger() << "Unexpected fault number " << number
				<< ", from cs: 0x" << frg::hex_fmt(cs)
				<< ", ip: " << (void *)*image.ip() << "\n"
				<< "Error code: 0x" << frg::hex_fmt(*image.code())
				<< ", SS: 0x" << frg::hex_fmt(*image.ss())
				<< ", RSP: " << (void *)*image.sp() << frg::endlog;
	}

	disableInts();
	iplRaise(ipl::maximal);

	// This fault may have woken up threads on this CPU.
	// See Scheduler::resume() for details.
	if (image.inUserMode()) {
		auto thisThread = getCurrentThread();
		assert(thisThread);

		checkThreadPreemption();

		if (thisThread->checkConditions()) {
			iplDemoteContext(ipl::passive);
			enableInts();

			StatelessIrqLock irqLock(frg::dont_lock);
			handleThreadReturnToUserMode(image, irqLock);
			irqLock.release();
		}
	} else {
		checkThreadPreemption(image);
	}

	iplLeaveContext(*image.iplState());
}

extern "C" void onPlatformIrq(IrqImageAccessor image, int number) {
	iplSave(*image.iplState());
	iplEnterContext(ipl::interrupt, *image.iplState());

	if(inStub(*image.ip()))
		panicLogger() << "IRQ " << number
				<< " in stub section, cs: 0x" << frg::hex_fmt(*image.cs())
				<< ", ip: " << (void *)*image.ip() << frg::endlog;

	uint16_t cs = *image.cs();
	assert(cs == kSelKernelCode || cs == kSelUserCode);

	assert(!irqMutex().nesting());
	disableUserAccess();

	handleIrq(image, irqSlots.get().slots[number].pin());

	if (image.inUserMode()) {
		auto thisThread = getCurrentThread();
		assert(thisThread);

		localScheduler.get().checkPreemption();

		if (thisThread->checkConditions()) {
			iplDemoteContext(ipl::passive);
			enableInts();

			StatelessIrqLock irqLock(frg::dont_lock);
			handleThreadReturnToUserMode(image, irqLock);
			irqLock.release();
		}
	} else {
		localScheduler.get().checkPreemption(image);
	}

	iplLeaveContext(*image.iplState());
}

extern "C" void onPlatformLegacyIrq(IrqImageAccessor image, int number) {
	iplSave(*image.iplState());
	iplEnterContext(ipl::interrupt, *image.iplState());

	if(inStub(*image.ip()))
		panicLogger() << "IRQ " << number
				<< " in stub section, cs: 0x" << frg::hex_fmt(*image.cs())
				<< ", ip: " << (void *)*image.ip() << frg::endlog;

	uint16_t cs = *image.cs();
	assert(cs == kSelKernelCode || cs == kSelUserCode);

	assert(!irqMutex().nesting());
	disableUserAccess();

	if(checkLegacyPicIsr(number)) {
		urgentLogger() << "thor: Spurious IRQ " << number
				<< " of legacy PIC" << frg::endlog;
	}else{
		urgentLogger() << "thor: Ignoring non-spurious IRQ " << number
				<< " of legacy PIC" << frg::endlog;
	}

	iplLeaveContext(*image.iplState());
}

extern "C" void onPlatformPreemption(IrqImageAccessor image) {
	iplSave(*image.iplState());
	iplEnterContext(ipl::interrupt, *image.iplState());

	if(inStub(*image.ip()))
		panicLogger() << "Preemption IRQ"
				" in stub section, cs: 0x" << frg::hex_fmt(*image.cs())
				<< ", ip: " << (void *)*image.ip() << frg::endlog;

	uint16_t cs = *image.cs();
	if(logEveryPreemption)
		infoLogger() << "thor [CPU " << getLocalApicId()
				<< "]: Preemption from cs: 0x" << frg::hex_fmt(cs)
				<< ", ip: " << (void *)*image.ip() << frg::endlog;

	assert(cs == kSelKernelCode || cs == kSelUserCode);

	assert(!irqMutex().nesting());
	disableUserAccess();

	handleTimerInterrupt();

	getCpuData()->heartbeat.fetch_add(1, std::memory_order_relaxed);

	acknowledgeIrq(0);

	if (image.inUserMode()) {
		auto thisThread = getCurrentThread();
		assert(thisThread);

		localScheduler.get().checkPreemption();

		if (thisThread->checkConditions()) {
			iplDemoteContext(ipl::passive);
			enableInts();

			StatelessIrqLock irqLock(frg::dont_lock);
			handleThreadReturnToUserMode(image, irqLock);
			irqLock.release();
		}
	} else {
		localScheduler.get().checkPreemption(image);
	}

	iplLeaveContext(*image.iplState());
}

extern "C" void onPlatformSpurious(IrqImageAccessor image) {
	iplSave(*image.iplState());
	iplEnterContext(ipl::interrupt, *image.iplState());

	if(inStub(*image.ip()))
		panicLogger() << "Spurious IRQ"
				" in stub section, cs: 0x" << frg::hex_fmt(*image.cs())
				<< ", ip: " << (void *)*image.ip() << frg::endlog;

	uint16_t cs = *image.cs();
	assert(cs == kSelKernelCode || cs == kSelUserCode);

	assert(!irqMutex().nesting());
	disableUserAccess();

	// Spurious interrupts never set ISR bits; they must not be acknowledged with an EOI.
	infoLogger() << "thor [CPU " << getLocalApicId()
			<< "]: Spurious local APIC interrupt" << frg::endlog;

	iplLeaveContext(*image.iplState());
}

extern "C" void onPlatformSyscall(SyscallImageAccessor image) {
	iplSave(*image.iplState());
	assert(image.iplState()->current == ipl::passive);

	assert(!irqMutex().nesting());
	enableInts();

	// Note that user access is disabled here since it is set in the FMASK MSR.

	handleSyscall(image);

	disableInts();
	iplRaise(ipl::interrupt);

	// This syscall may have woken up threads on this CPU.
	// See Scheduler::resume() for details.
	{
		auto thisThread = getCurrentThread();
		assert(thisThread);

		checkThreadPreemption();

		if (thisThread->checkConditions()) {
			iplDemoteContext(ipl::passive);
			enableInts();

			StatelessIrqLock irqLock(frg::dont_lock);
			handleThreadReturnToUserMode(image, irqLock);
			irqLock.release();
		}
	}

	iplLower(ipl::interrupt, ipl::passive);
}

extern "C" void onPlatformShootdown(IrqImageAccessor image) {
	iplSave(*image.iplState());
	iplEnterContext(ipl::interrupt, *image.iplState());

	if(inStub(*image.ip()))
		panicLogger() << "Shootdown IPI"
				<< " in stub section, cs: 0x" << frg::hex_fmt(*image.cs())
				<< ", ip: " << (void *)*image.ip() << frg::endlog;

	uint16_t cs = *image.cs();
	assert(cs == kSelKernelCode || cs == kSelUserCode);

	assert(!irqMutex().nesting());
	disableUserAccess();

	for(auto &binding : asidData.get()->bindings)
		binding.shootdown();

	asidData.get()->globalBinding.shootdown();

	acknowledgeIpi();

	if (image.inUserMode()) {
		auto thisThread = getCurrentThread();
		assert(thisThread);

		localScheduler.get().checkPreemption();

		if (thisThread->checkConditions()) {
			iplDemoteContext(ipl::passive);
			enableInts();

			StatelessIrqLock irqLock(frg::dont_lock);
			handleThreadReturnToUserMode(image, irqLock);
			irqLock.release();
		}
	} else {
		localScheduler.get().checkPreemption(image);
	}

	iplLeaveContext(*image.iplState());
}

extern "C" void onPlatformPing(IrqImageAccessor image) {
	iplSave(*image.iplState());
	iplEnterContext(ipl::interrupt, *image.iplState());

	if(inStub(*image.ip()))
		panicLogger() << "Ping IPI"
				<< " in stub section, cs: 0x" << frg::hex_fmt(*image.cs())
				<< ", ip: " << (void *)*image.ip() << frg::endlog;

	uint16_t cs = *image.cs();
	assert(cs == kSelKernelCode || cs == kSelUserCode);

	assert(!irqMutex().nesting());
	disableUserAccess();

	acknowledgeIpi();

	auto *scheduler = &localScheduler.get();
	scheduler->forcePreemptionCall();

	if (image.inUserMode()) {
		auto thisThread = getCurrentThread();
		assert(thisThread);

		scheduler->checkPreemption();

		if (thisThread->checkConditions()) {
			iplDemoteContext(ipl::passive);
			enableInts();

			StatelessIrqLock irqLock(frg::dont_lock);
			handleThreadReturnToUserMode(image, irqLock);
			irqLock.release();
		}
	} else {
		scheduler->checkPreemption(image);
	}

	iplLeaveContext(*image.iplState());
}

extern "C" void onPlatformCall(IrqImageAccessor image) {
	iplSave(*image.iplState());
	iplEnterContext(ipl::interrupt, *image.iplState());

	if(inStub(*image.ip()))
		panicLogger() << "Call IPI"
				<< " in stub section, cs: 0x" << frg::hex_fmt(*image.cs())
				<< ", ip: " << (void *)*image.ip() << frg::endlog;

	uint16_t cs = *image.cs();
	assert(cs == kSelKernelCode || cs == kSelUserCode);

	assert(!irqMutex().nesting());
	disableUserAccess();

	acknowledgeIpi();

	SelfIntCallBase::runScheduledCalls();

	if (image.inUserMode()) {
		auto thisThread = getCurrentThread();
		assert(thisThread);

		localScheduler.get().checkPreemption();

		if (thisThread->checkConditions()) {
			iplDemoteContext(ipl::passive);
			enableInts();

			StatelessIrqLock irqLock(frg::dont_lock);
			handleThreadReturnToUserMode(image, irqLock);
			irqLock.release();
		}
	} else {
		localScheduler.get().checkPreemption(image);
	}

	iplLeaveContext(*image.iplState());
}

namespace {
	void interruptIseq(IseqContext *iseq, NmiImageAccessor image) {
		if (!(iseq->state & IseqContext::STATE_TX)) [[likely]]
			return;

		// Always set the interrupted flag.
		iseq->state |= IseqContext::STATE_INTERRUPTED;

		// If IP is within an IseqRegion, advance IP.
		auto *region = iseq->region;
		if (region) {
			if (*image.ip() >= reinterpret_cast<uintptr_t>(region->startIp)
					&& *image.ip() < reinterpret_cast<uintptr_t>(region->commitIp)) {
				*image.ip() = reinterpret_cast<uintptr_t>(region->interruptIp);
			}
		}
	}
}

extern "C" void onPlatformNmi(NmiImageAccessor image, uint64_t expectedGs) {
	// If we interrupted user space or a kernel stub, we might need to update GS.
	auto gs = common::x86::rdmsr(common::x86::kMsrIndexGsBase);
	common::x86::wrmsr(common::x86::kMsrIndexGsBase, expectedGs);

	iplSave(*image.iplState());
	iplEnterContext(ipl::maximal, *image.iplState());

	auto cpuData = getCpuData();
	auto *iseq = cpuData->iseqPtr;

	interruptIseq(iseq, image);

	IseqContext ownIseq;
	cpuData->iseqPtr = &ownIseq;

	// Each sample is an array of uintptr_t.
	// Element 0 stores the number of entries and flags.
	// Elements >0 store the stack trace IPs.
	auto emitProfileSample = [&] {
		constexpr size_t maxDepth = 15;
		uintptr_t buffer[maxDepth + 1];
		size_t n = 0;
		buffer[1 + n++] = *image.ip();
#ifdef THOR_HAS_FRAME_POINTERS
		// We can (obviously) only backtrace in the higher half.
		// Also, we cannot backtrace if we did not make it out of the entry stubs yet
		// since the entry stubs may still run with userspace RBP.
		if (inHigherHalf(*image.ip()) && !inStub(*image.ip())) {
			walkStack(reinterpret_cast<void *>(*image.bp()), [&] (uintptr_t ip) {
				if(n < maxDepth)
					buffer[1 + n++] = ip;
			});
		}
#endif
		uint32_t flags{0};
		if (!(*image.rflags() & 0x200))
			flags |= 1;
		buffer[0] = n | (static_cast<uint64_t>(flags) << 32);
		cpuData->localProfileRing->enqueue(buffer, (1 + n) * sizeof(uintptr_t));
	};

	bool explained = false;
	auto pmcMechanism = cpuData->profileMechanism.load(std::memory_order_acquire);
	if(pmcMechanism == ProfileMechanism::intelPmc && checkIntelPmcOverflow()) {
		emitProfileSample();
		// Note: on Intel, the PMI is automatically masked on raises.
		LocalApicContext::clearPmi();
		setIntelPmc();
		explained = true;
	}else if(pmcMechanism == ProfileMechanism::amdPmc && checkAmdPmcOverflow()) {
		emitProfileSample();
		setAmdPmc();
		explained = true;
	}

	if(!explained) [[unlikely]] {
		infoLogger() << "thor [CPU " << getLocalApicId()
				<< "]: NMI triggered at heartbeat "
				<< cpuData->heartbeat.load(std::memory_order_relaxed) << frg::endlog;
		infoLogger() << "thor [CPU " << getLocalApicId()
				<< "]: From CS: 0x" << frg::hex_fmt(*image.cs())
				<< ", IP: " << (void *)*image.ip() << frg::endlog;
		infoLogger() << "thor [CPU " << getLocalApicId()
				<< "]: RFLAGS is " << (void *)*image.rflags() << frg::endlog;

		if(!getLocalApicId())
			sendGlobalNmi();
	}

	cpuData->iseqPtr = iseq;

	iplLeaveContext(*image.iplState());

	// Restore the old value of GS.
	common::x86::wrmsr(common::x86::kMsrIndexGsBase,
			reinterpret_cast<uintptr_t>(gs));
}

extern "C" void onFredEvent(Frame* frame) {
	uint64_t vector = (frame->ss >> 32) & 0xFF;
	uint8_t  type   = (frame->ss >> 48) & 0xF;
	bool     isUser = (frame->cs & 3) == 3;

	// type 7 is syscall
	if(isUser && (type == 7)) {
		onPlatformSyscall(SyscallImageAccessor{frame});
		return;
	}

	// type 2 is NMI
	if (type == 2) {
		assert(vector == 2);
		uint64_t currentGs = common::x86::rdmsr(common::x86::kMsrIndexGsBase);
		onPlatformNmi(NmiImageAccessor{frame}, currentGs);
		return;
	}

	// type 3 is hardware exception
	if (type == 3) {
		assert(vector <= 31);
		onPlatformFault(FaultImageAccessor{frame}, vector);
		return;
	}

	// type 0 is external interrupts
	if (type == 0) {
		if (vector == 39) {
			onPlatformLegacyIrq(IrqImageAccessor{frame}, 7);
			return;
		}
		if (vector == 47) {
			onPlatformLegacyIrq(IrqImageAccessor{frame}, 15);
			return;
		}

		if (vector >= irqSlotVectorBase && vector < irqSlotVectorBase + numIrqSlots) {
			onPlatformIrq(IrqImageAccessor{frame}, vector - irqSlotVectorBase);
			return;
		}

		if (vector == 0xF0) {
			onPlatformShootdown(IrqImageAccessor{frame});
			return;
		}
		if (vector == 0xF1) {
			onPlatformPing(IrqImageAccessor{frame});
			return;
		}
		if (vector == 0xF2) {
			onPlatformCall(IrqImageAccessor{frame});
			return;
		}
		if (vector == lapicTimerVector) {
			onPlatformPreemption(IrqImageAccessor{frame});
			return;
		}
		if (vector == lapicSpuriousVector) {
			onPlatformSpurious(IrqImageAccessor{frame});
			return;
		}

		panicLogger() << "FRED: Unhandled external interrupt" << frg::endlog;
	}

	// type 4 is oftware interrupt (INT n) (ignored)
	if (type == 6) {
		return;
	}

	// type 5 is privileged software exception (INT1)
	if (type == 5) {
		panicLogger() << "FRED: unexpected privileged software exception" << frg::endlog;
	}


	// type 6 is software exception (INT3 or INTO) (ignored)
	if (type == 6) {
		return;
	}

	panicLogger() << "FRED: unexpected event type" << frg::endlog;
}

extern "C" void enableIntsAndHaltForever();

void suspendSelf() {
	assert(!intsAreEnabled());
	enableIntsAndHaltForever();
}

} // namespace thor

