#include <frg/scope_exit.hpp>
#include <stddef.h>
#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/arch/system.hpp>
#include <thor-internal/arch/timer.hpp>
#include <thor-internal/arch/trap.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/arch-generic/timer.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/timer.hpp>
#include <thor-internal/schedule.hpp>
#include <initgraph.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/arch/gic.hpp>
#include <thor-internal/arch/gic_v2.hpp>
#include <thor-internal/arch/gic_v3.hpp>
#include <thor-internal/dtb/dtb.hpp>
#include <thor-internal/util.hpp>
#include <uacpi/acpi.h>
#include <uacpi/tables.h>

namespace thor {

// Timer tick duration stored in ns, and its reciprocal (= its frequency).
constinit Pow2Fraction<Rounding::down> timerTickDuration;
// The frequency is used to program the timer based on a deadline in ns.
// Round up such that the timer fires after the deadline, not before.
constinit Pow2Fraction<Rounding::up> timerTickFreq;

// In EL2 with VHE, CNTP_* and CNTV_* access the EL2 timers (CNTHP_* and CNTHV_*).
// We use the physical one of the two since firmware always describes its interrupt,
// while the interrupt of the EL2 virtual timer is optional.
// TODO: Linux prefers the EL2 virtual timer and only falls back to the EL2 physical one if
//       firmware does not describe its interrupt. Note that the GSIV of the EL2 virtual timer
//       lives in a GTDT revision 3 extension and not in the base table.

uint64_t getRawTimestampCounter() {
	uint64_t cnt;
	if (isKernelInEl2()) {
		asm volatile ("isb; mrs %0, cntpct_el0" : "=r"(cnt) : : "memory");
	} else {
		asm volatile ("isb; mrs %0, cntvct_el0" : "=r"(cnt) : : "memory");
	}
	return cnt;
}

namespace {

void setRawTimerDeadline(uint64_t deadline) {
	if (isKernelInEl2()) {
		asm volatile ("msr cntp_cval_el0, %0" :: "r"(deadline));
	} else {
		asm volatile ("msr cntv_cval_el0, %0" :: "r"(deadline));
	}
}

void setTimerControl(uint64_t ctl) {
	if (isKernelInEl2()) {
		asm volatile ("msr cntp_ctl_el0, %0" :: "r"(ctl));
	} else {
		asm volatile ("msr cntv_ctl_el0, %0" :: "r"(ctl));
	}
}

} // namespace anonymous

struct GenericTimerSink : IrqSink {
	GenericTimerSink()
	: IrqSink{frg::string<KernelAlloc>{*kernelAlloc, "generic-timer-irq"}} { }

	virtual ~GenericTimerSink() = default;

