#pragma once

#include <atomic>

#include <async/basic.hpp>
#include <async/mutex.hpp>
#include <async/oneshot-event.hpp>
#include <async/recurring-event.hpp>
#include <frg/container_of.hpp>
#include <frg/expected.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/hierarchy.hpp>
#include <thor-internal/memory-view.hpp>
#include <thor-internal/mm-rc.hpp>
#include <thor-internal/rcu.hpp>

namespace thor {

struct GlobalFutex {
	GlobalFutex(FutexIdentity id, PhysicalAddr physical)
	: id_{id}, physical_{physical} {
		assert(!(physical & (sizeof(int) - 1)));
	}

	FutexIdentity getIdentity() {
		return id_;
	}

	unsigned int read() {
		PageAccessor accessor{physical_ & ~(kPageSize - 1)};
		auto offset = physical_ & (kPageSize - 1);
		auto accessPtr = reinterpret_cast<unsigned int *>(
				reinterpret_cast<std::byte *>(accessor.get()) + offset);
		return __atomic_load_n(accessPtr, __ATOMIC_RELAXED);
	}

private:
	FutexIdentity id_;
	PhysicalAddr physical_;
};

struct VirtualSpace;

struct PagesAffected {
	ptrdiff_t rssIncrease{0};
	ptrdiff_t rssDecrease{0};
	// Number of bytes that were scanned by agePages().
	ptrdiff_t scanned{0};
	// Whether any page had its access rights revoked.
	// This covers both pages that have their permission restricted and pages that are unmapped.
	bool anyRevoked{false};
};

inline CachingMode determineCachingMode(CachingMode physicalRangeCaching,
		CachingMode requested) {
	// check if an override caching mode was requested
	if(requested != CachingMode::null) {
		// allow overriding UC with WC
		if(requested == CachingMode::writeCombine && physicalRangeCaching == CachingMode::uncached) {
			return requested;
		}
	}

	// otherwise, return the caching mode from the physical range
	return physicalRangeCaching;
}

template<typename Cursor, typename PageSpace>
frg::expected<Error, PagesAffected> mapPresentPagesByCursor(PageSpace *ps, VirtualAddr va,
		MemoryView *view, uintptr_t offset, size_t size, PageFlags flags, CachingMode mode) {
	assert(!(va & (kPageSize - 1)));
	assert(!(offset & (kPageSize - 1)));
	assert(!(size & (kPageSize - 1)));

	PagesAffected affected{};
	Cursor c{ps, va};
	while(c.virtualAddress() < va + size) {
		auto progress = c.virtualAddress() - va;
		auto physicalRange = view->peekRange(offset + progress, fetchNone);
		if(physicalRange.physical == PhysicalAddr(-1)) {
			c.advance4k();
			continue;
		}
		assert(!(physicalRange.physical & (kPageSize - 1)));

		auto effectiveFlags = flags;
		if (!physicalRange.isMutable)
			effectiveFlags &= ~page_access::write;
		if(auto descriptor = globalPfnDb().find(physicalRange.physical))
			incrementUses(*descriptor);
		auto [status, oldPhysical] = c.map4k(physicalRange.physical, effectiveFlags,
			determineCachingMode(physicalRange.cachingMode, mode));
		if((status & page_status::present) && (status & page_status::dirty)) {
			if(auto descriptor = globalPfnDb().find(oldPhysical))
				markDirty(*descriptor);
		}
		affected.rssIncrease += kPageSize;
		if(status & page_status::present) {
			affected.rssDecrease += kPageSize;
			if(auto descriptor = globalPfnDb().find(oldPhysical))
				decrementUses(*descriptor);
		}
		c.advance4k();
	}
	return affected;
}

template<typename Cursor, typename PageSpace>
frg::expected<Error, PagesAffected> restrictPagesByCursor(PageSpace *ps, VirtualAddr va,
		size_t size, PageFlags flags, CachingMode mode) {
	assert(!(va & (kPageSize - 1)));
	assert(!(size & (kPageSize - 1)));

	PagesAffected affected{};
	Cursor c{ps, va};
	while(c.virtualAddress() < va + size) {
		auto [status, physical, restricted] = c.restrict4k(flags, mode);
		if((status & page_status::present) && (status & page_status::dirty)) {
			if(auto descriptor = globalPfnDb().find(physical))
				markDirty(*descriptor);
		}
		if(restricted)
			affected.anyRevoked = true;
		c.advance4k();
	}
	return affected;
}

template<typename Cursor, typename PageSpace>
frg::expected<Error, PagesAffected> faultPageByCursor(PageSpace *ps, VirtualAddr va,
		MemoryView *view, uintptr_t offset, FetchFlags fetchFlags, PageFlags flags, CachingMode mode) {
	assert(!(va & (kPageSize - 1)));
	assert(!(offset & (kPageSize - 1)));

	PagesAffected affected{};
	Cursor c{ps, va};

	auto physicalRange = view->peekRange(offset, fetchFlags);
	if(physicalRange.physical == PhysicalAddr(-1))
		return Error::fault;

	auto effectiveFlags = flags;
	if (!physicalRange.isMutable)
		effectiveFlags &= ~page_access::write;
	if(auto descriptor = globalPfnDb().find(physicalRange.physical))
		incrementUses(*descriptor);
	auto [status, oldPhysical] = c.remap4k(physicalRange.physical, effectiveFlags,
		determineCachingMode(physicalRange.cachingMode, mode));
	if(status & page_status::present) {
		if(status & page_status::dirty) {
			if(auto descriptor = globalPfnDb().find(oldPhysical))
				markDirty(*descriptor);
		}
		if(auto descriptor = globalPfnDb().find(oldPhysical))
			decrementUses(*descriptor);
		affected.rssDecrease += kPageSize;
	}
	affected.rssIncrease += kPageSize;

	return affected;
}

template<typename Cursor, typename PageSpace>
frg::expected<Error, PagesAffected> cleanPagesByCursor(PageSpace *ps, VirtualAddr va, size_t size) {
	assert(!(va & (kPageSize - 1)));
	assert(!(size & (kPageSize - 1)));

	PagesAffected affected{};
	Cursor c{ps, va};
	while(c.findDirty(va + size)) {
		auto [status, physical] = c.clean4k();
		assert(status & page_status::present);
		assert(status & page_status::dirty);
		if(auto descriptor = globalPfnDb().find(physical))
			markDirty(*descriptor);
		affected.anyRevoked = true;
		c.advance4k();
	}
	return affected;
}

template<typename Cursor, typename PageSpace>
frg::expected<Error, PagesAffected> unmapPagesByCursor(PageSpace *ps, VirtualAddr va, size_t size) {
	assert(!(va & (kPageSize - 1)));
	assert(!(size & (kPageSize - 1)));

	PagesAffected affected{};
	Cursor c{ps, va};
	while(c.findPresent(va + size)) {
		auto [status, physical] = c.unmap4k();
		assert(status & page_status::present);
		if(status & page_status::dirty) {
			if(auto descriptor = globalPfnDb().find(physical))
				markDirty(*descriptor);
		}
		if(auto descriptor = globalPfnDb().find(physical))
			decrementUses(*descriptor);
		affected.rssDecrease += kPageSize;
		affected.anyRevoked = true;

		c.advance4k();
	}
	return affected;
}

template<typename Cursor, typename PageSpace>
frg::expected<Error, PagesAffected> agePagesByCursor(PageSpace *ps, VirtualAddr va, size_t size, bool vacate) {
	assert(!(va & (kPageSize - 1)));
	assert(!(size & (kPageSize - 1)));

	PagesAffected affected{};
	Cursor c{ps, va};
	while(c.findPresent(va + size)) {
		affected.scanned += kPageSize;
		auto [status, physical, unmapped] = c.age4k(vacate);
		if(unmapped) {
			if(status & page_status::dirty) {
				if(auto descriptor = globalPfnDb().find(physical))
					markDirty(*descriptor);
			}
			if(auto descriptor = globalPfnDb().find(physical))
				decrementUses(*descriptor);
			affected.rssDecrease += kPageSize;
			affected.anyRevoked = true;
		}
		c.advance4k();
	}
	return affected;
}

struct VirtualOperations {
	virtual void retire(RetireNode *node) = 0;

