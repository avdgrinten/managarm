#pragma once

// eir only uses static zstd contexts, so zstd never needs to allocate.
// Defining ZSTD_DEPS_MALLOC prevents zstd_deps.h from including <stdlib.h>.
#define ZSTD_DEPS_MALLOC
#define ZSTD_malloc(s) ((void)(s), __builtin_trap(), (void *)0)
#define ZSTD_calloc(n, s) ((void)(n), (void)(s), __builtin_trap(), (void *)0)
#define ZSTD_free(p) ((void)(p), __builtin_trap())
