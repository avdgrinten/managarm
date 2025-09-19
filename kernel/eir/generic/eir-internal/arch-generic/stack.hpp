#pragma once

#include <eir-internal/arch/stack.hpp>

extern "C" char eirStackBase[];
extern "C" char eirStackTop[];

namespace eir {

static_assert(requires(void *ctx, void (*fn)(void *), void *sp) { runFnPtrOnStack(ctx, fn, sp); });

template <typename Fn>
void runOnStack(Fn fn, void *sp) {
	runFnPtrOnStack(
	    &fn,
	    [](void *ctx) {
		    auto *fnObjPtr = reinterpret_cast<Fn *>(ctx);
		    (*fnObjPtr)();
	    },
	    sp
	);
}

} // namespace eir