	virtual bool submitShootdown(ShootNode *node) = 0;

	virtual frg::expected<Error, PagesAffected> mapPresentPages(VirtualAddr va, MemoryView *view,
			uintptr_t offset, size_t size, PageFlags flags, CachingMode mode) = 0;

	virtual frg::expected<Error, PagesAffected> restrictPages(VirtualAddr va,
			size_t size, PageFlags flags, CachingMode mode) = 0;

	virtual frg::expected<Error, PagesAffected> faultPage(VirtualAddr va, MemoryView *view,
			uintptr_t offset, FetchFlags fetchFlags, PageFlags flags, CachingMode mode) = 0;

	virtual frg::expected<Error, PagesAffected> cleanPages(VirtualAddr va, size_t size) = 0;

	virtual frg::expected<Error, PagesAffected> unmapPages(VirtualAddr va, size_t size) = 0;

	virtual frg::expected<Error, PagesAffected> agePages(VirtualAddr va, size_t size, bool vacate) = 0;

	// ----------------------------------------------------------------------------------
	// Sender boilerplate for retire()
	// ----------------------------------------------------------------------------------

	template<typename R>
	struct RetireOperation final : private RetireNode {
		RetireOperation(VirtualOperations *self, R receiver)
		: self_{self}, receiver_{std::move(receiver)} { }

