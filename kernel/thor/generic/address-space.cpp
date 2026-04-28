#include <cstddef>
#include <type_traits>
#include <frg/container_of.hpp>
#include <frg/safe_int.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/timer.hpp>
#include <thor-internal/types.hpp>

namespace thor {

namespace {
	constexpr bool logCleanup = false;
	constexpr bool logRss = true;

	// Used in working set limit computation.
	// TODO: Do not make this global and add a hierarchical API that allows userspace
	//       to control weights or similar for working set size computation.
	constinit std::atomic<size_t> numVirtualSpaces{0};

	uint32_t compilePageFlags(MappingFlags mappingFlags) {
		uint32_t pageFlags = 0;
		if(mappingFlags & MappingFlags::protRead)
			pageFlags |= page_access::read;
		if(mappingFlags & MappingFlags::protWrite)
			pageFlags |= page_access::write;
		if(mappingFlags & MappingFlags::protExecute)
			pageFlags |= page_access::execute;
		return pageFlags;
	}
}

// --------------------------------------------------------

MemorySlice::MemorySlice(smarter::shared_ptr<MemoryView> view,
		ptrdiff_t view_offset, size_t view_size, CachingFlags cachingFlags)
: _view{std::move(view)}, _viewOffset{view_offset}, _viewSize{view_size}, cachingFlags_{cachingFlags} {
	assert(!(_viewOffset & (kPageSize - 1)));
	assert(!(_viewSize & (kPageSize - 1)));
}

// --------------------------------------------------------
// HoleAggregator
// --------------------------------------------------------

bool HoleAggregator::aggregate(Hole *hole) {
	size_t size = hole->length();
	if(HoleTree::get_left(hole) && HoleTree::get_left(hole)->largestHole > size)
		size = HoleTree::get_left(hole)->largestHole;
	if(HoleTree::get_right(hole) && HoleTree::get_right(hole)->largestHole > size)
		size = HoleTree::get_right(hole)->largestHole;

	if(hole->largestHole == size)
		return false;
	hole->largestHole = size;
	return true;
}

bool HoleAggregator::check_invariant(HoleTree &tree, Hole *hole) {
	auto pred = tree.predecessor(hole);
	auto succ = tree.successor(hole);

	// Check largest hole invariant.
	size_t size = hole->length();
	if(tree.get_left(hole) && tree.get_left(hole)->largestHole > size)
		size = tree.get_left(hole)->largestHole;
	if(tree.get_right(hole) && tree.get_right(hole)->largestHole > size)
		size = tree.get_right(hole)->largestHole;

	if(hole->largestHole != size) {
		infoLogger() << "largestHole violation: " << "Expected " << size
				<< ", got " << hole->largestHole << "." << frg::endlog;
		return false;
	}

	// Check non-overlapping memory areas invariant.
	if(pred && hole->address() < pred->address() + pred->length()) {
		infoLogger() << "Non-overlapping (left) violation" << frg::endlog;
		return false;
	}
	if(succ && hole->address() + hole->length() > succ->address()) {
		infoLogger() << "Non-overlapping (right) violation" << frg::endlog;
		return false;
	}

	return true;
}

// --------------------------------------------------------
// Mapping
// --------------------------------------------------------

Mapping::Mapping(
	smarter::shared_ptr<VirtualSpace> owner,
	VirtualAddr address,
	size_t length,
	smarter::shared_ptr<MemorySlice> sl,
	uintptr_t viewOffset,
	MappingFlags flags
) : owner{std::move(owner)},
		address{address},
		length{length},
		slice{std::move(sl)},
		view{slice->getView()},
		viewOffset{viewOffset},
		flags{flags} {
	assert(viewOffset >= slice->offset());
	assert(viewOffset + length <= slice->offset() + slice->length());
}

Mapping::~Mapping() {
	assert(state.load(std::memory_order_relaxed) == MappingState::retired);
	//debugLogger() << "thor: Mapping is destructed" << frg::endlog;
}

void Mapping::protect(MappingFlags protectFlags) {
	auto newFlags = static_cast<std::underlying_type_t<MappingFlags>>(
		flags.load(std::memory_order_relaxed)
	);
	newFlags &= ~(MappingFlags::protRead | MappingFlags::protWrite | MappingFlags::protExecute);
	newFlags |= protectFlags;
	flags.store(static_cast<MappingFlags>(newFlags), std::memory_order_relaxed);
}

coroutine<void> Mapping::runEvictionLoop() {
	assert(currentIpl() == ipl::exceptionalWork);

	while(true) {
		auto eviction = co_await view->pollEviction(&observer, cancelEviction);
		if(!eviction)
			break;
		if (eviction.mode() == EvictMode::breakRange) {
			if(eviction.offset() + eviction.size() <= viewOffset
					|| eviction.offset() >= viewOffset + length) {
				eviction.done();
				continue;
			}

			// Begin and end offsets of the region that we need to unmap.
			auto shootBegin = frg::max(eviction.offset(), viewOffset);
			auto shootEnd = frg::min(eviction.offset() + eviction.size(),
					viewOffset + length);

			// Offset from the beginning of the mapping.
			auto shootOffset = shootBegin - viewOffset;
			auto shootSize = shootEnd - shootBegin;
			assert(shootSize);
			assert(!(shootOffset & (kPageSize - 1)));
			assert(!(shootSize & (kPageSize - 1)));

			co_await exposeRcu.barrier();

			bool anyRevoked;
			{
				LocalRcuEngine::Guard revokeGuard{revokeRcu};

				// Unmap the memory range.
				auto unmapOutcome = owner->_ops->unmapPages(address + shootOffset, shootSize);
				assert(unmapOutcome);
				owner->notifyRss_(unmapOutcome.value());
				anyRevoked = unmapOutcome.value().anyRevoked;

				if(anyRevoked)
					co_await owner->_ops->shootdown(address + shootOffset, shootSize);
			}
			if(!anyRevoked)
				co_await revokeRcu.barrier();
		} else if(eviction.mode() == EvictMode::fenceDirty) {
			// Ensure all in-flight revokeRcu critical sections (which will clear
			// PTE dirty bits) have completed.
			co_await revokeRcu.barrier();
		} else {
			assert(eviction.mode() == EvictMode::fenceEphemeral);

			// fenceEphemeral affects all CachePages with a use count of zero.
			// However, all mapped pages have use counts > zero.

			// Ensure that no references remain (aside from permanent ones in PTEs).
			co_await exposeRcu.barrier();
			// Ensure that prior unmapping + shootdown is complete.
			// This is needed since prior unmapping of this mapping may have decreased the use count to zero.
			co_await revokeRcu.barrier();
		}

		eviction.done();
	}

	evictionDoneEvent.raise();
}

// --------------------------------------------------------
// CowMapping
// --------------------------------------------------------

CowChain::CowChain()
: _pages{*kernelAlloc} {
}

CowChain::~CowChain() {
	if(logCleanup)
		infoLogger() << "thor: Releasing CowChain" << frg::endlog;
}

// --------------------------------------------------------
// VirtualSpace
// --------------------------------------------------------

VirtualSpace::VirtualSpace(VirtualOperations *ops, smarter::shared_ptr<Hierarchy> hierarchy)
: _ops{ops}, hierarchy_{std::move(hierarchy)} {
	numVirtualSpaces.fetch_add(1, std::memory_order_relaxed);
}

ptrdiff_t VirtualSpace::workingSetGoal_() {
	auto n = numVirtualSpaces.load(std::memory_order_relaxed);
	assert(n);
	return physicalAllocator->numTotalPages() * kPageSize / n;
}

void VirtualSpace::notifyRss_(const PagesAffected &affected) {
	rss_.fetch_add(affected.rssIncrease - affected.rssDecrease, std::memory_order_relaxed);
	agingTurnover_.fetch_add(affected.rssIncrease, std::memory_order_relaxed);
	if(shouldContinueAging_())
		agingEvent_.raise();
}

bool VirtualSpace::shouldContinueAging_() {
	// Considerations:
	// * We do not want to run aging if too few pages are mapped; otherwise, we would
	//   wrap around the address space too quickly and invest too much work for little to no gain.
	// * We want to run aging frequently enough to ensure the page ages
	//   are meaningful and not just all zero or one.
	auto goal = workingSetGoal_();
	return rss_.load(std::memory_order_relaxed) >= goal / 2
		&& agingTurnover_.load(std::memory_order_relaxed) >= goal / 5;
}

void VirtualSpace::setupInitialHole(VirtualAddr address, size_t size) {
	auto hole = frg::construct<Hole>(*kernelAlloc, address, size);
	_holes.insert(hole);
}

VirtualSpace::~VirtualSpace() {
	numVirtualSpaces.fetch_sub(1, std::memory_order_relaxed);

	if(logCleanup)
		debugLogger() << "thor: VirtualSpace is destructed" << frg::endlog;

	while(_holes.get_root()) {
		auto hole = _holes.get_root();
		_holes.remove(hole);
		frg::destruct(*kernelAlloc, hole);
	}

	assert(rss_.load(std::memory_order_relaxed) == 0);
}

void VirtualSpace::retire() {
	if(logCleanup)
		debugLogger() << "thor: VirtualSpace is cleared" << frg::endlog;

	// TODO: It would be less ugly to run this in a non-detached way.
	spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(), [] (smarter::shared_ptr<VirtualSpace> self)
			-> coroutine<void> {
		self->cancelAging_.cancel();
		co_await self->agingDoneEvent_.wait();

		co_await self->_consistencyMutex.async_lock();
		frg::unique_lock consistencyLock{frg::adopt_lock, self->_consistencyMutex};

		auto mapping = self->_mappings.first();
		while(mapping) {
			assert(mapping->state.load(std::memory_order_relaxed) == MappingState::active);
			mapping->state.store(MappingState::zombie, std::memory_order_relaxed);

			co_await mapping->exposeRcu.barrier();

			auto unmapOutcome = self->_ops->unmapPages(mapping->address, mapping->length);
			assert(unmapOutcome);
			self->notifyRss_(unmapOutcome.value());

			mapping = MappingTree::successor(mapping);
		}

		co_await self->_ops->retire();

		while(self->_mappings.get_root()) {
			auto mapping = self->_mappings.get_root();
			self->_mappings.remove(mapping);

			assert(mapping->state.load(std::memory_order_relaxed) == MappingState::zombie);
			mapping->state.store(MappingState::retired, std::memory_order_relaxed);

			if(mapping->view->canEvictMemory()) {
				mapping->cancelEviction.cancel();
				co_await mapping->evictionDoneEvent.wait();
			}
			mapping->view->removeObserver(&mapping->observer);
			mapping->selfPtr.policy().decrement();
		}
	}(selfPtr.lock()));
}

