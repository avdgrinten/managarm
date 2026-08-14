#include <arch/bits.hpp>
#include <arch/io_space.hpp>
#include <arch/mem_space.hpp>
#include <arch/register.hpp>
#include <thor-internal/arch/pic.hpp>
#include <thor-internal/arch/hpet.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/fiber.hpp>
#include <initgraph.hpp>
#include <thor-internal/irq.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/ostrace.hpp>
#include <thor-internal/arch-generic/ints.hpp>
#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/arch-generic/timer.hpp>
#include <thor-internal/acpi/acpi.hpp>
#include <frg/hash_map.hpp>

namespace thor {

namespace {
	constexpr bool debugTimer = false;
	constexpr bool debugIoApicMasking = false;
}

inline constexpr arch::bit_register<uint32_t> lApicId(0x0020);
inline constexpr arch::scalar_register<uint32_t> lApicEoi(0x00B0);
inline constexpr arch::bit_register<uint32_t> lApicSpurious(0x00F0);
inline constexpr arch::bit_register<uint32_t> lApicIcrLow(0x0300);
inline constexpr arch::bit_register<uint32_t> lApicIcrHigh(0x0310);
inline constexpr arch::bit_register<uint64_t> lX2ApicIcr{0x0300};
inline constexpr arch::bit_register<uint32_t> lApicLvtTimer(0x0320);
inline constexpr arch::bit_register<uint32_t> lApicLvtPerfCount(0x0340);
inline constexpr arch::bit_register<uint32_t> lApicLvtLocal0(0x0350);
inline constexpr arch::bit_register<uint32_t> lApicLvtLocal1(0x0360);
inline constexpr arch::scalar_register<uint32_t> lApicInitCount(0x0380);
inline constexpr arch::scalar_register<uint32_t> lApicCurCount(0x0390);

// lApicId registers
inline constexpr arch::field<uint32_t, uint8_t> apicId(24, 8);
inline constexpr arch::field<uint32_t, uint8_t> x2ApicId(0, 8);

// lApicSpurious registers
inline constexpr arch::field<uint32_t, uint8_t> apicSpuriousVector(0, 8);
inline constexpr arch::field<uint32_t, bool> apicSpuriousSwEnable(8, 1);
inline constexpr arch::field<uint32_t, bool> apicSpuriousFocusProcessor(9, 1);
inline constexpr arch::field<uint32_t, bool> apicSpuriousEoiBroadcastSuppression(12, 1);

// lApicIcrLow registers
inline constexpr arch::field<uint32_t, uint8_t> apicIcrLowVector(0, 8);
inline constexpr arch::field<uint32_t, uint8_t> apicIcrLowDelivMode(8, 3);
inline constexpr arch::field<uint32_t, bool> apicIcrLowDestMode(11, 1);
inline constexpr arch::field<uint32_t, bool> apicIcrLowDelivStatus(12, 1);
inline constexpr arch::field<uint32_t, bool> apicIcrLowLevel(14, 1);
inline constexpr arch::field<uint32_t, bool> apicIcrLowTriggerMode(15, 1);
inline constexpr arch::field<uint32_t, uint8_t> apicIcrLowShorthand(18, 2);

// lApicIcrHigh registers
inline constexpr arch::field<uint32_t, uint8_t> apicIcrHighDestField(24, 8);

// lX2ApicIcr registers
inline constexpr arch::field<uint64_t, uint8_t> x2apicIcrLowVector(0, 8);
inline constexpr arch::field<uint64_t, uint8_t> x2apicIcrLowDelivMode(8, 3);
inline constexpr arch::field<uint64_t, bool> x2apicIcrLowDestMode(11, 1);
inline constexpr arch::field<uint64_t, bool> x2apicIcrLowDelivStatus(12, 1);
inline constexpr arch::field<uint64_t, bool> x2apicIcrLowLevel(14, 1);
inline constexpr arch::field<uint64_t, bool> x2apicIcrLowTriggerMode(15, 1);
inline constexpr arch::field<uint64_t, uint8_t> x2apicIcrLowShorthand(18, 2);
inline constexpr arch::field<uint64_t, uint32_t> x2apicIcrHighDestField(32, 32);

// lApicLvtTimer registers
inline constexpr arch::field<uint32_t, uint8_t> apicLvtVector(0, 8);
inline constexpr arch::field<uint32_t, bool> apicLvtMask(16, 1);
inline constexpr arch::field<uint32_t, uint8_t> apicLvtMode(8, 3);
inline constexpr arch::field<uint32_t, uint8_t> apicLvtTimerMode(17, 2);

ApicRegisterSpace picBase;

namespace {
	bool getLocalApicIsr(unsigned int vector) {
		arch::scalar_register<uint32_t> isrRegister{0x100 + 0x10 * (vector >> 5)};
		return picBase.load(isrRegister) & (1 << (vector & 31));
	}

