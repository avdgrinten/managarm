#pragma once

#include <atomic>

#include <arch/mem_space.hpp>
#include <x86/machine.hpp>
#include <initgraph.hpp>
#include <thor-internal/arch/asm.h>
#include <thor-internal/irq.hpp>
#include <thor-internal/timer.hpp>
#include <thor-internal/types.hpp>
#include <thor-internal/util.hpp>

namespace thor {

// --------------------------------------------------------
// IRQ slots
// --------------------------------------------------------

static inline constexpr int numIrqSlots = THOR_NUM_IRQ_SLOTS;
static inline constexpr size_t irqStubSize = THOR_IRQ_STUB_SIZE;

// Represents a slot in the CPU's interrupt table.
// Slots might be global or per-CPU.
struct IrqSlot {
	// Links an IrqPin to this slot.
	// From now on all IRQ raises will go to this IrqPin.
	void link(IrqPin *pin);

	IrqPin *pin() {
		return _pin.load(std::memory_order_acquire);
	}

private:
	std::atomic<IrqPin *> _pin{nullptr};
};

extern IrqSlot globalIrqSlots[numIrqSlots];

// --------------------------------------------------------
// Local APIC management
// --------------------------------------------------------

// Vectors of interrupts that the local APIC generates itself.
static inline constexpr unsigned int lapicTimerVector = 0xFE;
static inline constexpr unsigned int lapicSpuriousVector = 0xFF;

struct ApicRegisterSpace {
	constexpr ApicRegisterSpace()
	: _x2apic{false}, _mem_base(0) { }

	ApicRegisterSpace(bool x2apic, void *base = nullptr)
	: _x2apic{x2apic}, _mem_base{reinterpret_cast<uintptr_t>(base)} { }

	template<typename RT>
	void store(RT r, typename RT::rep_type value) const {
		auto v = static_cast<typename RT::bits_type>(value);
		if(_x2apic) {
			auto msr = x2apic_msr_base + (r.offset() >> 4);
			common::x86::wrmsr(msr, v);
		} else {
			auto p = reinterpret_cast<typename RT::bits_type *>(_mem_base + r.offset());
			arch::mem_ops<typename RT::bits_type>::store(p, v);
		}
	}

	template<typename RT>
	typename RT::rep_type load(RT r) const {
		if(_x2apic) {
			auto msr = x2apic_msr_base + (r.offset() >> 4);
			return static_cast<typename RT::rep_type>(common::x86::rdmsr(msr));
		} else {
			auto p = reinterpret_cast<const typename RT::bits_type *>(_mem_base + r.offset());
			auto b = arch::mem_ops<typename RT::bits_type>::load(p);
			return static_cast<typename RT::rep_type>(b);
		}
	}

	bool isUsingX2apic() const {
		return _x2apic;
	}

private:
	bool _x2apic;
	uintptr_t _mem_base;
	static constexpr uint32_t x2apic_msr_base = 0x800;
};

struct LocalApicContext {
	constexpr LocalApicContext() = default;

	static void clearPmi();

	bool useTscMode = false;
	bool timersAreCalibrated = false;

	// TSC tick duration stored in ns.
	// Used for the timestamp -> monotonic clock time conversion.
	Pow2Fraction<Rounding::down> tscTickDuration;
	// The frequency is used to program the TSC timer based on a deadline in ns.
	// Round up such that the TSC timer fires after the deadline, not before.
	Pow2Fraction<Rounding::up> tscTickFreq;

	// Frequency of the local APIC timer in nHz.
	// Round up such that the APIC timer fires after the deadline, not before.
	Pow2Fraction<Rounding::up> lapicTickFreq;
};

initgraph::Stage *getApicDiscoveryStage();

void initLocalApicPerCpu();

uint32_t getLocalApicId();

void calibrateApicTimer();

void acknowledgeIpi();

void raiseInitAssertIpi(uint32_t dest_apic_id);

void raiseInitDeassertIpi(uint32_t dest_apic_id);

void raiseStartupIpi(uint32_t dest_apic_id, uint32_t page);

void sendGlobalNmi();

// --------------------------------------------------------
// MSI management
// --------------------------------------------------------

smarter::shared_ptr<MsiPin> allocateApicMsi(frg::string<KernelAlloc> name);

// --------------------------------------------------------
// I/O APIC management
// --------------------------------------------------------

void setupIoApic(int apic_id, int gsi_base, PhysicalAddr address);

// --------------------------------------------------------
// Legacy PIC management
// --------------------------------------------------------

void remapLegacyPic(int offset);
void maskLegacyPic();

bool checkLegacyPicIsr(int irq);

// --------------------------------------------------------
// General functions
// --------------------------------------------------------

void acknowledgeIrq(int irq);

smarter::shared_ptr<IrqPin> getGlobalSystemIrq(size_t n);

} // namespace thor