coroutine<void> VirtualSpace::runAgingLoop() {
	auto self = selfPtr.lock();

	// This function scans over the entire address space in a circular fashion.
	// It does not do one pass at a time, instead we stop when shouldContinueAging_() becomes false.
	// nextAddress is the virtual address that we will continue at.
	VirtualAddr nextAddress{0};
	while(true) {
		auto waitOutcome = co_await agingEvent_.async_wait_if([&] {
			return !shouldContinueAging_();
		}, cancelAging_);

		if (!waitOutcome)
			break;

		while(shouldContinueAging_()) {
			if (logRss && !nextAddress) {
				// Only log on wrap-around to avoid log spam.
				infoLogger() << frg::fmt(
					"thor: {} RSS: 0x{:x}, goal: 0x{:x}",
					hierarchy_->tag(),
					rss_.load(std::memory_order_relaxed),
					workingSetGoal_()
				) << frg::endlog;
			}

			smarter::shared_ptr<Mapping> mapping;
			{
				auto irqLock = frg::guard(&irqMutex());
				auto spaceGuard = frg::guard(&_snapshotMutex);
				auto node = _mappings.get_root();
				Mapping *candidate = nullptr;
				while(node) {
					if(node->address + node->length <= nextAddress) {
						node = MappingTree::get_right(node);
					} else if(node->address >= nextAddress) {
						candidate = node;
						node = MappingTree::get_left(node);
					} else {
						candidate = node;
						break;
					}
				}
				if(candidate)
					mapping = candidate->selfPtr.lock();
			}

			// Wrap-around when we reach the end of the address space.
			if(!mapping) {
				nextAddress = 0;
				continue;
			}
			nextAddress = mapping->address + mapping->length;

			if(mapping->state.load(std::memory_order_relaxed) != MappingState::active)
				continue;

			bool anyRevoked;
			{
				LocalRcuEngine::Guard revokeGuard{mapping->revokeRcu};

				bool vacate = rss_.load(std::memory_order_relaxed) > workingSetGoal_();
				auto ageOutcome = _ops->agePages(mapping->address, mapping->length, vacate);
				assert(ageOutcome);
				agingTurnover_.fetch_sub(ageOutcome.value().scanned, std::memory_order_relaxed);
				notifyRss_(ageOutcome.value());
				anyRevoked = ageOutcome.value().anyRevoked;

				if(anyRevoked)
					co_await _ops->shootdown(mapping->address, mapping->length);
			}
			if(!anyRevoked)
				co_await mapping->revokeRcu.barrier();
		}
	}

	agingDoneEvent_.raise();
}