		void start() {
			auto wq = workQueueFromEnv(async::execution::get_env(receiver_));
			RetireNode::wq_ = wq;
			Worklet::setup([] (Worklet *base) {
				auto op = static_cast<RetireOperation *>(base);
				async::execution::set_value(op->receiver_);
			});
			self_->retire(this);
		}

	private:
		VirtualOperations *self_;
		R receiver_;
	};

	struct RetireSender {
		using value_type = void;

		template<typename R>
		RetireOperation<R> connect(R receiver) {
			return {self, std::move(receiver)};
		}

		async::sender_awaiter<RetireSender> operator co_await() {
			return {*this};
		}

		VirtualOperations *self;
	};

	RetireSender retire() {
		return {this};
	}

	// ----------------------------------------------------------------------------------
	// Sender boilerplate for shootdown()
	// ----------------------------------------------------------------------------------

	template<typename R>
	struct ShootdownOperation;

	struct [[nodiscard]] ShootdownSender {
		using value_type = void;

		template<typename R>
		friend ShootdownOperation<R>
		connect(ShootdownSender sender, R receiver) {
			return {sender, std::move(receiver)};
		}

		VirtualOperations *self;
		VirtualAddr address;
		size_t size;
	};

	ShootdownSender shootdown(VirtualAddr address, size_t size) {
		return {this, address, size};
	}

	template<typename R>
	struct ShootdownOperation {
		struct Node : ShootNode, RcuCallable {
			ShootdownOperation *op;
		};

		ShootdownOperation(ShootdownSender s, R receiver)
		: s_{s}, receiver_{std::move(receiver)} { }

		ShootdownOperation(const ShootdownOperation &) = delete;

		ShootdownOperation &operator= (const ShootdownOperation &) = delete;

		void start() {
			auto wq = workQueueFromEnv(async::execution::get_env(receiver_));
			auto node = frg::construct<Node>(*kernelAlloc);
			node->address = s_.address;
			node->size = s_.size;
			node->wq_ = wq;
			node->op = this;
			node->Worklet::setup([] (Worklet *base) {
				auto w = static_cast<Node *>(base);
				auto op = w->op;
				submitRcu(w, [] (RcuCallable *r) {
					frg::destruct(*kernelAlloc, static_cast<Node *>(r));
				});
				async::execution::set_value(op->receiver_);
			});
			if(s_.self->submitShootdown(node)) {
				frg::destruct(*kernelAlloc, node);
				return async::execution::set_value(receiver_);
			}
		}

	private:
		ShootdownSender s_;
		R receiver_;
	};

	friend async::sender_awaiter<ShootdownSender>
	operator co_await(ShootdownSender sender) {
		return {sender};
	}

protected:
	~VirtualOperations() = default;

	// ----------------------------------------------------------------------------------
};

struct Hole {
	Hole(VirtualAddr address, size_t length)
	: _address{address}, _length{length}, largestHole{0} { }

	VirtualAddr address() const {
		return _address;
	}

	size_t length() const {
		return _length;
	}

	frg::rbtree_hook treeNode;

private:
	VirtualAddr _address;
	size_t _length;

public:
	// Largest hole in the subtree of this node.
	size_t largestHole;
};

enum MappingFlags : uint32_t {
	null = 0,

