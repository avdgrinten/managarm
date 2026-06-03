#include <assert.h>
#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/hierarchy.hpp>
#include <thor-internal/physical.hpp>

namespace thor {

static bool logPhysicalAllocs = false;

THOR_DEFINE_ELF_NOTE(memoryLayoutNote){elf_note_type::memoryLayout, {}};
THOR_DEFINE_ELF_NOTE(physicalMemoryNote){elf_note_type::physicalMemory, {}};

void poisonPhysicalAccess(PhysicalAddr physical) {
	auto address = directPhysicalOffset() + physical;
	KernelPageSpace::global().unmapSingle4k(address);
	invalidatePage(globalBindingId, reinterpret_cast<void *>(address));
}

void poisonPhysicalWriteAccess(PhysicalAddr physical) {
	auto address = directPhysicalOffset() + physical;
	KernelPageSpace::global().unmapSingle4k(address);
	KernelPageSpace::global().mapSingle4k(address, physical, 0, CachingMode::null);
	invalidatePage(globalBindingId, reinterpret_cast<void *>(address));
}

// --------------------------------------------------------
// PhysicalChunkAllocator
// --------------------------------------------------------

PhysicalChunkAllocator::PhysicalChunkAllocator() {
}

void PhysicalChunkAllocator::bootstrapRegion(PhysicalAddr address,
		int order, size_t numRoots, int8_t *buddyTree) {
	if(_numRegions >= static_cast<int>(eirMaxMemoryRegions)) {
		infoLogger() << "thor: Ignoring memory region (can only handle "
				<< eirMaxMemoryRegions << " regions)" << frg::endlog;
		return;
	}

	int n = _numRegions++;
	_allRegions[n].physicalBase = address;
	_allRegions[n].regionSize = numRoots << (order + kPageShift);
	_allRegions[n].buddyAccessor = BuddyAccessor{address, kPageShift,
			buddyTree, numRoots, order};

	auto currentTotal = _totalPages.load(std::memory_order_relaxed);
	auto currentFree = _freePages.load(std::memory_order_relaxed);
	_totalPages.store(currentTotal + (numRoots << order), std::memory_order_relaxed);
	_freePages.store(currentFree + (numRoots << order), std::memory_order_relaxed);
}

PhysicalAddr PhysicalChunkAllocator::allocate(size_t size, int addressBits) {
	{
		auto irq_lock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		auto currentFree = _freePages.load(std::memory_order_relaxed);
		auto currentUsed = _usedPages.load(std::memory_order_relaxed);
		assert(currentFree > size / kPageSize);
		_freePages.store(currentFree - size / kPageSize, std::memory_order_relaxed);
		_usedPages.store(currentUsed + size / kPageSize, std::memory_order_relaxed);

		// TODO: This could be solved better.
		int target = 0;
		while(size > (size_t(kPageSize) << target))
			target++;
		assert(size == (size_t(kPageSize) << target));

		if(logPhysicalAllocs)
			infoLogger() << "thor: Allocating physical memory of order "
						<< (target + kPageShift) << frg::endlog;
		for(int i = 0; i < _numRegions; i++) {
			if(target > _allRegions[i].buddyAccessor.tableOrder())
				continue;

			auto physical = _allRegions[i].buddyAccessor.allocate(target, addressBits);
			if(physical == BuddyAccessor::illegalAddress)
				continue;
		//	infoLogger() << "Allocate " << (void *)physical << frg::endlog;
			assert(!(physical % (size_t(kPageSize) << target)));
			return physical;
		}
	}

	// The allocation failed: report per-hierarchy memory usage before signalling failure.
	// Done outside the lock so that the report can walk hierarchies and log freely.
	dumpHierarchyMemoryUsage();

	return static_cast<PhysicalAddr>(-1);
}

void PhysicalChunkAllocator::free(PhysicalAddr address, size_t size) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);
	
	int target = 0;
	while(size > (size_t(kPageSize) << target))
		target++;

	for(int i = 0; i < _numRegions; i++) {
		if(address < _allRegions[i].physicalBase)
			continue;
		if(address + size - _allRegions[i].physicalBase > _allRegions[i].regionSize)
			continue;

		_allRegions[i].buddyAccessor.free(address, target);
		auto currentFree = _freePages.load(std::memory_order_relaxed);
		auto currentUsed = _usedPages.load(std::memory_order_relaxed);
		assert(currentUsed > size / kPageSize);
		_freePages.store(currentFree + size / kPageSize, std::memory_order_relaxed);
		_usedPages.store(currentUsed - size / kPageSize, std::memory_order_relaxed);
		return;
	}

	assert(!"Physical page is not part of any region");
}

PhysicalWindow::PhysicalWindow(PhysicalAddr physical, size_t size, CachingMode caching)
: size_{size} {
	uintptr_t lowAddr = physical & ~(kPageSize - 1);
	uintptr_t highAddr = ((physical + size) + (kPageSize - 1)) & ~(kPageSize - 1);
	size_t diff = highAddr - lowAddr;
	pages_ = diff / kPageSize;

	auto windowBase = KernelVirtualMemory::global().allocate(pages_ * kPageSize);
	for(size_t i = 0; i < pages_; i++) {
		KernelPageSpace::global().mapSingle4k(VirtualAddr(windowBase) + i * kPageSize,
				lowAddr + i * kPageSize, page_access::write, caching);
	}

	window_ = reinterpret_cast<void *>(uintptr_t(windowBase) + (physical - lowAddr));
}

PhysicalWindow::~PhysicalWindow() {
	auto windowBase = reinterpret_cast<uintptr_t>(window_) & ~(kPageSize - 1);

	for(size_t i = 0; i < pages_; i++) {
		KernelPageSpace::global().unmapSingle4k(VirtualAddr(windowBase) + i * kPageSize);
	}
}

} // namespace thor
