#include <assert.h>
#include <initializer_list>
#include <utility>
#include <frg/algorithm.hpp>
#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/block-on.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/thread.hpp>

namespace thor {

static bool logPhysicalAllocs = false;

THOR_DEFINE_ELF_NOTE(memoryLayoutNote){elf_note_type::memoryLayout, {}};
THOR_DEFINE_ELF_NOTE(physicalMemoryNote){elf_note_type::physicalMemory, {}};

namespace {

CachingMode firmwareAttributesToCachingMode(uint32_t attributes) {
	if(attributes & eir_memory_attrs::wb)
		return CachingMode::writeBack;
	// We do not have a CachingMode to represent WT, treat it like WC.
	if(attributes & (eir_memory_attrs::wc | eir_memory_attrs::wt))
		return CachingMode::writeCombine;
	// If neither WB nor WC is supported, treat it as device memory.
	return CachingMode::mmioNonPosted;
}

} // namespace

frg::optional<CachingMode> determineFirmwareCachingMode(PhysicalAddr physical, size_t size) {
	auto *entries = reinterpret_cast<const EirFirmwareMemory *>(physicalMemoryNote->firmwareEntriesPtr);
	size_t numEntries = physicalMemoryNote->numFirmwareEntries;

	frg::optional<CachingMode> result;
	bool conflict = false;
	auto accumulate = [&](CachingMode mode) {
		if(result && *result != mode)
			conflict = true;
		result = mode;
	};

	// Walk all sorted entries that intersect the range, including overlapping ones.
	// Treat gaps in the firmware memory map as device memory.
	PhysicalAddr cursor = physical;
	PhysicalAddr limit = physical + frg::max(size, size_t{1});
	for(size_t i = 0; i < numEntries; i++) {
		auto &entry = entries[i];
		if(entry.address >= limit)
			break;
		if(entry.address + entry.size <= physical)
			continue;
		if(entry.address > cursor)
			accumulate(CachingMode::mmioNonPosted);
		accumulate(firmwareAttributesToCachingMode(entry.attributes));
		cursor = frg::max(cursor, entry.address + entry.size);
	}
	if(cursor < limit)
		accumulate(CachingMode::mmioNonPosted);

	if(conflict) {
		warningLogger() << "thor: Physical range 0x" << frg::hex_fmt{physical}
				<< " - 0x" << frg::hex_fmt{physical + size}
				<< " spans firmware memory with conflicting caching modes" << frg::endlog;
		return frg::null_opt;
	}
	return result;
}

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

bool canBlockForMemory() {
	if(currentIpl() >= ipl::noSchedule)
		return false;
	auto ctx = currentExecutorContext();
	return ctx && ctx->mayBlockForMemory;
}

size_t currentContextFloor() {
	if(!canBlockForMemory())
		return 0;
	auto ctx = currentExecutorContext();
	if(!ctx)
		return 0;
	return ctx->memoryFloor;
}

void PhysicalChunkAllocator::bootstrapRegion(PhysicalAddr address,
		int order, size_t numRoots, int8_t *buddyTree) {
	if(_numRegions >= static_cast<int>(eirMaxMemoryRegions)) {
		infoLogger() << "thor: Ignoring memory region (can only handle "
				<< eirMaxMemoryRegions << " regions)" << frg::endlog;
		return;
	}
	assert(numRoots <= maxRootsPerRegion);

	int n = _numRegions++;
	auto &region = _allRegions[n];
	region.physicalBase = address;
	region.regionSize = numRoots << (order + kPageShift);
	region.buddyAccessor = BuddyAccessor{address, kPageShift,
			buddyTree, numRoots, order};

	// Eir already allocated from the buddy tree (e.g., for the kernel's page tables).
	size_t usedPages = 0;
	for(size_t i = 0; i < numRoots; i++) {
		auto freeItems = region.buddyAccessor.countFreeItems(i);
		region.usedPagesOfRoot[i] = region.pagesPerRoot() - freeItems;
		region.unevictablePagesOfRoot[i] = region.usedPagesOfRoot[i];
		usedPages += region.usedPagesOfRoot[i];
	}

	auto currentTotal = _totalPages.load(std::memory_order_relaxed);
	auto currentUsed = _usedPages.load(std::memory_order_relaxed);
	auto currentFree = _freePages.load(std::memory_order_relaxed);
	_totalPages.store(currentTotal + (numRoots << order), std::memory_order_relaxed);
	_usedPages.store(currentUsed + usedPages, std::memory_order_relaxed);
	_freePages.store(currentFree + (numRoots << order) - usedPages, std::memory_order_relaxed);
	updateFloors_();
}

void PhysicalChunkAllocator::updateFloors_() {
	auto base = numTotalPages() / 256;
	// Clamp to minimum/maximum sizes to avoid wasting too much RAM.
	constexpr size_t minBase = size_t{1} << (20 - kPageShift);
	constexpr size_t maxBase = size_t{32} << (20 - kPageShift);
	if(base < minBase)
		base = minBase;
	if(base > maxBase)
		base = maxBase;
	kernelFloor_ = base;
	userFloor_ = 2 * base;

	// Background reclaim starts when an allocation leaves less than 1% of memory as headroom
	// above its floor and stops once the headroom is 2%.
	auto total = numTotalPages();
	reclaimResumeHeadroom_ = total / 100;
	reclaimSuspendHeadroom_ = total / 50;
	// Floors of up to 5% of memory (but at least 128 MiB) on top of the user floor get
	// assistance from the reclaimer.
	constexpr size_t minAssistedFloor = size_t{128} << (20 - kPageShift);
	maxAssistedFloor_ = frg::min(frg::max(total / 20, minAssistedFloor), total / 4);
}

void PhysicalChunkAllocator::assignRootClasses(size_t reserveBytes) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);
	assert(!_classesAssigned);

	size_t numFreeRoots = 0;
	for(int i = 0; i < _numRegions; i++) {
		auto &region = _allRegions[i];
		auto numRoots = region.buddyAccessor.numRoots();
		for(size_t r = 0; r < numRoots; r++) {
			auto cls = region.usedPagesOfRoot[r] ? RootClass::admitsPinned : RootClass::evictableOnly;
			region.rootClasses[r] = cls;
			freePagesOfClass_[static_cast<size_t>(cls)]
					+= region.pagesPerRoot() - region.usedPagesOfRoot[r];
			if(cls == RootClass::evictableOnly)
				numFreeRoots++;
		}
	}

	// Reserve the lowest free roots. Eir does not sort the regions by address.
	int order[eirMaxMemoryRegions];
	for(int i = 0; i < _numRegions; i++)
		order[i] = i;
	frg::insertion_sort(order, order + _numRegions, [&] (int a, int b) {
		return _allRegions[a].physicalBase > _allRegions[b].physicalBase;
	});

	// Never reserve more than half of the free roots.
	auto maxReservedRoots = numFreeRoots / 2;
	size_t numReservedRoots = 0;
	size_t reservedBytes = 0;
	size_t largestReservedRoot = 0;
	for(int i = 0; i < _numRegions && reservedBytes < reserveBytes; i++) {
		auto &region = _allRegions[order[i]];
		auto numRoots = region.buddyAccessor.numRoots();
		auto rootBytes = region.pagesPerRoot() << kPageShift;
		for(size_t r = 0; r < numRoots && reservedBytes < reserveBytes; r++) {
			if(region.rootClasses[r] != RootClass::evictableOnly)
				continue;
			if(numReservedRoots >= maxReservedRoots)
				break;
			convertRoot_(region, r, RootClass::contiguousReserve);
			numReservedRoots++;
			reservedBytes += rootBytes;
			largestReservedRoot = frg::max(largestReservedRoot, rootBytes);
		}
	}
	if(reservedBytes < reserveBytes)
		warningLogger() << "thor: Reserved only " << (reservedBytes >> 20)
				<< " MiB of the requested " << (reserveBytes >> 20)
				<< " MiB for contiguous allocations" << frg::endlog;

	_classesAssigned = true;

	size_t classBytes[numRootClasses]{};
	for(int i = 0; i < _numRegions; i++) {
		auto &region = _allRegions[i];
		for(size_t r = 0; r < region.buddyAccessor.numRoots(); r++)
			classBytes[static_cast<size_t>(region.rootClasses[r])]
					+= region.pagesPerRoot() << kPageShift;
	}
	infoLogger() << "thor: Physical memory classes: "
			<< (classBytes[static_cast<size_t>(RootClass::evictableOnly)] >> 20)
			<< " MiB evictable only, "
			<< (classBytes[static_cast<size_t>(RootClass::admitsPinned)] >> 20)
			<< " MiB admit pinned, "
			<< (classBytes[static_cast<size_t>(RootClass::contiguousReserve)] >> 20)
			<< " MiB contiguous reserve (largest reserved root: " << (largestReservedRoot >> 20)
			<< " MiB)" << frg::endlog;
}

