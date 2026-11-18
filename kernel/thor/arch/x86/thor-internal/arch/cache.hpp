#pragma once

#include <stddef.h>
#include <stdint.h>

namespace thor {

inline void cacheFlush(void const *ptr) {
	asm volatile ("clflush (%0)" : : "r" (ptr) : "memory");
}

inline void cacheFlush(void const *ptr, size_t len) {
	for(size_t i = 0; i < len; i += 64) {
		asm volatile ("clflush (%0)" : : "r" (reinterpret_cast<uintptr_t>(ptr) + i) : "memory");
	}
}

} // namespace thor
