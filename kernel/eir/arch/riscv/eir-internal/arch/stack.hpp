#pragma once

namespace eir {

inline void runFnPtrOnStack(void *ctx, void (*fn)(void *), void *sp) {
	register void *a0 asm("a0") = ctx;
	asm volatile(
	    // clang-format off
	         "mv sp, %0" "\n"
	    "\r" "jalr %1" "\n"
	    "\r" "unimp"
	    // clang-format on
	    :
	    : "r"(sp), "r"(fn), "r"(a0)
	    : "memory"
	);
}

} // namespace eir