coroutine<frg::expected<Error, VirtualAddr>>
VirtualSpace::map(smarter::borrowed_ptr<MemorySlice> slice,
		VirtualAddr address, size_t offset, size_t length, uint32_t flags) {
	assert(currentIpl() == ipl::exceptionalWork);
	assert(length);
	assert(!(length % kPageSize));

	size_t endOffset;
	if (!(frg::safe_int{offset} + frg::safe_int{length}).into(endOffset))
		co_return Error::illegalArgs;
	if(endOffset > slice->length())
		co_return Error::bufferTooSmall;

	co_await _consistencyMutex.async_lock();
	frg::unique_lock consistencyLock{frg::adopt_lock, _consistencyMutex};

	if (flags & kMapFixed) {
		auto [start, end] = co_await _splitMappings(address, length);
		assert(start || (!start && !end));
		co_await _unmapMappings(address, length, start, end);
	}

	// The shared_ptr to the new Mapping needs to survive until the locks are released.
	VirtualAddr actualAddress;
	smarter::shared_ptr<Mapping> mapping;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto spaceLock = frg::guard(&_snapshotMutex);

		assert((address % kPageSize) == 0);
		if(flags & kMapFixed) {
			actualAddress = FRG_CO_TRY(_allocateAt(address, length));
		}else if(flags & kMapFixedNoReplace) {
			if(_areMappingsInRange(address, length)) {
				co_return Error::alreadyExists;
			}
			actualAddress = FRG_CO_TRY(_allocateAt(address, length));
		}else{
			if(address && !_areMappingsInRange(address, length)) {
				if(auto res = _allocateAt(address, length)) {
					actualAddress = res.unwrap();
				}else {
					actualAddress = FRG_CO_TRY(_allocate(length, flags));
				}
			}else {
				actualAddress = FRG_CO_TRY(_allocate(length, flags));
			}
		}

	//	infoLogger() << "Creating new mapping at " << (void *)actualAddress
	//			<< ", length: " << (void *)length << frg::endlog;

		// Setup a new Mapping object.
		std::underlying_type_t<MappingFlags> mappingFlags = 0;

		// TODO: The upgrading mechanism needs to be arch-specific:
		// Some archs might only support RX, while other support X.
		auto mask = kMapProtRead | kMapProtWrite | kMapProtExecute;
		if((flags & mask) == (kMapProtRead | kMapProtWrite | kMapProtExecute)
				|| (flags & mask) == (kMapProtWrite | kMapProtExecute)) {
			// WX is upgraded to RWX.
			mappingFlags |= MappingFlags::protRead | MappingFlags::protWrite
				| MappingFlags::protExecute;
		}else if((flags & mask) == (kMapProtRead | kMapProtExecute)
				|| (flags & mask) == kMapProtExecute) {
			// X is upgraded to RX.
			mappingFlags |= MappingFlags::protRead | MappingFlags::protExecute;
		}else if((flags & mask) == (kMapProtRead | kMapProtWrite)
				|| (flags & mask) == kMapProtWrite) {
			// W is upgraded to RW.
			mappingFlags |= MappingFlags::protRead | MappingFlags::protWrite;
		}else if((flags & mask) == kMapProtRead) {
			mappingFlags |= MappingFlags::protRead;
		}else{
			assert(!(flags & mask));
		}

		if(flags & kMapDontRequireBacking)
			mappingFlags |= MappingFlags::dontRequireBacking;

		mapping = smarter::allocate_shared<Mapping>(Allocator{},
			selfPtr.lock(),
			actualAddress,
			length,
			slice.lock(),
			slice->offset() + offset,
			static_cast<MappingFlags>(mappingFlags)
		);
		mapping->selfPtr = mapping;

		// Install the new mapping object.
		_mappings.insert(mapping.get());

		assert(mapping->state.load(std::memory_order_relaxed) == MappingState::null);
		mapping->state.store(MappingState::active, std::memory_order_relaxed);

		// We keep one reference until the detach the observer.
		mapping.policy().increment();
		mapping->view->addObserver(&mapping->observer);

	}

	// Not populating the range is the default.
	// Populating is quite expensive on CoW memory, mostly due to additional shootdowns
	// that need to happen when an already mapped page is unmapped during copy-on-write.
	if (flags & kMapPopulate) {
		auto caching = CachingMode::null;
		if(mapping->slice->getCachingFlags() == cacheWriteCombine)
			caching = CachingMode::writeCombine;

		LocalRcuEngine::Guard exposeGuard{mapping->exposeRcu};

		if(mapping->state.load(std::memory_order_relaxed) == MappingState::active) {
			auto actualMappingFlags = mapping->flags.load(std::memory_order_relaxed);
			uint32_t pageFlags = 0;
			if((actualMappingFlags & MappingFlags::permissionMask) & MappingFlags::protWrite)
				pageFlags |= page_access::write;
			if((actualMappingFlags & MappingFlags::permissionMask) & MappingFlags::protExecute)
				pageFlags |= page_access::execute;
			if((actualMappingFlags & MappingFlags::permissionMask) & MappingFlags::protRead)
				pageFlags |= page_access::read;

			{
				LocalRcuEngine::Guard revokeGuard{mapping->revokeRcu};

				auto mapOutcome = _ops->mapPresentPages(mapping->address, mapping->view.get(),
						mapping->viewOffset, mapping->length, pageFlags, caching);
				assert(mapOutcome);
				notifyRss_(mapOutcome.value());
				if(mapOutcome.value().anyRevoked)
					co_await _ops->shootdown(mapping->address, mapping->length);
			}
		}
	}

	if(mapping->view->canEvictMemory())
		spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(), mapping->runEvictionLoop());

	co_return actualAddress;
}

