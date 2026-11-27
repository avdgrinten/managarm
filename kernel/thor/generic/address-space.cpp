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
	constexpr bool logRss = false;

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

std::expected<smarter::shared_ptr<MemorySlice>, Error> MemorySlice::create(
		smarter::shared_ptr<MemoryView> view, ptrdiff_t view_offset, size_t view_size,
		CachingFlags cachingFlags) {
	if((view_offset & (kPageSize - 1)) || (view_size & (kPageSize - 1)))
		return std::unexpected{Error::illegalArgs};

	auto ptr = allocate_rcu_shared<MemorySlice>(*kernelAlloc, CtorToken{},
			std::move(view), view_offset, view_size, cachingFlags);
	return ptr;
}

MemorySlice::MemorySlice(CtorToken, smarter::shared_ptr<MemoryView> view,
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
) : MemoryObserver{viewOffset, length},
		owner{std::move(owner)},
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

coroutine<void> Mapping::evict(EvictMode mode, uintptr_t offset, size_t size) {
	assert(currentIpl() == ipl::exceptionalWork);

	if(offset + size <= viewOffset
			|| offset >= viewOffset + length)
		co_return;

	// Begin and end offsets of the region that we need to unmap or clean.
	auto shootBegin = frg::max(offset, viewOffset);
	auto shootEnd = frg::min(offset + size,
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

		if(mode == EvictMode::cleanRange) {
			auto cleanOutcome = co_await revokePages(owner->_ops,
					address + shootOffset, shootSize, tracksDirty(),
					[&] (VirtualAddr va, size_t size, RevokeBatch &batch) {
						return owner->_ops->cleanPages(va, size, batch);
					});
			assert(cleanOutcome);
			anyRevoked = cleanOutcome.value().anyRevoked;
		} else {
			assert(mode == EvictMode::breakRange);
			auto unmapOutcome = co_await revokePages(owner->_ops,
					address + shootOffset, shootSize, tracksDirty(),
					[&] (VirtualAddr va, size_t size, RevokeBatch &batch) {
						return owner->_ops->unmapPages(va, size, batch);
					});
			assert(unmapOutcome);
			owner->notifyRss_(unmapOutcome.value());
			anyRevoked = unmapOutcome.value().anyRevoked;
		}
	}
	if(!anyRevoked)
		co_await revokeRcu.barrier();
}

// --------------------------------------------------------
// VirtualSpace
// --------------------------------------------------------