	bool getLocalApicTmr(unsigned int vector) {
		arch::scalar_register<uint32_t> tmrRegister{0x180 + 0x10 * (vector >> 5)};
		return picBase.load(tmrRegister) & (1 << (vector & 31));
	}

	bool getLocalApicIrr(unsigned int vector) {
		arch::scalar_register<uint32_t> irrRegister{0x200 + 0x10 * (vector >> 5)};
		return picBase.load(irrRegister) & (1 << (vector & 31));
	}
}

enum {
	kModelLegacy = 1,
	kModelApic = 2
};

static int picModel = kModelLegacy;

uint64_t getRawTimestampCounter() {
	uint32_t lsw, msw;
	asm volatile ("lfence; rdtsc" : "=a"(lsw), "=d"(msw));
	return (static_cast<uint64_t>(msw) << 32)
			| static_cast<uint64_t>(lsw);
}

// --------------------------------------------------------
// IrqSlot
// --------------------------------------------------------

constinit IrqSlot globalIrqSlots[numIrqSlots];

void IrqSlot::link(IrqPin *pin) {
	assert(!_pin.load(std::memory_order_relaxed));
	_pin.store(pin, std::memory_order_release);
}

// --------------------------------------------------------
// Local APIC timer
// --------------------------------------------------------

extern PerCpu<LocalApicContext> apicContext;
THOR_DEFINE_PERCPU(apicContext);

namespace {
	LocalApicContext *localApicContext() {
		return &apicContext.get();
	}
}

void setTimerDeadline(frg::optional<uint64_t> deadline) {
	assert(localApicContext()->timersAreCalibrated);

	if(localApicContext()->useTscMode) {
		// TODO: Re-enable once ostrace can emit from IRQ context without allocating.
		//ostrace::emit(ostEvtArmCpuTimer);

		if(!deadline) {
			common::x86::wrmsr(common::x86::kMsrIa32TscDeadline, 0);
			return;
		}

		uint64_t ticks = localApicContext()->timerFreq * *deadline;
		common::x86::wrmsr(common::x86::kMsrIa32TscDeadline, ticks);
		if(debugTimer)
			infoLogger() << "thor [CPU " << getLocalApicId() << "]: Setting TSC deadline to "
					<< ticks << frg::endlog;
	}else{
		if(!deadline) {
			picBase.store(lApicInitCount, 0);
			return;
		}

		auto now = getClockNanos();
		uint64_t ticks;
		if(*deadline < now) {
			if(debugTimer)
				infoLogger() << "thor [CPU " << getLocalApicId()
						<< "]: Setting single tick timer" << frg::endlog;
			ticks = 1;
		}else{
			if(debugTimer)
				infoLogger() << "thor [CPU " << getLocalApicId() << "]: Setting timer "
						<< ((*deadline - now) / 1000) << " us in the future" << frg::endlog;
			ticks = localApicContext()->timerFreq * (*deadline - now);
			if(!ticks)
				ticks = 1;
		}
		picBase.store(lApicInitCount, ticks);
	}
}

void LocalApicContext::clearPmi() {
	picBase.store(lApicLvtPerfCount, apicLvtMode(4));
}

// --------------------------------------------------------
// Local PIC management
// --------------------------------------------------------

initgraph::Stage *getApicDiscoveryStage() {
	static initgraph::Stage s{&globalInitEngine, "x86.apic-discovered"};
	return &s;
}

static initgraph::Task discoverApicTask{&globalInitEngine, "x86.discover-apic",
	initgraph::Entails{getApicDiscoveryStage()},
	[] {
		uint64_t msr = common::x86::rdmsr(common::x86::kMsrLocalApicBase);
		msr |= (1 << 11); // Enable APIC

		bool haveX2apic = false;
		if(common::x86::cpuid(0x01)[2] & (uint32_t(1) << 21)){
			debugLogger() << "thor: CPU supports x2apic" << frg::endlog;
			msr |= (1 << 10);
			haveX2apic = true;
		} else {
			debugLogger() << "thor: CPU does not support x2apic" << frg::endlog;
		}

		common::x86::wrmsr(common::x86::kMsrLocalApicBase, msr);

		// TODO: We really only need a single page.
		auto register_ptr = KernelVirtualMemory::global().allocate(0x10000);
		// TODO: Intel SDM specifies that we should mask out all
		// bits > the physical address limit of the msr.
		// For now we just assume that they are zero.
		KernelPageSpace::global().mapSingle4k(VirtualAddr(register_ptr), msr & ~PhysicalAddr{0xFFF},
				page_access::write, CachingMode::null);
		picBase = ApicRegisterSpace(haveX2apic, register_ptr);
	}
};

void initLocalApicPerCpu() {
	uint64_t msr = common::x86::rdmsr(common::x86::kMsrLocalApicBase);
	msr |= (1 << 11); // Enable APIC

	if(picBase.isUsingX2apic()){
		assert(common::x86::cpuid(0x01)[2] & (uint32_t(1) << 21));
		msr |= (1 << 10);
	}

	common::x86::wrmsr(common::x86::kMsrLocalApicBase, msr);

	auto dumpLocalInt = [&] (int index) {
		auto regstr = (index == 0 ? lApicLvtLocal0 : lApicLvtLocal1);
		auto lvt = picBase.load(regstr);
		infoLogger() << "thor: CPU #" << getLocalApicId()
				<< " LINT " << index << " mode is " << (lvt & apicLvtMode)
				<< ", it is " << ((lvt & apicLvtMask) ? "masked" : "not masked")
				<< frg::endlog;
	};

	// Enable the local APIC.
	uint32_t spurious_vector = 0x81;
	picBase.store(lApicSpurious, apicSpuriousVector(spurious_vector)
			| apicSpuriousSwEnable(true));

	dumpLocalInt(0);
	dumpLocalInt(1);

	if(getGlobalCpuFeatures()->haveInvariantTsc
			&& getGlobalCpuFeatures()->haveTscDeadline)
		localApicContext()->useTscMode = true;

	// Setup a timer interrupt for scheduling.
	if(localApicContext()->useTscMode) {
		picBase.store(lApicLvtTimer, apicLvtVector(0xFF) | apicLvtTimerMode(2));
		// The SDM requires this to order MMIO and MSR writes.
		asm volatile ("mfence" : : : "memory");
	}else{
		picBase.store(lApicLvtTimer, apicLvtVector(0xFF));
	}

	// Setup the PMI.
	picBase.store(lApicLvtPerfCount, apicLvtMode(4));

	calibrateApicTimer();
}

uint32_t getLocalApicId() {
	if (picBase.isUsingX2apic()) {
		return picBase.load(lApicId) & x2ApicId;
	} else {
		return picBase.load(lApicId) & apicId;
	}
}

extern ClockSource *hpetClockSource;

void calibrateApicTimer() {
	constexpr uint64_t millis = 100;
	constexpr uint64_t nanos = millis * 1'000'000;

	// Calibrate the local APIC timer.
	if (!localApicContext()->useTscMode) {
		picBase.store(lApicInitCount, 0xFFFFFFFF);
		pollSleepNano(nanos);
		uint32_t elapsed = 0xFFFFFFFF
				- picBase.load(lApicCurCount);
		picBase.store(lApicInitCount, 0);

		localApicContext()->timerFreq
			= computeFreqFraction(elapsed, nanos);

		infoLogger() << "thor: Local APIC ticks/ms: "
				<< (elapsed / millis)
				<< " on CPU #" << getCpuData()->cpuIndex << frg::endlog;
	}

	// Calibrate the TSC.
	if (getCpuData() == getCpuData(0) || !getGlobalCpuFeatures()->haveInvariantTsc) {
		auto tscStart = getRawTimestampCounter();
		pollSleepNano(nanos);
		auto tscElapsed = getRawTimestampCounter() - tscStart;

		localApicContext()->tscInverseFreq
			= computeFreqFraction(nanos, tscElapsed);
		if (localApicContext()->useTscMode) {
			localApicContext()->timerFreq
				= computeFreqFraction(tscElapsed, nanos);
		}

		infoLogger() << "thor: TSC ticks/ms: " << (tscElapsed / millis)
					<< " on CPU #" << getCpuData()->cpuIndex << frg::endlog;
	} else {
		// Linux assumes invariant TSC to be globally synchronized.
		localApicContext()->tscInverseFreq = apicContext.getFor(0).tscInverseFreq;
		if (localApicContext()->useTscMode) {
			localApicContext()->timerFreq = apicContext.getFor(0).timerFreq;
		}
	}

	localApicContext()->timersAreCalibrated = true;
}

static initgraph::Task assessTimersTask{&globalInitEngine, "x86.assess-timers",
	initgraph::Requires{getHpetInitializedStage()},
	initgraph::Entails{getTaskingAvailableStage()},
	[] {
		if(!getGlobalCpuFeatures()->haveInvariantTsc) {
			infoLogger() << "thor: No invariant TSC; using HPET as system clock source"
					<< frg::endlog;

		}
	}
};

uint64_t getClockNanos() {
	assert(localApicContext()->timersAreCalibrated);
	if(getGlobalCpuFeatures()->haveInvariantTsc) [[likely]] {
		return localApicContext()->tscInverseFreq * getRawTimestampCounter();
	} else {
		return hpetClockSource->currentNanos();
	}
}

void acknowledgeIpi() {
	picBase.store(lApicEoi, 0);
}

void raiseInitAssertIpi(uint32_t dest_apic_id) {
	if(picBase.isUsingX2apic()){
		picBase.store(lX2ApicIcr, x2apicIcrLowDelivMode(5)
			| x2apicIcrLowLevel(true) | x2apicIcrLowTriggerMode(true) | x2apicIcrHighDestField(dest_apic_id));
	} else {
		picBase.store(lApicIcrHigh, apicIcrHighDestField(dest_apic_id));
		// DM:init = 5, Level:assert = 1, TM:Level = 1
		picBase.store(lApicIcrLow, apicIcrLowDelivMode(5)
				| apicIcrLowLevel(true) | apicIcrLowTriggerMode(true));
		while(picBase.load(lApicIcrLow) & apicIcrLowDelivStatus) {
			// Wait for IPI delivery.
		}
	}
}

void raiseInitDeassertIpi(uint32_t dest_apic_id) {
	if(picBase.isUsingX2apic()) {
		picBase.store(lX2ApicIcr, x2apicIcrLowDelivMode(5)
			| x2apicIcrLowTriggerMode(true) | x2apicIcrHighDestField(dest_apic_id));
	} else {
		picBase.store(lApicIcrHigh, apicIcrHighDestField(dest_apic_id));
		// DM:init = 5, TM:Level = 1
		picBase.store(lApicIcrLow, apicIcrLowDelivMode(5)
				| apicIcrLowTriggerMode(true));
		while(picBase.load(lApicIcrLow) & apicIcrLowDelivStatus) {
			// Wait for IPI delivery.
		}
	}
}

void raiseStartupIpi(uint32_t dest_apic_id, uint32_t page) {
	assert((page % 0x1000) == 0);
	uint32_t vector = page / 0x1000; // determines the startup code page
	if(picBase.isUsingX2apic()) {
		picBase.store(lX2ApicIcr, x2apicIcrLowVector(vector)
				| x2apicIcrLowDelivMode(6) | x2apicIcrHighDestField(dest_apic_id));
	} else {
		picBase.store(lApicIcrHigh, apicIcrHighDestField(dest_apic_id));
		// DM:startup = 6
		picBase.store(lApicIcrLow, apicIcrLowVector(vector)
				| apicIcrLowDelivMode(6));
		while(picBase.load(lApicIcrLow) & apicIcrLowDelivStatus) {
			// Wait for IPI delivery.
		}
	}
}

void sendShootdownIpi() {
	if(picBase.isUsingX2apic()) {
		picBase.store(lX2ApicIcr, x2apicIcrLowVector(0xF0) | x2apicIcrLowDelivMode(0)
				| x2apicIcrLowLevel(true) | x2apicIcrLowShorthand(2) | x2apicIcrHighDestField(0));
	} else {
		picBase.store(lApicIcrHigh, apicIcrHighDestField(0));
		picBase.store(lApicIcrLow, apicIcrLowVector(0xF0) | apicIcrLowDelivMode(0)
				| apicIcrLowLevel(true) | apicIcrLowShorthand(2));
		while(picBase.load(lApicIcrLow) & apicIcrLowDelivStatus) {
			// Wait for IPI delivery.
		}
	}
}

void sendPingIpi(CpuData *dstData) {
	auto apic = dstData->localApicId;
//	infoLogger() << "thor [CPU" << getLocalApicId() << "]: Sending ping" << frg::endlog;
	if(picBase.isUsingX2apic()) {
		picBase.store(lX2ApicIcr, x2apicIcrLowVector(0xF1) | x2apicIcrLowDelivMode(0)
				| x2apicIcrLowLevel(true) | x2apicIcrLowShorthand(0) | x2apicIcrHighDestField(apic));
	} else {
		picBase.store(lApicIcrHigh, apicIcrHighDestField(apic));
		picBase.store(lApicIcrLow, apicIcrLowVector(0xF1) | apicIcrLowDelivMode(0)
				| apicIcrLowLevel(true) | apicIcrLowShorthand(0));
		while(picBase.load(lApicIcrLow) & apicIcrLowDelivStatus) {
			// Wait for IPI delivery.
		}
	}
}

void sendSelfCallIpi() {
	auto apic = getCpuData()->localApicId;
	unsigned int vec = 0xF2;
	if(picBase.isUsingX2apic()) {
		picBase.store(lX2ApicIcr, x2apicIcrLowVector(vec) | x2apicIcrLowDelivMode(0)
				| x2apicIcrLowLevel(true) | x2apicIcrLowShorthand(0) | x2apicIcrHighDestField(apic));
	} else {
		picBase.store(lApicIcrHigh, apicIcrHighDestField(apic));
		picBase.store(lApicIcrLow, apicIcrLowVector(vec) | apicIcrLowDelivMode(0)
				| apicIcrLowLevel(true) | apicIcrLowShorthand(0));
		while(picBase.load(lApicIcrLow) & apicIcrLowDelivStatus) {
			// Wait for IPI delivery.
		}
	}
}

void sendGlobalNmi() {
	// Send the NMI to all /other/ CPUs but not to the current one.
	if(picBase.isUsingX2apic()) {
		picBase.store(lX2ApicIcr, x2apicIcrLowVector(0) | x2apicIcrLowDelivMode(4)
				| x2apicIcrLowLevel(true) | x2apicIcrLowShorthand(3) | x2apicIcrHighDestField(0));
	} else {
		picBase.store(lApicIcrHigh, apicIcrHighDestField(0));
		picBase.store(lApicIcrLow, apicIcrLowVector(0) | apicIcrLowDelivMode(4)
				| apicIcrLowLevel(true) | apicIcrLowShorthand(3));
		while(picBase.load(lApicIcrLow) & apicIcrLowDelivStatus) {
			// Wait for IPI delivery.
		}
	}
}

// --------------------------------------------------------
// MSI management
// --------------------------------------------------------

namespace {
	IrqSpinlock irqAllocationLock;
	static bool irqSlotAllocated[numIrqSlots]{};