coroutine<frg::expected<Error>>
VirtualSpace::protect(VirtualAddr address, size_t length, uint32_t flags) {
	assert(currentIpl() == ipl::exceptionalWork);

	std::underlying_type_t<MappingFlags> mappingFlags = 0;

	// TODO: The upgrading mechanism needs to be arch-specific:
	// Some archs might only support RX, while other support X.
	auto mask = kMapProtRead | kMapProtWrite | kMapProtExecute;
	if((flags & mask) == (kMapProtRead | kMapProtWrite | kMapProtExecute)
			|| (flags & mask) == (kMapProtWrite | kMapProtExecute)) {
		// WX is upgraded to RWX.
		mappingFlags |= MappingFlags::protRead | MappingFlags::protWrite
			| MappingFlags::protExecute;
	}else if((flags & mask) == (kMapProtRead | kMapProtExecute)
			|| (flags & mask) == kMapProtExecute) {
		// X is upgraded to RX.
		mappingFlags |= MappingFlags::protRead | MappingFlags::protExecute;
	}else if((flags & mask) == (kMapProtRead | kMapProtWrite)
			|| (flags & mask) == kMapProtWrite) {
		// W is upgraded to RW.
		mappingFlags |= MappingFlags::protRead | MappingFlags::protWrite;
	}else if((flags & mask) == kMapProtRead) {
		mappingFlags |= MappingFlags::protRead;
	}else{
		assert(!(flags & mask));
	}

	co_await _consistencyMutex.async_lock();
	frg::unique_lock consistencyLock{frg::adopt_lock, _consistencyMutex};

	auto [start, end] = co_await _splitMappings(address, length);
	assert(start || (!start && !end));
	for (auto it = start; it != end;) {
		auto mapping = it->selfPtr.lock();
		it = MappingTree::successor(it);

		mapping->protect(static_cast<MappingFlags>(mappingFlags));

		assert(mapping->state.load(std::memory_order_relaxed) == MappingState::active);

		auto actualMappingFlags = mapping->flags.load(std::memory_order_relaxed);
		uint32_t pageFlags = 0;
		if((actualMappingFlags & MappingFlags::permissionMask) & MappingFlags::protWrite)
			pageFlags |= page_access::write;
		if((actualMappingFlags & MappingFlags::permissionMask) & MappingFlags::protExecute)
			pageFlags |= page_access::execute;
		if((actualMappingFlags & MappingFlags::permissionMask) & MappingFlags::protRead)
			pageFlags |= page_access::read;

		auto caching = CachingMode::null;
		if(mapping->slice->getCachingFlags() == cacheWriteCombine)
			caching = CachingMode::writeCombine;

		co_await mapping->exposeRcu.barrier();

		bool anyRevoked;
		{
			LocalRcuEngine::Guard revokeGuard{mapping->revokeRcu};

			auto restrictOutcome = _ops->restrictPages(mapping->address,
					mapping->length, pageFlags, caching);
			assert(restrictOutcome);
			anyRevoked = restrictOutcome.value().anyRevoked;

			if(anyRevoked)
				co_await _ops->shootdown(mapping->address, mapping->length);
		}
		if(!anyRevoked)
			co_await mapping->revokeRcu.barrier();
	}

	co_return {};
}

