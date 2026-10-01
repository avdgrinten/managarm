#include <stdint.h>

#include <hel.h>
#include <hel-syscalls.h>
#include <helix/clock.hpp>
#include <helix/memory.hpp>

namespace helix {

namespace {

// Read-only mapping of the kernel's clock page.
struct ClockPage {
	ClockPage() {
		HelHandle handle;
		if(helObtainHandle(kHelObtainClockPage, &handle) != kHelErrNone)
			return;
		UniqueDescriptor descriptor{handle};

		_mapping = Mapping{descriptor, 0, Mapping::pageSize, kHelMapProtRead};
		page = static_cast<const HelClockPage *>(_mapping.get());
	}

	const HelClockPage *page{nullptr};

private:
	Mapping _mapping;
};

// Cache and access the clock page.
const HelClockPage *clockPage() {
	static ClockPage singleton;
	return singleton.page;
}

// Reads the raw tick counter of the given clock. Returns false if this build cannot read it.
// The memory clobbers keep the compiler from moving the read out of the seqlock window.
bool readRawTicks(uint32_t clockType, uint64_t &ticks) {
#if defined(__x86_64__)
	if(clockType == kHelClockTsc) [[likely]] {
		uint32_t lsw, msw;
		asm volatile ("lfence; rdtsc" : "=a"(lsw), "=d"(msw) : : "memory");
		ticks = (static_cast<uint64_t>(msw) << 32) | static_cast<uint64_t>(lsw);
		return true;
	}
#elif defined(__aarch64__)
	if(clockType == kHelClockCntpct) {
		asm volatile ("isb; mrs %0, cntpct_el0" : "=r"(ticks) : : "memory");
		return true;
	}
	if(clockType == kHelClockCntvct) {
		asm volatile ("isb; mrs %0, cntvct_el0" : "=r"(ticks) : : "memory");
		return true;
	}
#elif defined(__riscv) && __riscv_xlen == 64
	if(clockType == kHelClockTime) [[likely]] {
		asm volatile ("rdtime %0" : "=r"(ticks) : : "memory");
		return true;
	}
#endif
	return false;
}

uint64_t ticksToNanos(uint64_t tickFactor, int32_t tickShift, uint64_t ticks) {
	auto product = static_cast<unsigned __int128>(tickFactor) * ticks;
	auto quotient = product >> tickShift;
	if(quotient >> 64)
		return UINT64_MAX;
	return static_cast<uint64_t>(quotient);
}

// Reads the clock without a syscall. Returns false if the kernel exports no clock that this
// build can read, or if the read raced with a kernel update of the clock page.
bool readUserspaceClock(uint64_t &nanos) {
	auto page = clockPage();
	if(!page)
		return false;

	// Start the seqlock read.
	// We do not loop around here. Instead, we fall through into helGetClock().
	auto seqlock = __atomic_load_n(&page->seqlock, __ATOMIC_ACQUIRE);
	if(seqlock & 1)
		return false;

	// Perform the actual loads.
	auto clockType = __atomic_load_n(&page->clockType, __ATOMIC_RELAXED);
	auto tickShift = __atomic_load_n(&page->tickShift, __ATOMIC_RELAXED);
	auto tickFactor = __atomic_load_n(&page->tickFactor, __ATOMIC_RELAXED);

	uint64_t ticks;
	auto readable = readRawTicks(clockType, ticks);

	// Finish the seqlock read.
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	if(!readable || __atomic_load_n(&page->seqlock, __ATOMIC_RELAXED) != seqlock)
		return false;

	nanos = ticksToNanos(tickFactor, tickShift, ticks);
	return true;
}

} // anonymous namespace

uint64_t getClock() {
	uint64_t nanos;
	if(readUserspaceClock(nanos)) [[likely]]
		return nanos;

	HEL_CHECK(helGetClock(&nanos));
	return nanos;
}

bool hasUserspaceClock() {
	uint64_t nanos;
	return readUserspaceClock(nanos);
}

} // namespace helix