	std::optional<int> allocateIrqSlot() {
		auto guard = frg::guard(&irqAllocationLock);

		for(int i = 0; i < numIrqSlots; i++) {
			if(irqSlotAllocated[i])
				continue;
			irqSlotAllocated[i] = true;
			return i;
		}

		return std::nullopt;
	}

	struct ApicMsiPin final : MsiPin {
		ApicMsiPin(frg::string<KernelAlloc> name, unsigned int vector)
		: MsiPin{std::move(name)}, vector_{vector} { }

		IrqStrategy program(TriggerMode mode, Polarity) override {
			assert(mode == TriggerMode::edge);
			return irq_strategy::endOfInterrupt;
		}

		void mask() override {
			// TODO: This may be worth implementing (but it is not needed for correctness).
		}

		void unmask() override {
			// TODO: This may be worth implementing (but it is not needed for correctness).
		}

		void endOfInterrupt() override {
			acknowledgeIrq(0);
		}

		uint64_t getMessageAddress() override {
			return 0xFEE00000;
		}

		uint32_t getMessageData() override {
			return vector_;
		}

	private:
		unsigned int vector_;
	};
}

smarter::shared_ptr<MsiPin> allocateApicMsi(frg::string<KernelAlloc> name) {
	auto maybeSlotIndex = allocateIrqSlot();
	if (!maybeSlotIndex)
		return nullptr;
	auto slotIndex = *maybeSlotIndex;

	// Create an IRQ pin for the MSI.
	auto pin = createIrqPin<ApicMsiPin>(std::move(name), 64 + slotIndex);
	pin->configure(IrqConfiguration{
		.trigger = TriggerMode::edge,
		.polarity = Polarity::high
	});

	infoLogger() << "thor: Allocating IRQ slot " << slotIndex
			<< " to " << pin->name() << frg::endlog;
	globalIrqSlots[slotIndex].link(pin.get());

	// Leak a reference until IrqPin teardown exists;
	// otherwise the slot dangles once the last sink goes away.
	pin.policy().increment();

	return pin;
}

// --------------------------------------------------------
// I/O APIC management
// --------------------------------------------------------

inline constexpr arch::scalar_register<uint32_t> apicIndex(0x00);
inline constexpr arch::scalar_register<uint32_t> apicData(0x10);

namespace pin_word1 {
	inline constexpr arch::field<uint32_t, unsigned int> vector(0, 8);
	inline constexpr arch::field<uint32_t, unsigned int> deliveryMode(8, 3);
//	inline constexpr arch::field<uint32_t, bool> logicalMode(11, 1);
	inline constexpr arch::field<uint32_t, bool> deliveryStatus(12, 1);
	inline constexpr arch::field<uint32_t, bool> activeLow(13, 1);
	inline constexpr arch::field<uint32_t, bool> remoteIrr(14, 1);
	inline constexpr arch::field<uint32_t, bool> levelTriggered(15, 1);
	inline constexpr arch::field<uint32_t, bool> masked(16, 1);
};

namespace pin_word2 {
	inline constexpr arch::field<uint32_t, unsigned int> destination(24, 8);
};

namespace {
	enum {
		kIoApicId = 0,
		kIoApicVersion = 1,
		kIoApicInts = 16,
	};