coroutine<frg::expected<Error>> VirtualSpace::unmap(VirtualAddr address, size_t length) {
	assert(currentIpl() == ipl::exceptionalWork);

	co_await _consistencyMutex.async_lock();
	frg::unique_lock consistencyLock{frg::adopt_lock, _consistencyMutex};

	auto [start, end] = co_await _splitMappings(address, length);
	assert(start || (!start && !end));
	co_await _unmapMappings(address, length, start, end);

	co_return {};
}

coroutine<frg::expected<Error>>
VirtualSpace::synchronize(VirtualAddr address, size_t size) {
	assert(currentIpl() == ipl::exceptionalWork);

	auto misalign = address & (kPageSize - 1);
	auto alignedAddress = address & ~(kPageSize - 1);
	auto alignedSize = (size + misalign + kPageSize - 1) & ~(kPageSize - 1);

	size_t overallProgress = 0;
	while(overallProgress < alignedSize) {
		smarter::shared_ptr<Mapping> mapping;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto spaceGuard = frg::guard(&_snapshotMutex);

			mapping = _findMapping(alignedAddress + overallProgress);
		}
		if(!mapping)
			co_return Error::fault;

		auto mappingOffset = alignedAddress + overallProgress - mapping->address;
		auto mappingChunk = frg::min(alignedSize - overallProgress,
				mapping->length - mappingOffset);
		assert(mapping->state.load(std::memory_order_relaxed) == MappingState::active);
		assert(mappingOffset + mappingChunk <= mapping->length);

		bool anyRevoked;
		{
			LocalRcuEngine::Guard revokeGuard{mapping->revokeRcu};

			auto cleanOutcome = _ops->cleanPages(mapping->address + mappingOffset, mappingChunk);
			assert(cleanOutcome);
			anyRevoked = cleanOutcome.value().anyRevoked;
			if(anyRevoked)
				co_await _ops->shootdown(mapping->address + mappingOffset, mappingChunk);
		}

		overallProgress += mappingChunk;
	}

	co_return {};
}

coroutine<frg::expected<Error>>
VirtualSpace::handleFault(VirtualAddr address, uint32_t faultFlags) {
	assert(currentIpl() == ipl::exceptionalWork);

	while (true) {
		smarter::shared_ptr<Mapping> mapping;
		{
			auto irq_lock = frg::guard(&irqMutex());
			auto space_guard = frg::guard(&_snapshotMutex);

			mapping = _findMapping(address);
		}
		if(!mapping)
			co_return Error::fault;

		// Check access attributes.
		// Since this is not in a critical section, they may be stale by the time
		// we get to the touchRange() or peekRange() calls below.
		// However, this avoids touchRange() calls if the permissions are already violated here.
		auto flags = mapping->flags.load(std::memory_order_relaxed);
		if(faultFlags & VirtualSpace::kFaultWrite) {
			if (!(flags & MappingFlags::protWrite))
				co_return Error::badPermissions;
		}
		if(faultFlags & VirtualSpace::kFaultExecute) {
			if (!(flags & MappingFlags::protExecute))
				co_return Error::badPermissions;
		}

		// TODO: Aligning should not be necessary here.
		auto offset = (address - mapping->address) & ~(kPageSize - 1);

		FetchFlags fetchFlags = 0;
		if(flags & MappingFlags::dontRequireBacking)
			fetchFlags |= fetchDisallowBacking;
		if(faultFlags & VirtualSpace::kFaultWrite)
			fetchFlags |= fetchRequireMutable;

		// Calling touchRange() on stale mappings is allowed,
		// so we do not enter a critical section here.
		FRG_CO_TRY(co_await mapping->view->touchRange(
				mapping->viewOffset + offset, kPageSize, fetchFlags));

		auto caching = CachingMode::null;
		if(mapping->slice->getCachingFlags() == cacheWriteCombine)
			caching = CachingMode::writeCombine;

		// Try to fault in the page.
		{
			LocalRcuEngine::Guard exposeGuard{mapping->exposeRcu};

			if (mapping->state.load(std::memory_order_relaxed) != MappingState::active)
				continue;
			auto flags = mapping->flags.load(std::memory_order_relaxed);

			{
				LocalRcuEngine::Guard revokeGuard{mapping->revokeRcu};

				auto remapOutcome = _ops->faultPage(
					address & ~(kPageSize - 1),
					mapping->view.get(),
					mapping->viewOffset + offset,
					fetchFlags,
					compilePageFlags(flags),
					caching
				);
				if(!remapOutcome) {
					if(remapOutcome.error() == Error::spuriousOperation) {
						// Spurious page faults are the result of race conditions.
						// They should be rare. If they happen too often, something is probably wrong!
						warningLogger() << "thor: Spurious page fault" << frg::endlog;
					}else{
						assert(remapOutcome.error() == Error::fault);
						warningLogger() << "thor: Page still not available after touchRange()"
							<< frg::endlog;
						continue;
					}
				} else {
					notifyRss_(remapOutcome.value());
					if(remapOutcome.value().anyRevoked)
						co_await _ops->shootdown(address & ~(kPageSize - 1), kPageSize);
				}
			}
			co_return {};
		}
	}
}

