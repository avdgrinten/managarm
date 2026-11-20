#pragma once

#include <frg/mutex.hpp>
#include <thor-internal/arch/ints.hpp>
#include <thor-internal/cpu-data.hpp>

namespace thor {

// Always inline as this might be called from code that disables coverage
[[gnu::always_inline]] inline Ipl contextIpl() {
	return __atomic_load_n(&getCpuData()->contextIpl, __ATOMIC_RELAXED);
}

inline Ipl currentIpl() {
	return getCpuData()->currentIpl.load(std::memory_order_relaxed);
}

[[noreturn]] void panicOnIplStateCorruption();
[[noreturn]] void panicOnIllegalIplEntry(Ipl newIpl, Ipl currentIpl);
[[noreturn]] void panicOnIplScopeNesting(Ipl expectedIpl);
[[noreturn]] void panicOnInterruptIplDesync();

inline void iplSave(IplState &savedIpl) {
	auto cpuData = getCpuData();
	savedIpl.current = cpuData->currentIpl.load(std::memory_order_relaxed);
	savedIpl.context = __atomic_load_n(&cpuData->contextIpl, __ATOMIC_RELAXED);
}

// Raise context IPL. Takes saved IPL for error checking.
inline void iplEnterContext(Ipl newIpl, IplState savedIpl) {
	if(newIpl < ipl::maximal) {
		if (!(savedIpl.current < newIpl)) [[unlikely]]
			panicOnIllegalIplEntry(newIpl, savedIpl.current);
	}
	auto cpuData = getCpuData();
	cpuData->currentIpl.store(newIpl, std::memory_order_relaxed);
	std::atomic_signal_fence(std::memory_order_release);
	__atomic_store_n(&cpuData->contextIpl, newIpl, __ATOMIC_RELAXED);
	// Perform (w, rw) fence to prevent re-ordering of future accesses with the IPL stores.
	std::atomic_signal_fence(std::memory_order_seq_cst);
}

// Restore (= lowering) context + current IPL.
inline void iplLeaveContext(IplState savedIpl) {
	auto cpuData = getCpuData();
	// Perform (rw, w) fence to prevent re-ordering of past accesses with the IPL stores.
	std::atomic_signal_fence(std::memory_order_release);
	__atomic_store_n(&cpuData->contextIpl, savedIpl.context, __ATOMIC_RELAXED);
	std::atomic_signal_fence(std::memory_order_release);
	cpuData->currentIpl.store(savedIpl.current, std::memory_order_relaxed);
	// Perform (w, rw) fence to prevent re-ordering of the IPL stores with future accesses.
	std::atomic_signal_fence(std::memory_order_seq_cst);
}

inline void iplDemoteContext(Ipl newIpl) {
	auto cpuData = getCpuData();
	// Perform (rw, w) fence to prevent re-ordering of past accesses with the IPL stores.
	std::atomic_signal_fence(std::memory_order_release);
	__atomic_store_n(&cpuData->contextIpl, newIpl, __ATOMIC_RELAXED);
	std::atomic_signal_fence(std::memory_order_release);
	cpuData->currentIpl.store(newIpl, std::memory_order_relaxed);
	// Perform (w, rw) fence to prevent re-ordering of the IPL stores with future accesses.
	std::atomic_signal_fence(std::memory_order_seq_cst);
}

inline void deferToIplLowerThan(Ipl l) {
	assert(l > 0);
	getCpuData()->iplDeferred.fetch_or(
		static_cast<IplMask>(1) << (l - 1),
		std::memory_order_relaxed
	);
}

void handleIplDeferred(Ipl current, Ipl ceiling);

// Return the previous IPL or ipl::bad if the IPL was not raised.
inline Ipl iplRaise(Ipl raisedIpl) {
	auto cpuData = getCpuData();
	auto current = cpuData->currentIpl.load(std::memory_order_relaxed);

	if (current >= raisedIpl)
		return ipl::bad;

	cpuData->currentIpl.store(raisedIpl, std::memory_order_relaxed);

	// Perform (w, rw) fence to prevent re-ordering of future accesses with the iplState store.
	std::atomic_signal_fence(std::memory_order_seq_cst);

	return current;
}

// expectedIpl is useful for error checking.
inline void iplLower(Ipl expectedIpl, Ipl lowerIpl) {
	auto cpuData = getCpuData();
	auto current = cpuData->currentIpl.load(std::memory_order_relaxed);

	assert(lowerIpl != ipl::bad);
	if (current != expectedIpl)
		panicOnIplScopeNesting(expectedIpl);

	// Perform (rw, w) fence to prevent re-ordering of past accesses with the iplState store.
	std::atomic_signal_fence(std::memory_order_release);
	cpuData->currentIpl.store(lowerIpl, std::memory_order_relaxed);

	// Perform (w, rw) fence to prevent re-ordering of iplState store and iplDeferred load.
	std::atomic_signal_fence(std::memory_order_seq_cst);
}

template<Ipl L>
struct IplGuard {
	IplGuard() {
		auto cpuData = getCpuData();
		auto context = __atomic_load_n(&cpuData->contextIpl, __ATOMIC_RELAXED);

		// Otherwise, this guard is taken in a context where it cannot be taken.
		assert(context <= L);

		previous_ = iplRaise(L);
	}