	struct IoApic {
	public:
		struct Pin final : IrqPin {
			Pin(IoApic *chip, unsigned int index);

			void dumpHardwareState() override;
			IrqStrategy program(TriggerMode mode, Polarity polarity) override;
			void mask() override;
			void unmask() override;
			void endOfInterrupt() override;

		private:
			IoApic *_chip;
			unsigned int _index;
			int _vector = -1;

			// The following variables store the current pin configuration.
			bool _levelTriggered;
			bool _activeLow;
		};

		IoApic(int apic_id, arch::mem_space space);

		size_t pinCount();

		smarter::shared_ptr<IrqPin> accessPin(size_t n);

	private:
		// Must only be called with _mutex taken.
		uint32_t _loadRegister(uint32_t index) {
			_space.store(apicIndex, index);
			return _space.load(apicData);
		}

		// Must only be called with _mutex taken.
		void _storeRegister(uint32_t index, uint32_t value) {
			_space.store(apicIndex, index);
			_space.store(apicData, value);
		}

		int _apicId;
		arch::mem_space _space;
		size_t _numPins;
		// TODO: Replace by dyn_array?
		smarter::shared_ptr<Pin> *_pins;

		// Protects the I/O APIC MMIO registers.
		frg::ticket_spinlock _mutex;
	};