std::expected<PhysicalAddr, Error> PhysicalChunkAllocator::allocate(size_t size, PhysicalKind kind,
		int addressBits) {
	AllocationPolicy policy{AllocationClass::kernel, currentContextFloor()};
	if(auto outcome = tryAllocate(policy, size, kind, addressBits); outcome)
		return outcome;
	if(isFungibleAllocation_(size, kind, addressBits) && canBlockForMemory()) {
		if(auto outcome = allocateBlocking(policy, kind); outcome)
			return outcome;
	}
	return tryAllocate({AllocationClass::emergency, 0}, size, kind, addressBits);
}

PhysicalAddr PhysicalChunkAllocator::allocate(size_t size, int addressBits) {
	auto outcome = allocate(size, PhysicalKind::pinned, addressBits);
	if(!outcome)
		return static_cast<PhysicalAddr>(-1);
	return *outcome;
}

PhysicalAddr PhysicalChunkAllocator::allocateInfallible(PhysicalKind kind) {
	assert(isFungibleAllocation_(kPageSize, kind, 64));
	if(auto outcome = allocate(kPageSize, kind); outcome)
		return *outcome;
	// allocate() only fails here if the emergency class is exhausted and blocking was either
	// impossible or cancelled by termination. The caller cannot fail, so wait uncancellably.
	if(canBlockForMemory()) {
		auto outcome = blockOn(
			allocateAwaitable({AllocationClass::kernel, currentContextFloor()}, kind,
					async::cancellation_token{})
		);
		assert(outcome);
		return *outcome;
	}
	panicOutOfMemory_(kind, __builtin_return_address(0));
}