	permissionMask = 0x70,
	protRead = 0x10,
	protWrite = 0x20,
	protExecute = 0x40,

	dontRequireBacking = 0x100
};

struct TouchVirtualResult {
	PhysicalRange range;
	bool spurious;
};

enum class MappingState {
	null,
	active,
	zombie,
	retired
};

struct Mapping {
	Mapping(
		smarter::shared_ptr<VirtualSpace> owner,
		VirtualAddr address,
		size_t length,
		smarter::shared_ptr<MemorySlice> view,
		uintptr_t offset,
		MappingFlags flags
	);

	Mapping(const Mapping &) = delete;

	~Mapping();

	Mapping &operator= (const Mapping &) = delete;

	void protect(MappingFlags flags);

	smarter::borrowed_ptr<Mapping> selfPtr;

	uint32_t compilePageFlags();

	coroutine<void> runEvictionLoop();

	const smarter::shared_ptr<VirtualSpace> owner;
	const VirtualAddr address;
	const size_t length;
	const smarter::shared_ptr<MemorySlice> slice;
	const smarter::shared_ptr<MemoryView> view;
	const size_t viewOffset;

	// Protected against writes by _consistencyMutex.
	// May be read without holding any mutex.
	std::atomic<MappingFlags> flags;
	// Protected against writes by _consistencyMutex.
	// May be read without holding any mutex.
	std::atomic<MappingState> state{MappingState::null};

	// Protected by _snapshotMutex.
	frg::rbtree_hook treeNode;

	// Protected against writes by _consistencyMutex.
	MemoryObserver observer;

	// Code paths MUST perform an exposeRcu barrier() after they cause page
	// permission to be narrowed (or pages to become invalid) but before this
	// change is actually committed.
	// In particular:
	// * Code that acknowledges an eviction. More precisely, code that calls done() on the handle
	//   returned by pollEviction() needs to do a barrier() before unmapping the pages
	//   via unmapPages() (which happens before done()).
	// * Code that reduces the permission bits of a mapping.
	//   This needs to call barrier() before restricting permissions in the page tables
	//   via restrictPages() or unmapPages().
	// * Code that moves a mapping out of MappingState::active.
	//   This needs to call barrier() before unmap
	//
	// This gurantees that exposeRcu critical sections can rely on pages returned from peekRange()
	// to remain valid with permissions determined by the mappings flags that are
	// read during the exposeRcu critical section.
	// The same applies for pages that are already mapped into page tables.
	LocalRcuEngine exposeRcu;

	// The following code paths MUST be protected by a revokeRcu critical section:
	// * Code that narrows page permissions (via VirtualOperations::restrictPages()).
	// * Code that unmaps pages entirely (via VirtualOperations::unmapPages()).
	// In both cases, the revokeRcu critical section MUST cover both the
	// page tables changes and the shootdown.
	//
	// This guarantees that a revokeRcu barrier() waits for all prior
	// permission revocation and associated shootdown to complete.
	LocalRcuEngine revokeRcu;

	async::cancellation_event cancelEviction;
	async::oneshot_event evictionDoneEvent;
};

struct HoleLess {
	bool operator() (const Hole &a, const Hole &b) {
		return a.address() < b.address();
	}
};

struct HoleAggregator;

using HoleTree = frg::rbtree<
	Hole,
	&Hole::treeNode,
	HoleLess,
	HoleAggregator
>;

struct HoleAggregator {
	static bool aggregate(Hole *node);
	static bool check_invariant(HoleTree &tree, Hole *node);
};

struct MappingLess {
	bool operator() (const Mapping &a, const Mapping &b) {
		return a.address < b.address;
	}
};

using MappingTree = frg::rbtree<
	Mapping,
	&Mapping::treeNode,
	MappingLess
>;

struct VirtualSpace {
	friend struct Mapping;

public:
	typedef uint32_t MapFlags;
	enum : MapFlags {
		kMapFixed = 0x01,
		kMapPreferBottom = 0x02,
		kMapPreferTop = 0x04,
		kMapProtRead = 0x08,
		kMapProtWrite = 0x10,
		kMapProtExecute = 0x20,
		kMapPopulate = 0x200,
		kMapDontRequireBacking = 0x400,
		kMapFixedNoReplace = 0x800
	};