	static frg::string<KernelAlloc> buildName(int apic_id, unsigned int index) {
		return frg::string<KernelAlloc>{*kernelAlloc, "io-apic."}
				+ frg::to_allocated_string(*kernelAlloc, apic_id)
				+ frg::string<KernelAlloc>{*kernelAlloc, ":"}
				+ frg::to_allocated_string(*kernelAlloc, index);
	}

	IoApic::Pin::Pin(IoApic *chip, unsigned int index)
	: IrqPin{buildName(chip->_apicId, index)}, _chip{chip}, _index{index} { }

	void IoApic::Pin::dumpHardwareState() {
		assert(!intsAreEnabled());
		auto lock = frg::guard(&_chip->_mutex);

		infoLogger() << "thor: Local APIC state of vector " << _vector << ":"
				<< " ISR: " << (int)getLocalApicIsr(_vector)
				<< ", TMR: " << (getLocalApicTmr(_vector) ? "level" : "edge")
				<< ", IRR: " << (int)getLocalApicIrr(_vector)
				<< frg::endlog;

		arch::bit_value<uint32_t> word1{_chip->_loadRegister(kIoApicInts + _index * 2)};
		infoLogger() << "thor: Configuration of pin " << name() << ": "
				<< ((word1 & pin_word1::levelTriggered) ? "level" : "edge")
				<< "-triggered, active-"
				<< ((word1 & pin_word1::activeLow) ? "low" : "high")
				<< frg::endlog;
		if(_levelTriggered != (word1 & pin_word1::levelTriggered))
			urgentLogger() << "thor: Trigger mode does not match software state!"
					<< frg::endlog;
		if(_activeLow != (word1 & pin_word1::activeLow))
			urgentLogger() << "thor: Trigger mode does not match software state!"
					<< frg::endlog;
		infoLogger() << "thor: I/O APIC state:"
				<< " mask: " << (int)(word1 & pin_word1::masked)
				<< ", delivery status: " << (int)(word1 & pin_word1::deliveryStatus)
				<< ", remote IRR: " << (int)(word1 & pin_word1::remoteIrr)
				<< frg::endlog;
	}

