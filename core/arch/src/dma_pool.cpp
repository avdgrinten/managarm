#include <arch/dma_pool.hpp>
#include <frg/bitops.hpp>
#include <algorithm>
#include <hel-syscalls.h>
#include <hel.h>
#include <protocols/posix/procdata.hpp>
#include <print>

namespace arch {

namespace {

} // namespace

contiguous_pool::contiguous_pool(contiguous_pool_options options)
: options_{options} { }

dma_ptr contiguous_pool::allocate(size_t size, size_t count, size_t align) {
	auto b = shift_of_(size, count, align);
	auto alloc_size = size_t{1} << b;

	dma_ptr ptr;
	if (b <= max_shift) {
		std::lock_guard lock{mutex_};
		auto bkt = &buckets_[b - min_shift];

		if (bkt->freelist.empty()) {
			auto *p = allocate_pages_(small_region_size);
			auto rn = new region{this, p};
			for (size_t off = 0; off + alloc_size <= small_region_size; off += alloc_size) {
				bkt->freelist.push_back({static_cast<arch::dma_region *>(rn), off});
			}
		}
		assert(!bkt->freelist.empty());

		ptr = bkt->freelist.back();
		bkt->freelist.pop_back();
	} else {
		// Large allocation. Allocate directly from the kernel.
		auto *p = allocate_pages_(alloc_size);
		auto rn = new region{this, p};
		ptr = {static_cast<arch::dma_region *>(rn), 0};
	}

	assert(ptr.get_raw_ptr());
	assert(!(reinterpret_cast<uintptr_t>(ptr.get_raw_ptr()) & (align - 1)));

	return ptr;
}

void contiguous_pool::deallocate(dma_ptr ptr, size_t size, size_t count, size_t align) {
	assert(ptr.pool() == this);
	auto b = shift_of_(size, count, align);
	auto alloc_size = size_t{1} << b;
	auto rn = static_cast<region *>(ptr.region());

	if (b <= max_shift) {
		std::lock_guard lock{mutex_};
		auto bkt = &buckets_[b - min_shift];

		bkt->freelist.push_back(ptr);
	} else {
		// Large allocation. Deallocate directly using the kernel.
		assert(!ptr.offset());
		auto p = reinterpret_cast<void *>(rn->get_base_va());
		deallocate_pages_(p, alloc_size);
		delete rn;
	}
}

// Power-of-two that is used for a particular allocation.
int contiguous_pool::shift_of_(size_t size, size_t count, size_t align) {
	return frg::ceil_log2(
		std::max({size * count, align, min_size_class, options_.minAllocationGap})
	);
}

void *contiguous_pool::allocate_pages_(size_t region_size) {
	HelAllocRestrictions restrictions{};
	restrictions.addressBits = options_.addressBits;

	HelHandle memory;
	void *p;
	HEL_CHECK(helAllocateMemory(posix::getProcessHierarchy(), region_size, kHelAllocContinuous,
			&restrictions, &memory));
	HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr, 0, region_size,
			kHelMapProtRead | kHelMapProtWrite, &p));
	HEL_CHECK(helCloseDescriptor(kHelThisUniverse, memory));
	assert(p);
	return p;
}

void contiguous_pool::deallocate_pages_(void *p, size_t region_size) {
	HEL_CHECK(helUnmapMemory(kHelNullHandle, p, region_size));
}

} // namespace arch
