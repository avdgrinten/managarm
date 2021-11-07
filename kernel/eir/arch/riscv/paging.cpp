#include <eir-internal/arch.hpp>

namespace eir {

void mapSingle4kPage(address_t address, address_t physical, uint32_t flags,
		CachingMode caching_mode) {
	(void)address;
	(void)physical;
	(void)flags;
	(void)caching_mode;
}

void initProcessorPaging(void *kernel_start, uint64_t &kernel_entry) {
	(void)kernel_start;
	(void)kernel_entry;
}

} // namespace eir