	IrqStrategy IoApic::Pin::program(TriggerMode mode, Polarity polarity) {
		IrqStrategy strategy;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&_chip->_mutex);

			if(mode == TriggerMode::edge) {
				_levelTriggered = false;
				strategy = irq_strategy::maskable | irq_strategy::endOfInterrupt;
			}else{
				assert(mode == TriggerMode::level);
				_levelTriggered = true;
				strategy = irq_strategy::maskable | irq_strategy::maskInService | irq_strategy::endOfInterrupt;
			}

			if(polarity == Polarity::high) {
				_activeLow = false;
			}else{
				assert(polarity == Polarity::low);
				_activeLow = true;
			}
		}

		// Allocate an IRQ vector for the I/O APIC pin.
		if(_vector == -1) {
			auto maybeSlotIndex = allocateIrqSlot();
			if (!maybeSlotIndex)
				panicLogger() << "thor: Could not allocate interrupt vector for "
						<< name() << frg::endlog;
			auto slotIndex = *maybeSlotIndex;
			globalIrqSlots[slotIndex].link(this);
			_vector = 64 + slotIndex;
		}

		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&_chip->_mutex);

			_chip->_storeRegister(kIoApicInts + _index * 2 + 1,
					static_cast<uint32_t>(pin_word2::destination(0)));
			_chip->_storeRegister(kIoApicInts + _index * 2,
					static_cast<uint32_t>(pin_word1::vector(_vector)
					| pin_word1::deliveryMode(0) | pin_word1::levelTriggered(_levelTriggered)
					| pin_word1::activeLow(_activeLow)));
		}
		return strategy;
	}

	void IoApic::Pin::mask() {
		if (debugIoApicMasking)
			infoLogger() << "thor: Masking pin " << _index << frg::endlog;
		assert(!intsAreEnabled());
		auto lock = frg::guard(&_chip->_mutex);

		_chip->_storeRegister(kIoApicInts + _index * 2,
				static_cast<uint32_t>(pin_word1::vector(_vector)
				| pin_word1::deliveryMode(0) | pin_word1::levelTriggered(_levelTriggered)
				| pin_word1::activeLow(_activeLow) | pin_word1::masked(true)));

		// Dummy load from the I/O APIC to ensure that the mask has taken effect.
		// Without this, we encounter innocuous but annoying races on some hardware:
		// since (x2)APIC EOIs are not necessarily serializing, we observe that the
		// I/O APIC submits IRQs to the local APIC even *after* they have been masked
		// in the I/O APIC.
		_chip->_loadRegister(kIoApicInts + _index * 2);
	}

	void IoApic::Pin::unmask() {
		if (debugIoApicMasking)
			infoLogger() << "thor: Unmasking pin " << _index << frg::endlog;
		assert(!intsAreEnabled());
		auto lock = frg::guard(&_chip->_mutex);

		_chip->_storeRegister(kIoApicInts + _index * 2,
				static_cast<uint32_t>(pin_word1::vector(_vector)
				| pin_word1::deliveryMode(0) | pin_word1::levelTriggered(_levelTriggered)
				| pin_word1::activeLow(_activeLow)));
	}

	void IoApic::Pin::endOfInterrupt() {
		acknowledgeIrq(0);
	}

	IoApic::IoApic(int apic_id, arch::mem_space space)
	: _apicId(apic_id), _space{std::move(space)} {
		_numPins = ((_loadRegister(kIoApicVersion) >> 16) & 0xFF) + 1;
		infoLogger() << "thor: I/O APIC " << apic_id << " supports "
				<< _numPins << " pins" << frg::endlog;

		_pins = frg::construct_n<smarter::shared_ptr<Pin>>(*kernelAlloc, _numPins);
		for(size_t i = 0; i < _numPins; i++) {
			_pins[i] = createIrqPin<Pin>(this, i);

			// Dump interesting configurations.
			arch::bit_value<uint32_t> current{_loadRegister(kIoApicInts + i * 2)};
			if(!(current & pin_word1::masked))
				infoLogger() << "    Pin " << i << " was not masked by BIOS."
						<< frg::endlog;

			// Mask all interrupts before they are configured.
			_storeRegister(kIoApicInts + i * 2, static_cast<uint32_t>(pin_word1::masked(true)));
		}
	}

	size_t IoApic::pinCount() {
		return _numPins;
	}

	smarter::shared_ptr<IrqPin> IoApic::accessPin(size_t n) {
		return _pins[n];
	}
}