VirtualSpace::VirtualSpace(VirtualOperations *ops)
: _ops{ops} {
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
	spawnOnWorkQueue(*kernelAlloc, WorkQueue::memoryQueue().lock(), [] (smarter::shared_ptr<VirtualSpace> self)
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

			{
				LocalRcuEngine::Guard revokeGuard{mapping->revokeRcu};

				auto unmapOutcome = co_await revokePages(self->_ops,
						mapping->address, mapping->length, mapping->tracksDirty(),
						[&] (VirtualAddr va, size_t size, RevokeBatch &batch) {
							return self->_ops->unmapPages(va, size, batch);
						});
				assert(unmapOutcome);
				self->notifyRss_(unmapOutcome.value());
			}

			mapping = MappingTree::successor(mapping);
		}

		co_await self->_ops->retire();

		while(self->_mappings.get_root()) {
			auto mapping = self->_mappings.get_root();

			{
				auto irqLock = frg::guard(&irqMutex());
				auto snapshotLock = frg::guard(&self->_snapshotMutex);

				self->_mappings.remove(mapping);
			}

			assert(mapping->state.load(std::memory_order_relaxed) == MappingState::zombie);
			mapping->state.store(MappingState::retired, std::memory_order_relaxed);

			co_await mapping->view->removeObserver(mapping);
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
					this,
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
				auto ageOutcome = co_await revokePages(_ops, mapping->address, mapping->length,
						mapping->tracksDirty(),
						[&] (VirtualAddr va, size_t size, RevokeBatch &batch) {
							return _ops->agePages(va, size, vacate, batch);
						});
				assert(ageOutcome);
				agingTurnover_.fetch_sub(ageOutcome.value().scanned, std::memory_order_relaxed);
				notifyRss_(ageOutcome.value());
				anyRevoked = ageOutcome.value().anyRevoked;
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

	if(flags & kMapNoDirtyTracking)
		mappingFlags |= MappingFlags::noDirtyTracking;

	mapping = allocate_rcu_shared<Mapping>(Allocator{},
		selfPtr.lock(),
		actualAddress,
		length,
		slice.lock(),
		slice->offset() + offset,
		static_cast<MappingFlags>(mappingFlags)
	);
	mapping->selfPtr = mapping;

	// We keep one reference until the detach the observer.
	// Attach before the mapping becomes faultable, otherwise evictions can miss its PTEs.
	mapping.policy().increment();
	mapping->view->addObserver(mapping.get());

	{
		auto irqLock = frg::guard(&irqMutex());
		auto snapshotLock = frg::guard(&_snapshotMutex);

		// Install the new mapping object.
		_mappings.insert(mapping.get());

		assert(mapping->state.load(std::memory_order_relaxed) == MappingState::null);
		mapping->state.store(MappingState::active, std::memory_order_relaxed);
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

			// PROT_NONE mappings have no accessible pages to populate.
			if(pageFlags) {
				LocalRcuEngine::Guard revokeGuard{mapping->revokeRcu};

				PageMagazine tables;
				auto mapOutcome = co_await revokePages(_ops, mapping->address, mapping->length,
						mapping->tracksDirty(),
						[&] (VirtualAddr va, size_t size, RevokeBatch &batch) {
							return _ops->mapPresentPages(va, mapping->view.get(),
									mapping->viewOffset + (va - mapping->address), size,
									pageFlags, caching, batch, tables);
						});
				assert(mapOutcome);
				notifyRss_(mapOutcome.value());
			}
		}
	}

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

		co_await mapping->exposeRcu.barrier();

		bool anyRevoked;
		{
			LocalRcuEngine::Guard revokeGuard{mapping->revokeRcu};

			// A present page is always readable, so dropping all access requires unmapping.
			if(pageFlags) {
				auto restrictOutcome = co_await revokePages(_ops, mapping->address, mapping->length,
						mapping->tracksDirty(),
						[&] (VirtualAddr va, size_t size, RevokeBatch &batch) {
							return _ops->restrictPages(va, size, pageFlags, batch);
						});
				assert(restrictOutcome);
				anyRevoked = restrictOutcome.value().anyRevoked;
			}else{
				auto unmapOutcome = co_await revokePages(_ops, mapping->address, mapping->length,
						mapping->tracksDirty(),
						[&] (VirtualAddr va, size_t size, RevokeBatch &batch) {
							return _ops->unmapPages(va, size, batch);
						});
				assert(unmapOutcome);
				notifyRss_(unmapOutcome.value());
				anyRevoked = unmapOutcome.value().anyRevoked;
			}
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

			auto cleanOutcome = co_await revokePages(_ops,
					mapping->address + mappingOffset, mappingChunk, mapping->tracksDirty(),
					[&] (VirtualAddr va, size_t size, RevokeBatch &batch) {
						return _ops->cleanPages(va, size, batch);
					});
			assert(cleanOutcome);
			anyRevoked = cleanOutcome.value().anyRevoked;
		}
		if(!anyRevoked)
			co_await mapping->revokeRcu.barrier();

		overallProgress += mappingChunk;
	}

	co_return {};
}