	enum FaultFlags : uint32_t {
		kFaultWrite = (1 << 1),
		kFaultExecute = (1 << 2)
	};

	VirtualSpace(VirtualOperations *ops, smarter::shared_ptr<Hierarchy> hierarchy);

	~VirtualSpace();

	smarter::borrowed_ptr<Hierarchy> hierarchy() const {
		return hierarchy_;
	}

	void retire();

	void setupInitialHole(VirtualAddr address, size_t size);

	coroutine<frg::expected<Error, VirtualAddr>>
	map(smarter::borrowed_ptr<MemorySlice> view,
			VirtualAddr address, size_t offset, size_t length, uint32_t flags);

	coroutine<frg::expected<Error>>
	protect(VirtualAddr address, size_t length, uint32_t flags);

	coroutine<frg::expected<Error>>
	synchronize(VirtualAddr address, size_t length);

	coroutine<frg::expected<Error>>
	unmap(VirtualAddr address, size_t length);

	coroutine<frg::expected<Error>>
	handleFault(VirtualAddr address, uint32_t flags);

	coroutine<frg::expected<Error, PhysicalAddr>>
	retrievePhysical(VirtualAddr address);

	coroutine<void> runAgingLoop();

	size_t rss() {
		auto value = rss_.load(std::memory_order_relaxed);
		return (value > 0) ? value : 0;
	}

	// ----------------------------------------------------------------------------------
	// Read/write support.
	// ----------------------------------------------------------------------------------

	// These functions read as much data as possible;
	// on error, they read/write a partially filled buffer.
	coroutine<size_t> readPartialSpace(uintptr_t address, void *buffer, size_t size);
	coroutine<size_t> writePartialSpace(uintptr_t address, const void *buffer, size_t size);

	auto readSpace(uintptr_t address, void *buffer, size_t size) {
		return async::transform(
			readPartialSpace(address, buffer, size),
			[=] (size_t actualSize) -> frg::expected<Error> {
				if(actualSize != size)
					return Error::fault;
				return {};
			}
		);
	}

	auto writeSpace(uintptr_t address, const void *buffer, size_t size) {
		return async::transform(
			writePartialSpace(address, buffer, size),
			[=] (size_t actualSize) -> frg::expected<Error> {
				if(actualSize != size)
					return Error::fault;
				return {};
			}
		);
	}

	// ----------------------------------------------------------------------------------
	// GlobalFutex support.
	// ----------------------------------------------------------------------------------

	struct GlobalFutexSpace {
		template<typename F>
		coroutine<frg::expected<Error>> withFutex(uintptr_t address, F &&f) {
			assert(currentIpl() == ipl::exceptionalWork);

			if (address & (sizeof(int) - 1))
				co_return Error::illegalArgs;

			while (true) {
				smarter::shared_ptr<Mapping> mapping;
				{
					auto irqLock = frg::guard(&irqMutex());
					auto spaceGuard = frg::guard(&self->_snapshotMutex);
					mapping = self->_findMapping(address);
				}
				if(!mapping)
					co_return Error::fault;

				auto offset = address - mapping->address;
				auto alignedOffset = offset & ~(kPageSize - 1);
				auto offsetMisalign = offset & (kPageSize - 1);

				// TODO: We may want to resolve the page to a (owner MemoryView, offset) pair
				//       to handle futexes behind IndirectMemory.
				//       However, we do not have any futexes on IndirectMemory right now.
				FutexIdentity id{
					.spaceQualifier = reinterpret_cast<uintptr_t>(mapping->view.get()),
					.localAddress = mapping->viewOffset + offset,
				};

				// Lock exposeRcu to prevent page eviction.
				{
					LocalRcuEngine::Guard exposeGuard{mapping->exposeRcu};

					// Complete the operation if the memory page is available.
					auto physicalRange = mapping->view->peekRange(mapping->viewOffset + alignedOffset, fetchNone);
					if(physicalRange.physical != PhysicalAddr(-1)) {
						f(GlobalFutex{id, physicalRange.physical + offsetMisalign});
						co_return {};
					}
				}

				// Otherwise, try to make the page available.
				FRG_CO_TRY(co_await mapping->view->touchRange(
					mapping->viewOffset + alignedOffset, kPageSize, fetchNone
				));
			}
		}