	IplGuard(const IplGuard &) = delete;

	~IplGuard() {
		auto cpuData = getCpuData();
		auto current = cpuData->currentIpl.load(std::memory_order_relaxed);

		if (previous_ == ipl::bad) {
			assert(current >= L);
			return;
		}
		assert(previous_ < L);

		iplLower(L, previous_);

		auto deferred = cpuData->iplDeferred.load(std::memory_order_relaxed);
		auto mask = (~static_cast<IplMask>(0)) << previous_;
		if (deferred & mask) [[unlikely]]
			handleIplDeferred(previous_, L);
	}

	IplGuard operator=(const IplGuard &) = delete;

private:
	Ipl previous_{ipl::bad};
};

using MigrationGuard = IplGuard<ipl::noMigration>;
using PreemptionGuard = IplGuard<ipl::noPreemption>;
using ScheduleGuard = IplGuard<ipl::noSchedule>;

struct IrqMutex {
	IrqMutex() = default;

	IrqMutex(const IrqMutex &) = delete;

	void lock() {
		auto cpuData = getCpuData();
		auto intState = &cpuData->intState;

		auto outerIpl = cpuData->currentIpl.load(std::memory_order_relaxed);
		if (outerIpl < ipl::interrupt) {
			if (!intsAreEnabled()) [[unlikely]]
				panicOnInterruptIplDesync();

			// Update IPL, then IrqMutex nesting.
			disableInts();
			cpuData->currentIpl.store(ipl::interrupt, std::memory_order_relaxed);
			std::atomic_signal_fence(std::memory_order_release);
			intState->nesting.store(1, std::memory_order_relaxed);

			// (w, rw) fence to keep following accesses after the intState store.
			std::atomic_signal_fence(std::memory_order_seq_cst);

			intState->outerIpl = outerIpl;
		} else {
			auto n = intState->nesting.load(std::memory_order_relaxed);
			intState->nesting.store(n + 1, std::memory_order_relaxed);
		}
	}

	void unlock() {
		auto cpuData = getCpuData();
		auto intState = &cpuData->intState;

		auto n = intState->nesting.load(std::memory_order_relaxed);
		if (n == 1) {
			auto outerIpl = intState->outerIpl;
			intState->outerIpl = ipl::bad;

			// (rw, w) fence to keep preceeding accesses before the intState store.
			std::atomic_signal_fence(std::memory_order_release);

			// Update IrqMutex nesting, then IPL.
			intState->nesting.store(0, std::memory_order_relaxed);
			if (outerIpl != ipl::bad) {
				std::atomic_signal_fence(std::memory_order_release);
				if (cpuData->currentIpl.load(std::memory_order_relaxed) != ipl::interrupt) [[unlikely]]
					panicOnIplScopeNesting(ipl::interrupt);
				cpuData->currentIpl.store(outerIpl, std::memory_order_relaxed);
				enableInts();
			}
		} else {
			if (!n) [[unlikely]]
				panicOnIplStateCorruption();
			intState->nesting.store(n - 1, std::memory_order_relaxed);
		}
	}