coroutine<frg::expected<Error>>
VirtualSpace::handleFault(VirtualAddr address, uint32_t faultFlags) {
	assert(currentIpl() == ipl::exceptionalWork);

	PageMagazine tables;
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
		// A mapping without any permission (PROT_NONE) faults on every access.
		if(!(flags & MappingFlags::permissionMask))
			co_return Error::badPermissions;

		// TODO: Aligning should not be necessary here.
		auto offset = (address - mapping->address) & ~(kPageSize - 1);

		FetchFlags fetchFlags = 0;
		if(flags & MappingFlags::dontRequireBacking)
			fetchFlags |= fetchDisallowBacking;
		if(faultFlags & VirtualSpace::kFaultWrite)
			fetchFlags |= fetchRequireMutable;
		if(faultFlags & VirtualSpace::kFaultSynchronous)
			fetchFlags |= fetchSynchronous;

		// Calling touchRange() on stale mappings is allowed,
		// so we do not enter a critical section here.
		FRG_CO_TRY(co_await mapping->view->touchRange(
				mapping->viewOffset + offset, kPageSize, fetchFlags));

		auto caching = CachingMode::null;
		if(mapping->slice->getCachingFlags() == cacheWriteCombine)
			caching = CachingMode::writeCombine;

		// Try to fault in the page.
		bool needTables = false;
		{
			LocalRcuEngine::Guard exposeGuard{mapping->exposeRcu};

			if (mapping->state.load(std::memory_order_relaxed) != MappingState::active)
				continue;
			auto flags = mapping->flags.load(std::memory_order_relaxed);
			// Re-check under the RCU guard: a concurrent protect() may have dropped all
			// access and unmapped the range, so the page must not be made present again.
			if(!(flags & MappingFlags::permissionMask))
				co_return Error::badPermissions;

			{
				LocalRcuEngine::Guard revokeGuard{mapping->revokeRcu};

				// A fault replaces at most one page table entry.
				auto remapOutcome = co_await revokePages<1>(_ops, address & ~(kPageSize - 1),
						kPageSize, mapping->tracksDirty(),
						[&] (VirtualAddr va, size_t, RevokeBatch &batch) {
							return _ops->faultPage(
								va,
								mapping->view.get(),
								mapping->viewOffset + offset,
								fetchFlags,
								compilePageFlags(flags),
								caching,
								batch,
								tables
							);
						});
				if(!remapOutcome) {
					if(remapOutcome.error() == Error::spuriousOperation) {
						// Spurious page faults are the result of race conditions.
						// They should be rare. If they happen too often, something is probably wrong!
						warningLogger() << "thor: Spurious page fault" << frg::endlog;
					}else if(remapOutcome.error() == Error::noMemory) {
						needTables = true;
					}else{
						assert(remapOutcome.error() == Error::fault);
						warningLogger() << "thor: Page still not available after touchRange()"
							<< frg::endlog;
						continue;
					}
				} else {
					notifyRss_(remapOutcome.value());
				}
			}
			if(!needTables)
				co_return {};
		}

		// Allocate the page tables outside of the RCU critical sections
		// since evictions wait for these critical sections to end.
		assert(tables.wanted);
		if(auto refilled = tables.refill(); !refilled)
			co_return refilled.error();
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

		// helPointerPhysical() blocks the calling thread.
		FRG_CO_TRY(co_await mapping->view->touchRange(
				mapping->viewOffset + offset, kPageSize, fetchFlags | fetchSynchronous));

		LocalRcuEngine::Guard exposeGuard{mapping->exposeRcu};
		auto physicalRange = mapping->view->peekRange(mapping->viewOffset + offset, fetchFlags);
		if(physicalRange.physical == PhysicalAddr(-1)) {
			warningLogger() << "thor: Page still not available after touchRange()" << frg::endlog;
			continue;
		}

		co_return physicalRange.physical;
	}
}

// Callers must hold _snapshotMutex, or _consistencyMutex (shared or exclusive).
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

// Callers must hold _snapshotMutex, or _consistencyMutex (shared or exclusive).
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

// Callers must hold _consistencyMutex exclusively.
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

// Callers must hold _consistencyMutex exclusively.
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

// Callers must hold _consistencyMutex exclusively.
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