void PhysicalChunkAllocator::panicOutOfMemory_(PhysicalKind kind, void *caller) {
	size_t pinnable;
	size_t numWaiters = 0;
	size_t waitingPages = 0;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);
		pinnable = pinnablePages_();
		for(auto node = waitTree_.first(); node; node = WaitTree::successor(node)) {
			++numWaiters;
			waitingPages += node->numPages;
		}
	}

	const char *kindName = "?";
	switch(kind) {
	case PhysicalKind::evictable: kindName = "evictable"; break;
	case PhysicalKind::pinned: kindName = "pinned"; break;
	case PhysicalKind::contiguous: kindName = "contiguous"; break;
	}
	const char *context = "no executor";
	if(getCurrentThread())
		context = "thread";
	else if(thisFiber())
		context = "fiber";

	urgentLogger() << "thor: Infallible allocation of a " << kindName << " page failed"
			<< " (caller " << caller << ", " << context
			<< ", IPL " << static_cast<int>(currentIpl()) << ")" << frg::endlog;
	urgentLogger() << "thor: Pages: " << numTotalPages() << " total, "
			<< numFreePages() << " free, " << pinnable << " pinnable;"
			<< " floors: kernel " << kernelFloor_ << ", user " << userFloor_ << ";"
			<< " waiting: " << numWaiters << " allocations of " << waitingPages << " pages"
			<< frg::endlog;
	panicLogger() << "thor: Out of physical memory" << frg::endlog;
	__builtin_trap();
}

std::expected<PhysicalAddr, Error> PhysicalChunkAllocator::tryAllocate(AllocationPolicy policy,
		size_t size, PhysicalKind kind, int addressBits) {
	PhysicalAddr physical;
	bool kickReclaim = false;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		auto floor = effectiveFloor_(floorFor_(policy.cls) + policy.floor);
		physical = allocateChunk_(size, kind, addressBits, floor);
		noteAllocation_(policy, kickReclaim);
	}

	if(kickReclaim)
		handleReclaimPressure();
	if(physical == static_cast<PhysicalAddr>(-1))
		return std::unexpected{Error::noMemory};
	return physical;
}

