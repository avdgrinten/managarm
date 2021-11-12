#include <assert.h>

#include <eir-internal/debug.hpp>

namespace eir {

void discoverMemoryFromDtb(void *dtbPtr) {
	eir::infoLogger() << "DTB pointer " << dtbPtr << frg::endlog;
}

} // namespace eir
