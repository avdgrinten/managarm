#pragma once

#include <frg/spinlock.hpp>
#include <thor-internal/debug.hpp>

namespace thor {

// Always inline as this might be called from code that disables coverage
[[gnu::always_inline]] inline bool intsAreEnabled() {
	uint64_t v;
	asm volatile("mrs %0, daif" : "=r"(v));
	return !v;
}

// Always inline as this might be called from code that disables coverage
[[gnu::always_inline]] inline void enableInts() { asm volatile("msr daifclr, #15"); }

// Always inline as this might be called from code that disables coverage
[[gnu::always_inline]] inline void disableInts() { asm volatile("msr daifset, #15"); }

inline void halt() { asm volatile("wfi"); }

} // namespace thor