std::expected<PhysicalAddr, Error> PhysicalChunkAllocator::allocateBlocking(
		AllocationPolicy policy, PhysicalKind kind) {
	assert(canBlockForMemory());
	// Fibers have no interrupts that could cancel the wait.
	if(!getCurrentThread())
		return blockOn(allocateAwaitable(policy, kind, async::cancellation_token{}));
	return Thread::asyncBlockCurrentInterruptible(
		[&] (async::cancellation_token ct) {
			return allocateAwaitable(policy, kind, ct);
		},
		nullptr,
		condition::interrupt
	);
}

std::expected<PageChain, Error> PhysicalChunkAllocator::tryAllocateChain(AllocationPolicy policy,
		PhysicalKind kind, size_t numPages) {
	assert(numPages);
	assert(kind != PhysicalKind::contiguous);
	std::expected<PageChain, Error> chain{std::unexpected{Error::noMemory}};
	bool kickReclaim = false;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		auto floor = effectiveFloor_(floorFor_(policy.cls) + policy.floor);
		if(canAllocateFungible_(numPages, floor))
			chain = allocateFungible_(kind, numPages, floor);
		noteAllocation_(policy, kickReclaim);
	}

	if(kickReclaim)
		handleReclaimPressure();
	return chain;
}

std::expected<PageChain, Error> PhysicalChunkAllocator::allocateChainBlocking(
		AllocationPolicy policy, PhysicalKind kind, size_t numPages) {
	assert(canBlockForMemory());
	// Fibers have no interrupts that could cancel the wait.
	if(!getCurrentThread())
		return blockOn(allocateChainAwaitable(policy, kind, numPages, async::cancellation_token{}));
	return Thread::asyncBlockCurrentInterruptible(
		[&] (async::cancellation_token ct) {
			return allocateChainAwaitable(policy, kind, numPages, ct);
		},
		nullptr,
		condition::interrupt
	);
}

void PhysicalChunkAllocator::startWait_(WaitNode *node) {
	assert(node->cls == AllocationClass::kernel || node->cls == AllocationClass::user);
	assert(node->kind != PhysicalKind::contiguous);
	assert(node->numPages);

	bool kickReclaim = false;
	bool completed = false;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		AllocationPolicy policy{node->cls, node->floor};
		node->floor += floorFor_(node->cls);
		if(!canAllocateFungible_(node->numPages, effectiveFloor_(node->floor))) {
			node->seq = nextSeq_++;
			node->queued = true;
			waitTree_.insert(node);
		} else {
			node->result = allocateFungible_(node->kind, node->numPages, node->floor);
			completed = true;
		}
		noteAllocation_(policy, kickReclaim);
	}

	if(kickReclaim)
		handleReclaimPressure();
	if(completed)
		node->event.raise();
}

void PhysicalChunkAllocator::cancelWait_(WaitNode *node) {
	WaitList served;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		// Otherwise, the node was already served and its completion is on the way.
		if(!node->queued)
			return;
		auto wasFirst = waitTree_.first() == node;
		waitTree_.remove(node);
		node->queued = false;
		node->result = std::unexpected{Error::cancelled};
		served.push_back(node);
		// Only the first node prevents other nodes from being served.
		if(wasFirst)
			serveWaiters_(served);
	}

	completeServed_(served);
}

size_t PhysicalChunkAllocator::floorFor_(AllocationClass cls) {
	switch(cls) {
	case AllocationClass::emergency: return 0;
	case AllocationClass::kernel: return kernelFloor_;
	case AllocationClass::user: return userFloor_;
	}
	__builtin_trap();
}

size_t PhysicalChunkAllocator::pressureShortfall() {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	updateReclaimTarget_();
	if(!reclaimTarget_)
		return 0;
	return reclaimTarget_ - pinnablePages_();
}

void PhysicalChunkAllocator::noteAllocation_(AllocationPolicy policy, bool &kickReclaim) {
	// Larger floors get no assistance from the reclaimer.
	// Otherwise, a floor that can never be met would make the reclaimer evict pages forever.
	if(policy.floor > maxAssistedFloor_)
		return;

	// The marks are relative to the user floor for all classes, such that reclaim starts
	// before user allocations have to wait.
	auto floor = floorFor_(AllocationClass::user) + policy.floor;
	// Allocations at or above the floor of the first waiter only proceed once it is served.
	if(auto first = waitTree_.first(); first && first->floor <= floor)
		floor += first->numPages;
	if(pinnablePages_() >= floor + reclaimResumeHeadroom_)
		return;
	auto target = floor + reclaimSuspendHeadroom_;
	if(target <= reclaimTarget_)
		return;
	reclaimTarget_ = target;
	kickReclaim = true;
}

