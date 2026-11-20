#pragma once

#include <atomic>

#include <frg/spinlock.hpp>

namespace thor {

void setupEarlyInterruptHandlers();

void setupIdt(uint32_t *table);
void setupFred();

// Always inline as this might be called from code that disables coverage
[[gnu::always_inline]] inline bool intsAreEnabled() {
	uint64_t rflags;
	asm volatile (
		"pushfq\n"
		"\rpop %0"
		: "=r" (rflags)
	);
	return (rflags & 0x200) != 0;
}

// Always inline as this might be called from code that disables coverage
[[gnu::always_inline]] inline void enableInts() {
	asm volatile ("sti");
}

// Always inline as this might be called from code that disables coverage
[[gnu::always_inline]] inline void disableInts() {
	asm volatile ("cli");
}

inline void halt() {
	asm volatile ("hlt");
}

} // namespace thor