	unsigned int nesting() {
		auto cpuData = getCpuData();
		auto intState = &cpuData->intState;
		return intState->nesting.load(std::memory_order_relaxed);
	}
};

struct StatelessIrqLock {
	StatelessIrqLock() {
		lock();
	}

	StatelessIrqLock(frg::dont_lock_t) { }

	StatelessIrqLock(const StatelessIrqLock &) = delete;

	~StatelessIrqLock() {
		if(_outerIpl != ipl::bad)
			unlock();
	}

	StatelessIrqLock &operator= (const StatelessIrqLock &) = delete;

	void lock() {
		if (_outerIpl != ipl::bad) [[unlikely]]
			panicOnIplStateCorruption();

		auto cpuData = getCpuData();
		auto outerIpl = cpuData->currentIpl.load(std::memory_order_relaxed);
		if (outerIpl < ipl::interrupt) {
			if (!intsAreEnabled()) [[unlikely]]
				panicOnInterruptIplDesync();

			disableInts();
			cpuData->currentIpl.store(ipl::interrupt, std::memory_order_relaxed);

			// (w, rw) fence to keep following accesses after the IPL store.
			std::atomic_signal_fence(std::memory_order_seq_cst);

			_outerIpl = outerIpl;
		}
	}

	void unlock() {
		auto cpuData = getCpuData();
		if (_outerIpl != ipl::bad) {
			// (rw, w) fence to keep preceeding accesses before the IPL store.
			std::atomic_signal_fence(std::memory_order_release);

			if (cpuData->currentIpl.load(std::memory_order_relaxed) != ipl::interrupt) [[unlikely]]
				panicOnIplScopeNesting(ipl::interrupt);
			cpuData->currentIpl.store(_outerIpl, std::memory_order_relaxed);
			enableInts();

			_outerIpl = ipl::bad;
		}
	}

	void release() {
		_outerIpl = ipl::bad;
	}

private:
	Ipl _outerIpl{ipl::bad};
};

inline IrqMutex globalIrqMutex;

inline IrqMutex &irqMutex() {
	return globalIrqMutex;
}

// Saves and restore both IPL and hardware IRQ state.
// In contrast to StatelessIrqLock, this class does not assert() or panic on broken invariants.
// This is used by the logging code (e.g., when logging kernel panics).
struct RobustIrqLock {
	RobustIrqLock() {
		auto cpuData = getCpuData();

		_outerInts = intsAreEnabled();
		if (_outerInts)
			disableInts();
		auto currentIpl = cpuData->currentIpl.load(std::memory_order_relaxed);
		if (currentIpl < ipl::interrupt) {
			_outerIpl = currentIpl;
			cpuData->currentIpl.store(ipl::interrupt, std::memory_order_relaxed);
		}

		// (w, rw) fence to keep following accesses after the IPL store.
		std::atomic_signal_fence(std::memory_order_seq_cst);
	}

	RobustIrqLock(const RobustIrqLock &) = delete;

	~RobustIrqLock() {
		auto cpuData = getCpuData();

		// (rw, w) fence to keep preceeding accesses before the IPL store.
		std::atomic_signal_fence(std::memory_order_release);

		if (_outerIpl != ipl::bad)
			cpuData->currentIpl.store(_outerIpl, std::memory_order_relaxed);
		if (_outerInts)
			enableInts();
	}

	RobustIrqLock &operator= (const RobustIrqLock &) = delete;

private:
	bool _outerInts{false};
	Ipl _outerIpl{ipl::bad};
};

} // namespace thor