	IrqStatus raise() override {
		handleTimerInterrupt();
		return IrqStatus::acked;
	}
};

uint64_t getClockNanos() {
	return timerTickDuration * getRawTimestampCounter();
}

UserspaceClock getUserspaceClock() {
	assert(timerTickDuration);
	return {
		.type = isKernelInEl2() ? UserspaceClockType::armCntpct : UserspaceClockType::armCntvct,
		.tickDuration = timerTickDuration
	};
}

void setTimerDeadline(frg::optional<uint64_t> deadline) {
	if (deadline) {
		uint64_t rawDeadline = timerTickFreq * *deadline;

		setRawTimerDeadline(rawDeadline);
		// Unmask the timer interrupt.
		setTimerControl(0b01);
	} else {
		// Mask the timer interrupt.
		setTimerControl(0b11);
	}
}

bool timerDisarmsItself() {
	// The generic timer keeps asserting its interrupt while cntvct is past cntv_cval.
	return false;
}

void initializeTimers() {
	constexpr uint64_t divisor = 1'000'000'000;
	uint64_t freqHz;
	asm volatile ("mrs %0, cntfrq_el0" : "=r"(freqHz));

	// Divide by 10^9 to convert Hz to nHz.
	timerTickDuration = computePow2Fraction<Rounding::down>(divisor, freqHz);
	timerTickFreq = computeReciprocal(timerTickDuration);

	// Enable and mask the timer interrupt.
	setTimerControl(0b11);
}

static bool timersFound = false;

static DeviceTreeNode *timerNode = nullptr;

static dt::IrqController *timerIrqParent = nullptr;
static frg::manual_box<dtb::Cells> timerIrq;

enum class TimerIrqSource {
	none,
	dt,
	acpi
};

static TimerIrqSource timerIrqSource = TimerIrqSource::none;
static GlobalIrqInfo acpiTimerIrq;

smarter::shared_ptr<IrqPin> setupAcpiTimerIrq() {
	return std::visit(
	    frg::overloaded{
	        [](GicV2 *gic) -> smarter::shared_ptr<IrqPin> {
		        auto pin = gic->getPin(acpiTimerIrq.gsi);
		        if (!pin)
			        panicLogger() << "thor: GTDT timer GSI " << acpiTimerIrq.gsi
			                      << " has no GIC pin" << frg::endlog;
		        pin->configure(acpiTimerIrq.configuration);
		        return pin;
	        },
	        [](GicV3 *gic) -> smarter::shared_ptr<IrqPin> {
		        auto pin = gic->getPin(acpiTimerIrq.gsi);
		        if (!pin)
			        panicLogger() << "thor: GTDT timer GSI " << acpiTimerIrq.gsi
			                      << " has no GIC pin" << frg::endlog;
		        pin->configure(acpiTimerIrq.configuration);
		        return pin;
	        },
	        [](auto &&) -> smarter::shared_ptr<IrqPin> {
		        panicLogger() << "thor: GTDT timer requires a GIC" << frg::endlog;
		        __builtin_unreachable();
	        }
	    },
	    externalIrq
	);
}

bool initTimerIrqFromAcpi() {
	if (!acpiRsdpNote->rsdp)
		return false;

	uacpi_table gtdtTbl;
	if (uacpi_table_find_by_signature("GTDT", &gtdtTbl) != UACPI_STATUS_OK)
		return false;
	frg::scope_exit finish{[&] { uacpi_table_unref(&gtdtTbl); }};

	// Only require the fields that we read below. acpi_gtdt also covers the EL2 virtual
	// timer that revision 3 appended to the table.
	constexpr size_t requiredLength = offsetof(acpi_gtdt, el2_flags) + sizeof(acpi_gtdt::el2_flags);
	if (gtdtTbl.hdr->length < requiredLength)
		panicLogger() << "thor: GTDT is too small" << frg::endlog;

	auto *gtdt = reinterpret_cast<acpi_gtdt *>(gtdtTbl.ptr);
	bool inEl2 = isKernelInEl2();
	auto name = inEl2 ? "EL2 physical" : "EL1 virtual";
	auto flags = inEl2 ? gtdt->el2_flags : gtdt->el1_virtual_flags;
	auto gsiv = inEl2 ? gtdt->el2_gsiv : gtdt->el1_virtual_gsiv;

	// GSIV zero would resolve to an SGI, i.e., the timer is not described at all.
	if (!gsiv) {
		infoLogger() << "thor: GTDT has no " << name << " timer" << frg::endlog;
		return false;
	}

	acpiTimerIrq.gsi = gsiv;
	acpiTimerIrq.configuration.trigger =
	    (flags & ACPI_GTDT_TRIGGERING) ? TriggerMode::edge : TriggerMode::level;
	acpiTimerIrq.configuration.polarity =
	    (flags & ACPI_GTDT_POLARITY) ? Polarity::low : Polarity::high;
	timerIrqSource = TimerIrqSource::acpi;

	infoLogger() << "thor: Found GTDT " << name << " timer at GSI " << acpiTimerIrq.gsi
	             << frg::endlog;
	return true;
}

static initgraph::Task initTimerIrq{&globalInitEngine, "arm.init-timer-irq",
	initgraph::Requires{getIrqControllerReadyStage()},
	initgraph::Entails{getTaskingAvailableStage()},
	[] {
		if (initTimerIrqFromAcpi()) {
			initTimerOnThisCpu();
			timersFound = true;
			return;
		}

		auto root = getDeviceTreeRoot();
		if (!root)
			panicLogger() << "thor: Failed to find timer" << frg::endlog;

		root->forEach([&](DeviceTreeNode *node) -> bool {
			if (node->isCompatible<1>({"arm,armv8-timer"})) {
				timerNode = node;
				return true;
			}

			return false;
		});

		assert(timerNode && "Failed to find timer");

		// TODO(qookie): I think Linux has some logic to pick
		// either the physical or virtual timer, which we
		// should probably replicate instead of always picking
		// the virtual one.

		// Index 2 is the EL1 virtual timer, index 3 the EL2 physical timer.
		int wantedIdx = isKernelInEl2() ? 3 : 2;

		int idx = 0;
		auto walkInterruptResult = dt::walkInterrupts(
			[&] (DeviceTreeNode *parentNode, dtb::Cells irqCells) {
				// This offset is defined in the Linux
				// DTB binding for compatible nodes.
				if (idx == wantedIdx) {
					timerIrqParent = parentNode->getAssociatedIrqController();
					timerIrq.initialize(irqCells);
					timerIrqSource = TimerIrqSource::dt;
				}

				idx++;
			}, timerNode);

		assert(walkInterruptResult && walkInterruptResult.value() && "Failed to parse generic timer interrupts");

		initTimerOnThisCpu();

		timersFound = true;
	}
};

bool haveTimer() {
	return timersFound;
}

// Sets up the proper interrupt trigger and polarity for the PPI
void initTimerOnThisCpu() {
	auto sink = frg::construct<GenericTimerSink>(*kernelAlloc);
	smarter::shared_ptr<IrqPin> pin;
	switch (timerIrqSource) {
		case TimerIrqSource::dt:
			pin = timerIrqParent->resolveDtIrq(*timerIrq);
			break;
		case TimerIrqSource::acpi:
			pin = setupAcpiTimerIrq();
			break;
		case TimerIrqSource::none:
			panicLogger() << "thor: Timer IRQ was not initialized" << frg::endlog;
	}
	IrqPin::attachSink(std::move(pin), sink);
}

} // namespace thor
