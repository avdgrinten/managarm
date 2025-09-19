#pragma once

namespace eir {

inline void runFnPtrOnStack(void *ctx, void (*fn)(void *), void *sp) {
	register void *rdi asm("rdi") = ctx;
	asm volatile(
	    // clang-format off
	         "mov %0, %%rsp" "\n"
	    "\r" "call *%1" "\n"
	    "\r" "ud2"
	    // clang-format on
	    :
	    : "r"(sp), "r"(fn), "r"(rdi)
	    : "memory"
	);
}

} // namespace eir