coroutine<frg::expected<Error, PhysicalAddr>>
VirtualSpace::retrievePhysical(VirtualAddr address) {
	// We do not take _consistencyMutex here since we are only interested in a snapshot.

	smarter::shared_ptr<Mapping> mapping;
	{
		auto irq_lock = frg::guard(&irqMutex());
		auto space_guard = frg::guard(&_snapshotMutex);

		mapping = _findMapping(address);
	}
	if(!mapping)
		co_return Error::fault;

	// TODO: Aligning should not be necessary here.
	auto offset = (address - mapping->address) & ~(kPageSize - 1);

	while(true) {
		FetchFlags fetchFlags = fetchRequireMutable;
		if(mapping->flags.load(std::memory_order_relaxed) & MappingFlags::dontRequireBacking)
			fetchFlags |= fetchDisallowBacking;

		FRG_CO_TRY(co_await mapping->view->touchRange(
				mapping->viewOffset + offset, kPageSize, fetchFlags));

		LocalRcuEngine::Guard exposeGuard{mapping->exposeRcu};
		auto physicalRange = mapping->view->peekRange(mapping->viewOffset + offset, fetchFlags);
		if(physicalRange.physical == PhysicalAddr(-1)) {
			warningLogger() << "thor: Page still not available after touchRange()" << frg::endlog;
			continue;
		}

		co_return physicalRange.physical;
	}
}

smarter::shared_ptr<Mapping> VirtualSpace::_findMapping(VirtualAddr address) {
	auto current = _mappings.get_root();
	while(current) {
		if(address < current->address) {
			current = MappingTree::get_left(current);
		}else if(address >= current->address + current->length) {
			current = MappingTree::get_right(current);
		}else{
			assert(address >= current->address
					&& address < current->address + current->length);
			return current->selfPtr.lock();
		}
	}

	return nullptr;
}

bool VirtualSpace::_areMappingsInRange(VirtualAddr address, size_t length) {
	auto end = address + length;

	auto current = _mappings.get_root();
	while(current) {
		auto currentEnd = current->address + current->length;
		if(address < currentEnd && current->address < end) {
			return true;
		}

		if(address < current->address) {
			current = MappingTree::get_left(current);
		}else if(address > current->address) {
			current = MappingTree::get_right(current);
		}else {
			return true;
		}
	}
	return false;
}

frg::expected<Error, VirtualAddr> VirtualSpace::_allocate(size_t length, MapFlags flags) {
	assert(length > 0);
	assert((length % kPageSize) == 0);
//	infoLogger() << "Allocate virtual memory area"
//			<< ", size: 0x" << frg::hex_fmt(length) << frg::endlog;

	if(_holes.get_root()->largestHole < length)
		return Error::noMemory;

	auto current = _holes.get_root();
	while(true) {
		if(flags & kMapPreferBottom) {
			// Try to allocate memory at the bottom of the range.
			if(HoleTree::get_left(current)
					&& HoleTree::get_left(current)->largestHole >= length) {
				current = HoleTree::get_left(current);
				continue;
			}

			if(current->length() >= length) {
				// Note that _splitHole can deallocate the hole!
				auto address = current->address();
				_splitHole(current, 0, length);
				return address;
			}

			assert(HoleTree::get_right(current));
			assert(HoleTree::get_right(current)->largestHole >= length);
			current = HoleTree::get_right(current);
		}else{
			// Try to allocate memory at the top of the range.
			assert(flags & kMapPreferTop);

			if(HoleTree::get_right(current)
					&& HoleTree::get_right(current)->largestHole >= length) {
				current = HoleTree::get_right(current);
				continue;
			}

			if(current->length() >= length) {
				// Note that _splitHole can deallocate the hole!
				auto offset = current->length() - length;
				auto address = current->address() + offset;
				_splitHole(current, offset, length);
				return address;
			}

			assert(HoleTree::get_left(current));
			assert(HoleTree::get_left(current)->largestHole >= length);
			current = HoleTree::get_left(current);
		}
	}
}

frg::expected<Error, VirtualAddr> VirtualSpace::_allocateAt(VirtualAddr address, size_t length) {
	assert(!(address % kPageSize));
	assert(!(length % kPageSize));

	auto current = _holes.get_root();
	while(true) {
		if(!current) {
			return Error::noMemory;
		}

		if(address < current->address()) {
			current = HoleTree::get_left(current);
		}else if(address >= current->address() + current->length()) {
			current = HoleTree::get_right(current);
		}else{
			assert(address >= current->address()
					&& address < current->address() + current->length());
			break;
		}
	}

	if(address - current->address() + length > current->length()) {
		return Error::noMemory;
	}
	_splitHole(current, address - current->address(), length);
	return address;
}

void VirtualSpace::_splitHole(Hole *hole, VirtualAddr offset, size_t length) {
	assert(length);
	assert(offset + length <= hole->length());

	_holes.remove(hole);

	if(offset) {
		auto predecessor = frg::construct<Hole>(*kernelAlloc, hole->address(), offset);
		_holes.insert(predecessor);
	}

	if(offset + length < hole->length()) {
		auto successor = frg::construct<Hole>(*kernelAlloc,
				hole->address() + offset + length, hole->length() - (offset + length));
		_holes.insert(successor);
	}

	frg::destruct(*kernelAlloc, hole);
}