void PhysicalChunkAllocator::updateReclaimTarget_() {
	if(reclaimTarget_ && pinnablePages_() >= reclaimTarget_)
		reclaimTarget_ = 0;
}

size_t PhysicalChunkAllocator::effectiveFloor_(size_t floor) {
	auto first = waitTree_.first();
	if(first && floor >= first->floor)
		return ~size_t{0};
	return floor;
}

// The floor of a class holds pages back for requests of earlier classes.
// These requests can be of any kind, so the held-back pages must be usable by all kinds.
// pinnablePages_() returns the number of pages that can be used by all kinds of allocations.
size_t PhysicalChunkAllocator::pinnablePages_() {
	// Pinned frames can convert evictableOnly roots, which does not change this count.
	return freePagesOfClass_[static_cast<size_t>(RootClass::admitsPinned)]
			+ freePagesOfClass_[static_cast<size_t>(RootClass::evictableOnly)];
}

size_t PhysicalChunkAllocator::pinnableBudget_(size_t floor) {
	auto pinnable = pinnablePages_();
	if(pinnable <= floor)
		return 0;
	return pinnable - floor;
}

bool PhysicalChunkAllocator::canAllocateFungible_(size_t numPages, size_t floor) {
	// N fungible allocations take exactly N pinnable pages.
	// Note that fragmentation (or other restrictions) do not apply to fungible allocation,
	// so this is exact: the allocation necessarily succeeds as long as the pages are available.
	return pinnableBudget_(floor) >= numPages;
}

PageChain PhysicalChunkAllocator::allocateFungible_(PhysicalKind kind, size_t numPages,
		size_t floor) {
	assert(kind != PhysicalKind::contiguous);
	PageChain chain;
	for(size_t k = 0; k < numPages; k++) {
		auto physical = allocateChunk_(kPageSize, kind, 64, floor);
		assert(physical != static_cast<PhysicalAddr>(-1));
		chain.push(physical);
	}
	return chain;
}

PhysicalAddr PhysicalChunkAllocator::allocateChunk_(size_t size, PhysicalKind kind,
		int addressBits, size_t floor) {
	assert(_classesAssigned);

	// TODO: This could be solved better.
	int target = 0;
	while(size > (size_t(kPageSize) << target))
		target++;
	assert(size == (size_t(kPageSize) << target));

	if(logPhysicalAllocs)
		infoLogger() << "thor: Allocating physical memory of order "
					<< (target + kPageShift) << frg::endlog;

	auto pinnableBudget = pinnableBudget_(floor);
	bool mayUsePinnableRoots = pinnableBudget >= (size >> kPageShift);
	auto pinnableBefore = pinnablePages_();

	frg::optional<Allocation> allocation;
	switch(kind) {
	case PhysicalKind::evictable:
		if(!mayUsePinnableRoots)
			break;
		// If possible, keep holes in admitsPinned roots for pinned frames.
		allocation = allocateInClass_(target, addressBits, RootClass::evictableOnly);
		if(!allocation)
			allocation = allocateInClass_(target, addressBits, RootClass::admitsPinned);
		break;
	case PhysicalKind::pinned:
		if(!mayUsePinnableRoots)
			break;
		allocation = allocateInClass_(target, addressBits, RootClass::admitsPinned);
		if(!allocation)
			allocation = allocateByConverting_(target, addressBits,
					RootClass::admitsPinned, pinnableBudget);
		break;
	case PhysicalKind::contiguous:
		// contiguousReserve roots serve nothing else, so they come first.
		// Holes in admitsPinned roots cost nothing since these roots are never emptied anyway.
		// Converting an evictableOnly root shrinks the memory that can be emptied, so it comes last.
		allocation = allocateInClass_(target, addressBits, RootClass::contiguousReserve);
		if(!allocation && mayUsePinnableRoots)
			allocation = allocateInClass_(target, addressBits, RootClass::admitsPinned);
		if(!allocation && mayUsePinnableRoots)
			allocation = allocateByConverting_(target, addressBits,
					RootClass::contiguousReserve, pinnableBudget);
		break;
	}
	if(!allocation)
		return static_cast<PhysicalAddr>(-1);

	accountAllocation_(*allocation, size / kPageSize, kind);
	// We must keep at least the floor available, or,
	// if the floor was already violated, we must not take any additional pinnable pages.
	assert(pinnablePages_() >= frg::min(pinnableBefore, floor));
	return allocation->physical;
}