// Callers must hold _consistencyMutex exclusively.
coroutine<frg::tuple<Mapping *, Mapping *>> VirtualSpace::_splitMappings(uintptr_t address, size_t size) {
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
				leftMapping = allocate_rcu_shared<Mapping>(
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
				rightMapping = allocate_rcu_shared<Mapping>(
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

			// We keep one reference until the detach the observer.
			// Attach before the mappings become faultable, otherwise evictions can miss their PTEs.
			leftMapping.policy().increment();
			leftMapping->view->addObserver(leftMapping.get());
			rightMapping.policy().increment();
			rightMapping->view->addObserver(rightMapping.get());

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
			assert(mapping->state.load(std::memory_order_relaxed) == MappingState::zombie);
			mapping->state.store(MappingState::retired, std::memory_order_relaxed);

			// Faults that saw the old mapping as active must finish mapping their pages
			// before it stops observing evictions; the new mappings do not wait for them.
			co_await mapping->exposeRcu.barrier();

			co_await mapping->view->removeObserver(mapping.get());
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

// Callers must hold _consistencyMutex exclusively.
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
				auto unmapOutcome = co_await revokePages(_ops, mapping->address, mapping->length,
						mapping->tracksDirty(),
						[&] (VirtualAddr va, size_t size, RevokeBatch &batch) {
							return _ops->unmapPages(va, size, batch);
						});
				assert(unmapOutcome);
				notifyRss_(unmapOutcome.value());
				anyRevoked = unmapOutcome.value().anyRevoked;
			}
			if(!anyRevoked)
				co_await mapping->revokeRcu.barrier();

			{
				auto irqLock = frg::guard(&irqMutex());
				auto snapshotLock = frg::guard(&_snapshotMutex);

				_mappings.remove(mapping.get());
			}

			assert(mapping->state.load(std::memory_order_relaxed) == MappingState::zombie);
			mapping->state.store(MappingState::retired, std::memory_order_relaxed);

			co_await mapping->view->removeObserver(mapping.get());
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

coroutine<std::expected<size_t, Error>> VirtualSpace::readPartialSpace(uintptr_t address,
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
		if(!mapping) {
			if(!progress)
				co_return std::unexpected{Error::fault};
			co_return progress;
		}

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
		if(!copyOutcome) {
			if(!progress)
				co_return std::unexpected{copyOutcome.error()};
			co_return progress;
		}

		progress += limitInMapping;
	}
	co_return progress;
}

coroutine<std::expected<size_t, Error>> VirtualSpace::writePartialSpace(uintptr_t address,
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
		if(!mapping) {
			if(!progress)
				co_return std::unexpected{Error::fault};
			co_return progress;
		}

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
		if(!copyOutcome) {
			if(!progress)
				co_return std::unexpected{copyOutcome.error()};
			co_return progress;
		}

		progress += limitInMapping;
	}
	co_return progress;
}

coroutine<std::expected<void, Error>> VirtualSpace::readSpace(uintptr_t address,
		void *buffer, size_t size) {
	size_t progress = 0;
	while(progress < size) {
		auto outcome = co_await readPartialSpace(address + progress,
				reinterpret_cast<std::byte *>(buffer) + progress, size - progress);
		if(!outcome)
			co_return std::unexpected{outcome.error()};
		progress += *outcome;
	}
	co_return {};
}

coroutine<std::expected<void, Error>> VirtualSpace::writeSpace(uintptr_t address,
		const void *buffer, size_t size) {
	size_t progress = 0;
	while(progress < size) {
		auto outcome = co_await writePartialSpace(address + progress,
				reinterpret_cast<const std::byte *>(buffer) + progress, size - progress);
		if(!outcome)
			co_return std::unexpected{outcome.error()};
		progress += *outcome;
	}
	co_return {};
}

// --------------------------------------------------------
// AddressSpace
// --------------------------------------------------------

void AddressSpace::activate(smarter::shared_ptr<AddressSpace, BindableHandle> space) {
	auto pageSpace = &space->pageSpace_;
	PageSpace::activate(smarter::shared_ptr<PageSpace>{space->selfPtr.lock(), pageSpace});
}

AddressSpace::AddressSpace(CtorToken)
: VirtualSpace{&ops_}, ops_{this} { }

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

void NamedMemoryViewLock::finalizeBeforeRcu() {
	_handle = MemoryViewLockHandle{};
}

} // namespace thor