void setupIoApic(int apic_id, int gsi_base, PhysicalAddr address) {
	// TODO: We really only need a single page.
	auto register_ptr = KernelVirtualMemory::global().allocate(0x10000);
	KernelPageSpace::global().mapSingle4k(VirtualAddr(register_ptr), address,
			page_access::write, CachingMode::null);

	picModel = kModelApic;

	auto apic = frg::construct<IoApic>(*kernelAlloc, apic_id, arch::mem_space{register_ptr});
	for(size_t i = 0; i < apic->pinCount(); i++) {
		auto pin = apic->accessPin(i);
		acpi::setGlobalSystemIrq(gsi_base + i, pin);
	}

	KernelFiber::run([=] {
		while(true) {
			for(size_t i = 0; i < apic->pinCount(); ++i)
				apic->accessPin(i)->warnIfPending();

			KernelFiber::asyncBlockCurrent(generalTimerEngine()->sleepFor(500'000'000));
		}
	});
}

// --------------------------------------------------------
// Legacy PIC management
// --------------------------------------------------------

static initgraph::Task setupPicTask{&globalInitEngine, "x86.setup-legacy-pic",
	initgraph::Entails{getTaskingAvailableStage()},
	[] {
		// TODO: managarm crashes on bochs if we do not remap the legacy PIC.
		// we need to debug that and find the cause of this problem.
		remapLegacyPic(32);
		maskLegacyPic();
	}
};