template<typename F>
frg::optional<PhysicalChunkAllocator::Allocation>
PhysicalChunkAllocator::search_(int order, int addressBits, F rootFilter) {
	for(bool allowLow : {false, true}) {
		for(int i = 0; i < _numRegions; i++) {
			auto &region = _allRegions[i];
			// Note that Eir cuts regions in such a way that they never cross 4GiB.
			if(!allowLow && region.physicalBase < (PhysicalAddr{1} << 32))
				continue;
			if(order > region.buddyAccessor.tableOrder())
				continue;

			auto physical = region.buddyAccessor.allocate(order, addressBits,
					[&] (BuddyAccessor::AddressType rootIndex) {
						return rootFilter(region, rootIndex);
					});
			if(physical == BuddyAccessor::illegalAddress)
				continue;
			assert(!(physical % (size_t(kPageSize) << order)));
			return Allocation{&region, physical};
		}
	}
	return frg::null_opt;
}

frg::optional<PhysicalChunkAllocator::Allocation>
PhysicalChunkAllocator::allocateInClass_(int order, int addressBits, RootClass cls) {
	return search_(order, addressBits, [&] (Region &region, size_t rootIndex) {
		return region.rootClasses[rootIndex] == cls;
	});
}

frg::optional<PhysicalChunkAllocator::Allocation>
PhysicalChunkAllocator::allocateByConverting_(int order, int addressBits, RootClass to,
		size_t pinnableBudget) {
	// Prefer entirely free roots: converting a partially used root traps its evictable frames
	// in a root that can no longer be emptied.
	frg::optional<Allocation> allocation;
	for(bool requireFreeRoot : {true, false}) {
		allocation = search_(order, addressBits, [&] (Region &region, size_t rootIndex) {
			if(region.rootClasses[rootIndex] != RootClass::evictableOnly)
				return false;
			if(requireFreeRoot && region.buddyAccessor.rootFreeOrder(rootIndex)
					!= region.buddyAccessor.tableOrder())
				return false;
			// Converting to contiguousReserve makes all free pages of the root non-pinnable.
			if(to == RootClass::contiguousReserve) {
				auto freeInThisRoot = region.pagesPerRoot() - region.usedPagesOfRoot[rootIndex];
				if(freeInThisRoot > pinnableBudget)
					return false;
			}
			return true;
		});
		if(allocation)
			break;
	}
	if(!allocation)
		return frg::null_opt;

	auto &region = *allocation->region;
	convertRoot_(region, region.rootOfAddress(allocation->physical), to);
	return allocation;
}

void PhysicalChunkAllocator::convertRoot_(Region &region, size_t rootIndex, RootClass to) {
	auto freePages = region.pagesPerRoot() - region.usedPagesOfRoot[rootIndex];
	auto &fromCount = freePagesOfClass_[static_cast<size_t>(region.rootClasses[rootIndex])];
	assert(fromCount >= freePages);
	fromCount -= freePages;
	freePagesOfClass_[static_cast<size_t>(to)] += freePages;
	region.rootClasses[rootIndex] = to;
}

void PhysicalChunkAllocator::accountAllocation_(Allocation allocation, size_t numPages,
		PhysicalKind kind) {
	auto &region = *allocation.region;
	auto rootIndex = region.rootOfAddress(allocation.physical);
	// Chunks are aligned to their size relative to the region, so they never straddle roots.
	assert(region.rootOfAddress(allocation.physical + (numPages << kPageShift) - 1) == rootIndex);
	region.usedPagesOfRoot[rootIndex] += numPages;
	if(kind != PhysicalKind::evictable)
		region.unevictablePagesOfRoot[rootIndex] += numPages;
	auto &classCount = freePagesOfClass_[static_cast<size_t>(region.rootClasses[rootIndex])];
	assert(classCount >= numPages);
	classCount -= numPages;

	auto currentFree = _freePages.load(std::memory_order_relaxed);
	auto currentUsed = _usedPages.load(std::memory_order_relaxed);
	assert(currentFree >= numPages);
	_freePages.store(currentFree - numPages, std::memory_order_relaxed);
	_usedPages.store(currentUsed + numPages, std::memory_order_relaxed);
}