coroutine<frg::tuple<Mapping *, Mapping *>> VirtualSpace::_splitMappings(uintptr_t address, size_t size) {
	// _consistencyMutex is held here by the caller

	auto left = _mappings.get_root();
	while (left) {
		if (auto next = MappingTree::get_left(left))
			left = next;
		else
			break;
	}

	Mapping *start = nullptr, *end = nullptr;
	for (auto it = left; it;) {
		if ((it->address + it->length) <= address) {
			it = MappingTree::successor(it);
			start = it;
			continue;
		}

		if (it->address >= (address + size)) {
			// If no mapping fell into the range, don't set the end iterator
			if (start)
				end = it;

			break;
		}

		if (!start)
			start = it;

		auto mapping = it->selfPtr.lock();
		auto at = address;

		// Starting split not within mapping, consider end split
		if (at <= mapping->address)
			at = address + size;

		if (at > mapping->address && at < (mapping->address + mapping->length)) {
			// Split mapping into left and right part
			smarter::shared_ptr<Mapping> leftMapping = nullptr;
			smarter::shared_ptr<Mapping> rightMapping = nullptr;

			assert(mapping->state.load(std::memory_order_relaxed) == MappingState::active);
			mapping->state.store(MappingState::zombie, std::memory_order_relaxed);

			{
				auto leftSize = at - mapping->address;
				leftMapping = smarter::allocate_shared<Mapping>(
					Allocator{},
					selfPtr.lock(),
					mapping->address,
					leftSize,
					mapping->slice,
					mapping->viewOffset,
					mapping->flags.load(std::memory_order_relaxed)
				);
				leftMapping->selfPtr = leftMapping;
			}

			{
				auto rightOffset = at - mapping->address;
				rightMapping = smarter::allocate_shared<Mapping>(
					Allocator{},
					selfPtr.lock(),
					at,
					mapping->length - rightOffset,
					mapping->slice,
					mapping->viewOffset + rightOffset,
					mapping->flags.load(std::memory_order_relaxed)
				);
				rightMapping->selfPtr = rightMapping;
			}

			assert(leftMapping && rightMapping);

			// Now remove the mapping and insert the new mappings.
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&_snapshotMutex);

				_mappings.remove(mapping.get());

				_mappings.insert(leftMapping.get());
				assert(leftMapping->state.load(std::memory_order_relaxed) == MappingState::null);
				leftMapping->state.store(MappingState::active, std::memory_order_relaxed);

				_mappings.insert(rightMapping.get());
				assert(rightMapping->state.load(std::memory_order_relaxed) == MappingState::null);
				rightMapping->state.store(MappingState::active, std::memory_order_relaxed);
			}

			// Retire the old mapping and start using the new ones.
			// We keep one reference until the detach the observer.
			leftMapping.policy().increment();
			leftMapping->view->addObserver(&leftMapping->observer);
			if (leftMapping->view->canEvictMemory())
				spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(), leftMapping->runEvictionLoop());

			// We keep one reference until the detach the observer.
			rightMapping.policy().increment();
			rightMapping->view->addObserver(&rightMapping->observer);
			if (rightMapping->view->canEvictMemory())
				spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(), rightMapping->runEvictionLoop());

			assert(mapping->state.load(std::memory_order_relaxed) == MappingState::zombie);
			mapping->state.store(MappingState::retired, std::memory_order_relaxed);

			if (mapping->view->canEvictMemory()) {
				mapping->cancelEviction.cancel();
				co_await mapping->evictionDoneEvent.wait();
			}
			mapping->view->removeObserver(&mapping->observer);
			mapping->selfPtr.policy().decrement();

			// If start pointed to the freshly-removed mapping,
			// determine the correct mapping to use as our new start.
			// Generally, this will be the left mapping, however, if we split the mapping 3 ways
			// so as to choose the center one, this will be the right mapping.
			if (start == it) {
				if(address < at) {
					start = leftMapping.get();
				} else {
					start = rightMapping.get();
				}
			}

			it = rightMapping.get();
		} else {
			it = MappingTree::successor(it);
		}
	}

	co_return frg::make_tuple(start, end);
}