		VirtualSpace *self;
	};
	static_assert(FutexSpace<GlobalFutexSpace>);

	GlobalFutexSpace globalFutexSpace() {
		return {this};
	}

	// ----------------------------------------------------------------------------------

	smarter::borrowed_ptr<VirtualSpace> selfPtr;

private:
	// Allocates a new mapping of the given length somewhere in the address space.
	frg::expected<Error, VirtualAddr> _allocate(size_t length, MapFlags flags);

	frg::expected<Error, VirtualAddr> _allocateAt(VirtualAddr address, size_t length);

	smarter::shared_ptr<Mapping> _findMapping(VirtualAddr address);

	bool _areMappingsInRange(VirtualAddr address, VirtualAddr length);

	// Splits some memory range from a hole mapping.
	void _splitHole(Hole *hole, VirtualAddr offset, VirtualAddr length);

	// Desired working set size of the process.
	// This is a soft limit, we will (asynchronously) unmap pages once we are above this limit.
	ptrdiff_t workingSetGoal_();

	// Updates rss_ and agingTurnover_.
	void notifyRss_(const PagesAffected &affected);

	// Returns true if the aging code should continue scanning accessed bits.
	bool shouldContinueAging_();

	// Potentially splits mappings into two parts at (address) and (address + size).
	// Returns the start and end mappings that are within the specified range.
	coroutine<frg::tuple<Mapping *, Mapping *>> _splitMappings(uintptr_t address, size_t size);

	// Used in conjunction with _splitMappings.
	// Unmaps and removes all mappings between start and end that fall within the specified range.
	// Returns whether shootdown needs to be performed (any of the mappings got unmapped).
	coroutine<void> _unmapMappings(VirtualAddr address, size_t length, Mapping *start, Mapping *end);

	VirtualOperations *_ops;

	// _consistencyMutex MUST be taken while:
	// * Mappings are added.
	// * Mappings are removed.
	// * The flags of mappings are modified.
	// In case of mapping removal and mapping flag change, the mutex must be held until the
	// page tables are changed, shootdown is complete (and the eviction loop is exited, if applicable).
	async::shared_mutex _consistencyMutex;

	// Protects _holes and _mappings.
	frg::ticket_spinlock _snapshotMutex;

	HoleTree _holes;
	MappingTree _mappings;

	std::atomic<ptrdiff_t> rss_;

	// Number of pages faulted minus number of pages scanned by aging.
	// This is used by shouldContinueAging_().
	std::atomic<ptrdiff_t> agingTurnover_{0};
	// Raise once shouldContinueAging_() becomes true.
	async::recurring_event agingEvent_;
	async::cancellation_event cancelAging_;
	async::oneshot_event agingDoneEvent_;

	smarter::shared_ptr<Hierarchy> hierarchy_;
};

struct AddressSpace final : VirtualSpace {
	friend struct Mapping;

	struct Operations final : VirtualOperations {
		Operations(AddressSpace *space)
		: space_{space} { }

		void retire(RetireNode *node) override {
			return space_->pageSpace_.retire(node);
		}

		bool submitShootdown(ShootNode *node) override {
			return space_->pageSpace_.submitShootdown(node);
		}

		frg::expected<Error, PagesAffected> mapPresentPages(VirtualAddr va, MemoryView *view,
				uintptr_t offset, size_t size, PageFlags flags, CachingMode mode) override {
			return mapPresentPagesByCursor<ClientPageSpace::Cursor>(&space_->pageSpace_,
					va, view, offset, size, flags, mode);
		}

		frg::expected<Error, PagesAffected> restrictPages(VirtualAddr va,
				size_t size, PageFlags flags, CachingMode mode) override {
			return restrictPagesByCursor<ClientPageSpace::Cursor>(&space_->pageSpace_,
					va, size, flags, mode);
		}

		frg::expected<Error, PagesAffected> faultPage(VirtualAddr va, MemoryView *view,
				uintptr_t offset, FetchFlags fetchFlags, PageFlags flags, CachingMode mode) override {
			return faultPageByCursor<ClientPageSpace::Cursor>(&space_->pageSpace_,
					va, view, offset, fetchFlags, flags, mode);
		}

		frg::expected<Error, PagesAffected> cleanPages(VirtualAddr va, size_t size) override {
			return cleanPagesByCursor<ClientPageSpace::Cursor>(&space_->pageSpace_,
					va, size);
		}