void PhysicalChunkAllocator::free(PhysicalAddr address, size_t size, PhysicalKind kind) {
	WaitList served;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		freeChunk_(address, size, kind);
		serveWaiters_(served);
		updateReclaimTarget_();
	}

	completeServed_(served);
}

void PhysicalChunkAllocator::freeChunk_(PhysicalAddr address, size_t size, PhysicalKind kind) {
	int target = 0;
	while(size > (size_t(kPageSize) << target))
		target++;

	for(int i = 0; i < _numRegions; i++) {
		auto &region = _allRegions[i];
		if(address < region.physicalBase)
			continue;
		if(address + size - region.physicalBase > region.regionSize)
			continue;

		region.buddyAccessor.free(address, target);

		auto numPages = size / kPageSize;
		auto rootIndex = region.rootOfAddress(address);
		assert(region.usedPagesOfRoot[rootIndex] >= numPages);
		region.usedPagesOfRoot[rootIndex] -= numPages;
		freePagesOfClass_[static_cast<size_t>(region.rootClasses[rootIndex])] += numPages;
		if(kind != PhysicalKind::evictable) {
			assert(region.unevictablePagesOfRoot[rootIndex] >= numPages);
			region.unevictablePagesOfRoot[rootIndex] -= numPages;
			if(!region.unevictablePagesOfRoot[rootIndex]
					&& region.rootClasses[rootIndex] == RootClass::admitsPinned)
				convertRoot_(region, rootIndex, RootClass::evictableOnly);
		}

		auto currentFree = _freePages.load(std::memory_order_relaxed);
		auto currentUsed = _usedPages.load(std::memory_order_relaxed);
		assert(currentUsed >= numPages);
		_freePages.store(currentFree + numPages, std::memory_order_relaxed);
		_usedPages.store(currentUsed - numPages, std::memory_order_relaxed);
		return;
	}

	assert(!"Physical page is not part of any region");
}

void PhysicalChunkAllocator::serveWaiters_(WaitList &served) {
	// Serving only the first node ensures that smaller nodes cannot overtake larger ones.
	while(auto node = waitTree_.first()) {
		if(!canAllocateFungible_(node->numPages, node->floor))
			break;
		waitTree_.remove(node);
		node->queued = false;
		node->result = allocateFungible_(node->kind, node->numPages, node->floor);
		served.push_back(node);
	}
}

void PhysicalChunkAllocator::completeServed_(WaitList &served) {
	while(!served.empty())
		served.pop_front()->event.raise();
}

PageMagazine::~PageMagazine() {
	while(!pages.empty())
		physicalAllocator->free(pages.pop(), kPageSize);
}

std::expected<void, Error> PageMagazine::refill() {
	// Reset wanted before blocking since blocking can allocate (and refill) in this context.
	auto remaining = std::exchange(wanted, 0);

	if(!remaining)
		return {};

	if(canBlockForMemory()) {
		auto blocked = physicalAllocator->allocateChainBlocking({AllocationClass::kernel, currentContextFloor()},
				PhysicalKind::pinned, remaining);
		// If blocking failed, fall back to the emergency tryAllocateChain() below.
		if(blocked) {
			assert(blocked->size() == remaining);
			pages.splice(std::move(*blocked));
			return {};
		}
	}
	// Try the emergency class if we either cannot block or if blocking failed above.
	auto emergency = physicalAllocator->tryAllocateChain({AllocationClass::emergency, 0},
			PhysicalKind::pinned, remaining);
	if(!emergency)
		return std::unexpected{emergency.error()};
	pages.splice(std::move(*emergency));
	return {};
}

void PageMagazine::refillInfallible() {
	for(auto n = std::exchange(wanted, 0); n; n--)
		pages.push(physicalAllocator->allocateInfallible(PhysicalKind::pinned));
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
