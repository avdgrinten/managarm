#include <assert.h>
#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/main.hpp>
#include <frg/bitops.hpp>
#include <frg/utility.hpp>
#include <stdint.h>

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

namespace eir {

namespace {

void decompressZstdInitrd() {
	auto bound = ZSTD_decompressBound(rawInitrd, rawInitrdSize);
	if (bound == ZSTD_CONTENTSIZE_ERROR || bound > SIZE_MAX) {
		panicLogger() << "eir: initrd is not a valid zstd stream" << frg::endlog;
		__builtin_trap();
	}

	auto dctxSize = frg::align_up(ZSTD_estimateDCtxSize(), pageSize);
	auto dctx = ZSTD_initStaticDCtx(physToVirt<void>(cutFromRegion(dctxSize)), dctxSize);
	assert(dctx);

	auto outCapacity = frg::align_up(frg::max<size_t>(bound, 1), pageSize);
	auto out = physToVirt<void>(cutFromRegion(outCapacity));

	auto outSize = ZSTD_decompressDCtx(dctx, out, outCapacity, rawInitrd, rawInitrdSize);
	if (ZSTD_isError(outSize)) {
		panicLogger() << "eir: Failed to decompress initrd: " << ZSTD_getErrorName(outSize)
		              << frg::endlog;
		__builtin_trap();
	}

	infoLogger() << "eir: Decompressed zstd initrd from " << rawInitrdSize << " to " << outSize
	             << " bytes" << frg::endlog;

	initrd = out;
	initrdSize = outSize;
}

} // namespace

void decompressInitrd() {
	assert(rawInitrd);

	if (ZSTD_isFrame(rawInitrd, rawInitrdSize)) {
		decompressZstdInitrd();
	} else {
		initrd = rawInitrd;
		initrdSize = rawInitrdSize;
	}
}

} // namespace eir