		frg::expected<Error, PagesAffected> unmapPages(VirtualAddr va, size_t size) override {
			return unmapPagesByCursor<ClientPageSpace::Cursor>(&space_->pageSpace_,
					va, size);
		}

		frg::expected<Error, PagesAffected> agePages(VirtualAddr va, size_t size, bool vacate) override {
			return agePagesByCursor<ClientPageSpace::Cursor>(&space_->pageSpace_,
					va, size, vacate);
		}

	private:
		AddressSpace *space_;
	};

public:
	static smarter::shared_ptr<AddressSpace, BindableHandle>
	constructHandle(smarter::shared_ptr<AddressSpace> ptr) {
		auto space = ptr.get();
		space->_bindableCtr.setup(smarter::adopt_rc, 1);
		ptr.release();
		return smarter::shared_ptr<AddressSpace, BindableHandle>{smarter::adopt_rc, space, BindableHandle{space}};
	}

	static smarter::shared_ptr<AddressSpace, BindableHandle> create(
			smarter::shared_ptr<Hierarchy> hierarchy) {
		// Note: technically, we do not rely on RCU here.
		//       However, the refcount may be decrement in IRQ context
		//       and RCU ensures that freeing happens on a work queue.
		auto ptr = allocate_rcu_shared<AddressSpace>(Allocator{}, std::move(hierarchy));
		ptr->selfPtr = ptr;
		ptr->setupInitialHole(0x1000, (UINT64_C(1) << getLowerHalfBits()) - 0x1000);
		spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(), ptr->runAgingLoop());
		return constructHandle(std::move(ptr));
	}

	static void activate(smarter::shared_ptr<AddressSpace, BindableHandle> space);

	AddressSpace(smarter::shared_ptr<Hierarchy> hierarchy);

	~AddressSpace();

	// Called when the BindableHandle refcount becomes zero.
	void dispose();

	FutexRealm localFutexRealm;

	bool updatePageAccess(VirtualAddr address, PageFlags flags) {
		return pageSpace_.updatePageAccess(address, flags);
	}

	smarter::counter _bindableCtr;

private:
	Operations ops_;
	ClientPageSpace pageSpace_;
};

struct MemoryViewLockHandle {
	friend void swap(MemoryViewLockHandle &a, MemoryViewLockHandle &b) {
		using std::swap;
		swap(a._view, b._view);
		swap(a._offset, b._offset);
		swap(a._size, b._size);
		swap(a._active, b._active);
	}

	MemoryViewLockHandle() = default;

	MemoryViewLockHandle(smarter::shared_ptr<MemoryView> view, uintptr_t offset, size_t size)
	: _view{view}, _offset{offset}, _size{size}, _active{false} { }

	MemoryViewLockHandle(const MemoryViewLockHandle &) = delete;

	MemoryViewLockHandle(MemoryViewLockHandle &&other)
	: MemoryViewLockHandle{} {
		swap(*this, other);
	}

	~MemoryViewLockHandle();

	MemoryViewLockHandle &operator= (MemoryViewLockHandle other) {
		swap(*this, other);
		return *this;
	}

	explicit operator bool () {
		return _active;
	}

	void acquire() {
		assert(!_active);

		auto err = _view->lockRange(_offset, _size);
		if(err != Error::success)
			return;
		_active = true;
	}

private:
	smarter::shared_ptr<MemoryView> _view = nullptr;
	uintptr_t _offset = 0;
	size_t _size = 0;
	bool _active = false;
};

struct NamedMemoryViewLock {
	NamedMemoryViewLock(MemoryViewLockHandle handle)
	: _handle{std::move(handle)} { }

	NamedMemoryViewLock(const NamedMemoryViewLock &) = delete;

	~NamedMemoryViewLock();

	NamedMemoryViewLock &operator= (const NamedMemoryViewLock &) = delete;

private:
	MemoryViewLockHandle _handle;
};

void initializeReclaim();

inline void BindableHandle::increment() const {
	_space->_bindableCtr.increment();
}

inline void BindableHandle::decrement() const {
	if(_space->_bindableCtr.decrement_and_check_if_zero()) {
		_space->dispose();
		_space->selfPtr.policy().decrement();
	}
}

} // namespace thor
