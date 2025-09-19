#pragma once

namespace eir {

inline void runFnPtrOnStack(void *ctx, void (*fn)(void *), void *sp) {
	register void *x0 asm("x0") = ctx;
	asm volatile(
	    // clang-format off
	         "mov sp, %0" "\n"
	    "\r" "blr %1" "\n"
	    "\r" "udf #0"
	    // clang-format on
	    :
	    : "r"(sp), "r"(fn), "r"(x0)
	    : "memory"
	);
}

} // namespace eir