coroutine<void> VirtualSpace::_unmapMappings(VirtualAddr address, size_t length, Mapping *start, Mapping *end) {
	for (auto it = start; it != end;) {
		auto mapping = it->selfPtr.lock();
		it = MappingTree::successor(it);

		if (mapping->address >= address && (mapping->address + mapping->length) <= (address + length)) {
			assert(mapping->state.load(std::memory_order_relaxed) == MappingState::active);
			mapping->state.store(MappingState::zombie, std::memory_order_relaxed);

			co_await mapping->exposeRcu.barrier();

			bool anyRevoked;
			{
				LocalRcuEngine::Guard revokeGuard{mapping->revokeRcu};

				// Mark pages as dirty and unmap without holding a lock.
				auto unmapOutcome = _ops->unmapPages(mapping->address, mapping->length);
				assert(unmapOutcome);
				notifyRss_(unmapOutcome.value());
				anyRevoked = unmapOutcome.value().anyRevoked;

				if(anyRevoked)
					co_await _ops->shootdown(mapping->address, mapping->length);
			}
			if(!anyRevoked)
				co_await mapping->revokeRcu.barrier();

			_mappings.remove(mapping.get());

			assert(mapping->state.load(std::memory_order_relaxed) == MappingState::zombie);
			mapping->state.store(MappingState::retired, std::memory_order_relaxed);

			if(mapping->view->canEvictMemory()) {
				mapping->cancelEviction.cancel();
				co_await mapping->evictionDoneEvent.wait();
			}
			mapping->view->removeObserver(&mapping->observer);
			mapping->selfPtr.policy().decrement();

			// Finally, coalesce the hole in the hole tree.

			// Find the holes that preceede/succeede mapping.
			Hole *pre;
			Hole *succ;

			auto current = _holes.get_root();
			while(true) {
				assert(current);
				if(mapping->address < current->address()) {
					if(HoleTree::get_left(current)) {
						current = HoleTree::get_left(current);
					}else{
						pre = HoleTree::predecessor(current);
						succ = current;
						break;
					}
				}else{
					assert(mapping->address >= current->address() + current->length());
					if(HoleTree::get_right(current)) {
						current = HoleTree::get_right(current);
					}else{
						pre = current;
						succ = HoleTree::successor(current);
						break;
					}
				}
			}

			// Try to merge the new hole and the existing ones.
			if(pre && pre->address() + pre->length() == mapping->address
					&& succ && mapping->address + mapping->length == succ->address()) {
				auto hole = frg::construct<Hole>(*kernelAlloc, pre->address(),
						pre->length() + mapping->length + succ->length());

				_holes.remove(pre);
				_holes.remove(succ);
				_holes.insert(hole);
				frg::destruct(*kernelAlloc, pre);
				frg::destruct(*kernelAlloc, succ);
			}else if(pre && pre->address() + pre->length() == mapping->address) {
				auto hole = frg::construct<Hole>(*kernelAlloc,
						pre->address(), pre->length() + mapping->length);

				_holes.remove(pre);
				_holes.insert(hole);
				frg::destruct(*kernelAlloc, pre);
			}else if(succ && mapping->address + mapping->length == succ->address()) {
				auto hole = frg::construct<Hole>(*kernelAlloc,
						mapping->address, mapping->length + succ->length());

				_holes.remove(succ);
				_holes.insert(hole);
				frg::destruct(*kernelAlloc, succ);
			}else{
				auto hole = frg::construct<Hole>(*kernelAlloc,
						mapping->address, mapping->length);

				_holes.insert(hole);
			}
		}
	}
}

coroutine<size_t> VirtualSpace::readPartialSpace(uintptr_t address,
		void *buffer, size_t size) {
	assert(currentIpl() == ipl::exceptionalWork);

	// We do not take _consistencyMutex here since we are only interested in a snapshot.

	size_t progress = 0;
	while(progress < size) {
		smarter::shared_ptr<Mapping> mapping;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto spaceGuard = frg::guard(&_snapshotMutex);

			mapping = _findMapping(address + progress);
		}
		if(!mapping)
			co_return progress;

		auto startInMapping = address + progress - mapping->address;
		auto limitInMapping = frg::min(size - progress, mapping->length - startInMapping);
		// Otherwise, _findMapping() would have returned garbage.
		assert(limitInMapping);

		FetchFlags fetchFlags = 0;
		if(mapping->flags.load(std::memory_order_relaxed) & MappingFlags::dontRequireBacking)
			fetchFlags |= fetchDisallowBacking;

		auto copyOutcome = co_await mapping->view->copyFrom(
				mapping->viewOffset + startInMapping,
				reinterpret_cast<std::byte *>(buffer) + progress,
				limitInMapping, fetchFlags);
		if(!copyOutcome)
			co_return progress;

		progress += limitInMapping;
	}
	co_return progress;
}

coroutine<size_t> VirtualSpace::writePartialSpace(uintptr_t address,
		const void *buffer, size_t size) {
	assert(currentIpl() == ipl::exceptionalWork);

	// We do not take _consistencyMutex here since we are only interested in a snapshot.

	size_t progress = 0;
	while(progress < size) {
		smarter::shared_ptr<Mapping> mapping;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto spaceGuard = frg::guard(&_snapshotMutex);

			mapping = _findMapping(address + progress);
		}
		if(!mapping)
			co_return progress;

		auto startInMapping = address + progress - mapping->address;
		auto limitInMapping = frg::min(size - progress, mapping->length - startInMapping);
		// Otherwise, _findMapping() would have returned garbage.
		assert(limitInMapping);

		FetchFlags fetchFlags = 0;
		if(mapping->flags.load(std::memory_order_relaxed) & MappingFlags::dontRequireBacking)
			fetchFlags |= fetchDisallowBacking;

		auto copyOutcome = co_await mapping->view->copyTo(
				mapping->viewOffset + startInMapping,
				reinterpret_cast<const std::byte *>(buffer) + progress,
				limitInMapping, fetchFlags);
		if(!copyOutcome)
			co_return progress;

		progress += limitInMapping;
	}
	co_return progress;
}

// --------------------------------------------------------
// AddressSpace
// --------------------------------------------------------

void AddressSpace::activate(smarter::shared_ptr<AddressSpace, BindableHandle> space) {
	auto pageSpace = &space->pageSpace_;
	PageSpace::activate(smarter::shared_ptr<PageSpace>{space->selfPtr.lock(), pageSpace});
}

AddressSpace::AddressSpace(smarter::shared_ptr<Hierarchy> hierarchy)
: VirtualSpace{&ops_, std::move(hierarchy)}, ops_{this} { }

AddressSpace::~AddressSpace() { }

void AddressSpace::dispose() {
	retire();
}

// --------------------------------------------------------
// MemoryViewLockHandle.
// --------------------------------------------------------

MemoryViewLockHandle::~MemoryViewLockHandle() {
	if(_active)
		_view->unlockRange(_offset, _size);
}

// --------------------------------------------------------
// NamedMemoryViewLock.
// --------------------------------------------------------

NamedMemoryViewLock::~NamedMemoryViewLock() { }

} // namespace thor