void ioWait() { }

enum LegacyPicRegisters {
	kPic1Command = 0x20,
	kPic1Data = 0x21,
	kPic2Command = 0xA0,
	kPic2Data = 0xA1
};

enum LegacyPicFlags {
	kIcw1Icw4 = 0x01,
	kIcw1Single = 0x02,
	kIcw1Interval4 = 0x04,
	kIcw1Level = 0x08,
	kIcw1Init = 0x10,

	kIcw4Mode8086 = 0x01,
	kIcw4Auto = 0x02,
	kIcw4BufSlave = 0x08,
	kIcw4BufMaster = 0x0C,
	kIcw4Sfnm = 0x10,

	kOcw3ReadIsr = 0x0B,

	kPicEoi = 0x20
};

void remapLegacyPic(int offset) {
	// save masks
	uint8_t a1 = common::x86::ioInByte(kPic1Data);
	uint8_t a2 = common::x86::ioInByte(kPic2Data);

	// start initilization
	common::x86::ioOutByte(kPic1Command, kIcw1Init | kIcw1Icw4);
	ioWait();
	common::x86::ioOutByte(kPic2Command, kIcw1Init | kIcw1Icw4);
	ioWait();
	common::x86::ioOutByte(kPic1Data, offset);
	ioWait();
	common::x86::ioOutByte(kPic2Data, offset + 8);
	ioWait();

	// setup cascade
	common::x86::ioOutByte(kPic1Data, 4);
	ioWait();
	common::x86::ioOutByte(kPic2Data, 2);
	ioWait();

	common::x86::ioOutByte(kPic1Data, kIcw4Mode8086);
	ioWait();
	common::x86::ioOutByte(kPic2Data, kIcw4Mode8086);
	ioWait();

	// restore saved masks
	common::x86::ioOutByte(kPic1Data, a1);
	common::x86::ioOutByte(kPic2Data, a2);
}

void maskLegacyPic() {
	common::x86::ioOutByte(kPic1Data, 0xFF);
	common::x86::ioOutByte(kPic2Data, 0xFF);
}

bool checkLegacyPicIsr(int irq) {
	if(irq < 8) {
		common::x86::ioOutByte(kPic1Command, kOcw3ReadIsr);
		auto isr = common::x86::ioInByte(kPic1Command);
		return isr & (1 << irq);
	}else{
		assert(irq < 16);
		common::x86::ioOutByte(kPic2Command, kOcw3ReadIsr);
		auto isr = common::x86::ioInByte(kPic2Command);
		return isr & (1 << (irq - 8));
	}
}

// --------------------------------------------------------
// General functions
// --------------------------------------------------------

// TODO: Split this function in two: One for the legacy PIC and one for APIC.
void acknowledgeIrq(int irq) {
	if(picModel == kModelApic) {
		picBase.store(lApicEoi, 0);
	}else if(picModel == kModelLegacy) {
		if(irq >= 8)
			common::x86::ioOutByte(kPic2Command, kPicEoi);
		common::x86::ioOutByte(kPic1Command, kPicEoi);
	}else{
		assert(!"Illegal PIC model");
	}
}

} // namespace thor
