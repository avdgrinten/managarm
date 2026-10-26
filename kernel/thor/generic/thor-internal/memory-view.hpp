#pragma once

#include <atomic>
#include <cstddef>
#include <expected>
#include <span>

#include <async/algorithm.hpp>
#include <async/oneshot-event.hpp>
#include <async/recurring-event.hpp>
#include <async/wait-group.hpp>
#include <frg/list.hpp>
#include <frg/rbtree.hpp>
#include <frg/rcu_radixtree.hpp>
#include <frg/shared_ptr.hpp>
#include <frg/vector.hpp>
#include <frg/expected.hpp>
#include <frg/functional.hpp>
#include <physical-buddy.hpp>
#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/futex.hpp>
#include <thor-internal/hierarchy.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/types.hpp>
#include <thor-internal/pfn-db.hpp>
#include <thor-internal/rcu.hpp>
#include <thor-internal/rcu-base.hpp>

namespace thor {

enum class ManageRequest {
	null,
	initialize,
	writeback
};

struct Mapping;
struct AddressSpace;
struct AddressSpaceLockHandle;
struct FaultNode;
struct MemoryReclaimer;

struct CacheBundle;

struct CachePage {
	// Page is registered with the reclaim mechanism.
	static constexpr uint32_t reclaimRegistered = 0x01;
	// Page is on the bundle's reclaim list and the bundle did not take it yet (not in an LRU list).
	static constexpr uint32_t reclaimPosted = 0x02;

	// CacheBundle that owns this page.
	CacheBundle *bundle = nullptr;

	// Identity of the page as part of the bundle.
	// Bundles can use this field however they like.
	uint64_t identity = 0;

	// Hooks for LRU lists.
	frg::default_list_hook<CachePage> listHook;

	uint32_t flags = 0;
	uint8_t generation = 0;

	// Counts the references that may access the page without a lock (i.e., PTE and RevokeBatch entries).
	// A PTE's use count may only be dropped after its shootdown completed.
	// The bundle may free the frame of a page without uses or locks once an rcuBarrier() has elapsed.
	// Lookups and use counts taken in the meantime avert the reclamation.
	// Hence, code that obtains the frame without already owning a use count or a lock
	// must take a use count within the same RCU critical section in which it obtained the page's address
	// (e.g., via peekRange() or by looking up a PTE).
	std::atomic<unsigned int> useCount = 0;
};

using CachePagesList = frg::intrusive_list<
	CachePage,
	frg::locate_member<
		CachePage,
		frg::default_list_hook<CachePage>,
		&CachePage::listHook
	>
>;

// This is the "backend" part of a memory object.
struct CacheBundle {
	friend struct MemoryReclaimer;

	// Number of generations for the LRU mechanism.
	static constexpr unsigned int numGenerations = 8;

	// Slow paths of the free incrementUses()/decrementUses() functions for the 0 <-> 1 transitions.
	// A 0 -> 1 transition during the grace period of a reclamation must avert it; see CachePage::useCount.
	virtual void incrementUses(CachePage *page) = 0;
	virtual void decrementUses(CachePage *page) = 0;

	// Marks a page as dirty. This may cause writeback to start immediately.
	// Note that callers who have the page mapped must perform shootdown before calling markDirty()
	// such that later stores re-dirty the PTE.
	virtual void markDirty(CachePage *page) = 0;

private:
	frg::ticket_spinlock reclaimMutex_;

	// One list of pages per generation.
	// Protected by reclaimMutex_.
	frg::intrusive_list<
		CachePage,
		frg::locate_member<
			CachePage,
			frg::default_list_hook<CachePage>,
			&CachePage::listHook
		>
	> genLists_[numGenerations];

	// Number of pages in each list of genLists_.
	// Protected by reclaimMutex_.
	size_t genSizes_[numGenerations] = {};

	// Points to the newest generation.
	// This is cyclically incremented when MemoryReclaimer starts a new generation.
	// Protected by reclaimMutex_.
	unsigned int newestGen_ = 0;

	// Protected by reclaimMutex_.
	frg::intrusive_list<
		CachePage,
		frg::locate_member<
			CachePage,
			frg::default_list_hook<CachePage>,
			&CachePage::listHook
		>
	> _reclaimList;

	async::recurring_event _reclaimEvent;

	// List hook used by MemoryReclaimer.
	frg::intrusive_rcu_list_hook<CacheBundle> reclaimerHook_;

	// Can be used to pin the bundle under RCU. Set by MemoryReclaimer::registerBundle().
	smarter::weak_ptr<CacheBundle> selfPtr_;
};

inline void markDirty(PfnDescriptor descriptor) {
	if(descriptor.isCachePage()) {
		auto *ptr = descriptor.cachePagePtr();
		ptr->bundle->markDirty(ptr);
	}
}

// Unless the caller already owns a use count of the page, this must be called in the
// RCU critical section that found the page; see CachePage::useCount.
inline void incrementUses(PfnDescriptor descriptor) {
	if(!descriptor.isCachePage())
		return;
	auto *ptr = descriptor.cachePagePtr();
	auto cnt = ptr->useCount.load(std::memory_order_relaxed);
	while(cnt > 0) {
		if(ptr->useCount.compare_exchange_weak(cnt, cnt + 1,
				std::memory_order_acquire, std::memory_order_relaxed))
			return;
	}
	ptr->bundle->incrementUses(ptr);
}

inline void decrementUses(PfnDescriptor descriptor) {
	if(!descriptor.isCachePage())
		return;
	auto *ptr = descriptor.cachePagePtr();
	auto cnt = ptr->useCount.load(std::memory_order_relaxed);
	while(cnt > 1) {
		if(ptr->useCount.compare_exchange_weak(cnt, cnt - 1,
				std::memory_order_release, std::memory_order_relaxed))
			return;
	}
	ptr->bundle->decrementUses(ptr);
}

struct PhysicalRange {
	PhysicalAddr physical{~PhysicalAddr{0}};
	size_t size{0};
	CachingMode cachingMode{CachingMode::null};
	// Whether pages returned by peekRange() are mutable or not.
	// If fetchRequireMutable is set, this must be set to true.
	// If fetchRequireMutable is clear, this may either be true or false.
	bool isMutable{false};
};

struct ManageNode {
	Error error() { return _error; }
	ManageRequest type() { return _type; }
	uintptr_t offset() { return _offset; }
	size_t size() { return _size; }

	void setup(Error error, ManageRequest type, uintptr_t offset, size_t size) {
		_error = error;
		_type = type;
		_offset = offset;
		_size = size;
	}

	frg::default_list_hook<ManageNode> processQueueItem;
	async::oneshot_primitive completionEvent;

private:
	// Results of the operation.
	Error _error;
	ManageRequest _type;
	uintptr_t _offset;
	size_t _size;
};

using ManageList = frg::intrusive_list<
	ManageNode,
	frg::locate_member<
		ManageNode,
		frg::default_list_hook<ManageNode>,
		&ManageNode::processQueueItem
	>
>;

struct MemoryNotification {
	ManageRequest type;
	uintptr_t offset;
	size_t size;
};


using FetchFlags = uint32_t;
inline constexpr FetchFlags fetchNone = 0;
// Require mutable pages to be returned.
// * For peekRange(): on success, the result must have PhysicalRange::isMutable set.
//   If this is not possible, this flag lets peekRange() fail instead.
// * For touchRange(): forces the MemoryView to make mutable pages available from future peekRange() calls.
inline constexpr FetchFlags fetchRequireMutable = 1;
// Fail touchRange() calls that need to block on unavailable pages.
// This is useful for userspace page caches that want to detect deadlocks.
inline constexpr FetchFlags fetchDisallowBacking = 2;
// The caller's thread is blocked until the call completes (e.g., for page faults).
// This makes corresponding page allocations await at the current context's allocation policy
// instead of using the memory object's allocation policy.
inline constexpr FetchFlags fetchSynchronous = 4;

using CachingFlags = uint32_t;
inline constexpr CachingFlags cacheWriteCombine = 1;

// Charges a single page to hierarchy, then allocates a fungible frame.
// The frame is allocated at the floor of hierarchy, or at the current context's floor
// if flags contains fetchSynchronous.
coroutine<std::expected<PhysicalAddr, Error>>
chargeAndAllocate(Hierarchy *hierarchy, PhysicalKind kind, FetchFlags flags);

// Like chargeAndAllocate() but fails instead of waiting.
// In contrast to chargeAndAllocate(), this can handle non-fungible allocations.
std::expected<PhysicalAddr, Error>
tryChargeAndAllocate(Hierarchy *hierarchy, size_t size, PhysicalKind kind, int addressBits,
		FetchFlags flags);

void deallocateAndUncharge(Hierarchy *hierarchy, PhysicalAddr physical, size_t size,
		PhysicalKind kind);

// Returned by the callback of MemoryView::accessRange().
struct PageAccessResult {
	// Whether the callback modified the range. Requires fetchRequireMutable.
	bool dirty = false;
};

// Non-owning reference to the callback of MemoryView::accessRange().
using PageAccessFn = frg::function_ref<PageAccessResult(void *, size_t)>;

enum class EvictMode {
	none,
	// Evicts all pages in a range.
	breakRange,
	// Collects PTE dirty bits in all mappings of a range and completes their shootdowns.
	cleanRange,
};

// Observes the evictions of a MemoryView (e.g., a Mapping of the view).
// Observers must only be freed after an RCU grace period has passed since removeObserver()
// (e.g., by allocating them with allocate_rcu_shared()) since they are traversed under RCU.
// An observer is attached to at most one view during its lifetime.
struct MemoryObserver {
	friend struct EvictionQueue;

	// The observer is only called for evictions that intersect [offset, offset + size).
	MemoryObserver(uintptr_t observedOffset, size_t observedSize)
	: observedOffset_{observedOffset}, observedSize_{observedSize} { }

	MemoryObserver(const MemoryObserver &) = delete;

	MemoryObserver &operator= (const MemoryObserver &) = delete;

	// Runs at ipl::exceptionalWork on the work queue of the code that initiates the eviction.
	// Different evictions can call this concurrently.
	virtual coroutine<void> evict(EvictMode mode, uintptr_t offset, size_t size) = 0;

protected:
	~MemoryObserver() = default;

private:
	static constexpr unsigned int detachedBit = 1u << 31;

	bool tryBeginEviction_() {
		auto n = inflightEvictions_.load(std::memory_order_relaxed);
		do {
			if(n & detachedBit)
				return false;
		} while(!inflightEvictions_.compare_exchange_weak(n, n + 1,
				std::memory_order_acquire, std::memory_order_relaxed));
		return true;
	}

	void endEviction_() {
		auto n = inflightEvictions_.fetch_sub(1, std::memory_order_acq_rel);
		assert(n & ~detachedBit);
		if(n == (detachedBit | 1))
			evictionsDrainedEvent_.raise();
	}

	const uintptr_t observedOffset_;
	const size_t observedSize_;

	// Number of in-flight evict() calls, plus detachedBit once removeObserver() started.
	std::atomic<unsigned int> inflightEvictions_{0};
	async::oneshot_event evictionsDrainedEvent_;

	// Protected against writes by EvictionQueue::mutex_.
	// Unlinked only while no eviction is in flight.
	frg::intrusive_rcu_list_hook<MemoryObserver> hook_;
};

struct EvictionQueue final : frg::intrusive_rc {
	void addObserver(MemoryObserver *observer) {
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		// Note that this push_back() publishes the observer since traversal does not hold mutex_.
		observers_.push_back(observer);
	}

	// Once this completes, no evict() call on the observer is running or will be started.
	coroutine<void> removeObserver(MemoryObserver *observer);

	coroutine<void> breakRange(uintptr_t offset, size_t size) {
		return dispatch_(EvictMode::breakRange, offset, size);
	}
	coroutine<void> cleanRange(uintptr_t offset, size_t size) {
		return dispatch_(EvictMode::cleanRange, offset, size);
	}

	// Whether an observer intersects the range, i.e., whether breakRange() would call evict().
	// Like evictions, this ignores observers that are being removed.
	bool isObserved(uintptr_t offset, size_t size);

private:
	// Calls evict() on all observers concurrently.
	coroutine<void> dispatch_(EvictMode mode, uintptr_t offset, size_t size);

	frg::ticket_spinlock mutex_;

	// Protected against writes by mutex_.
	frg::intrusive_rcu_list<
		MemoryObserver,
		frg::locate_member<
			MemoryObserver,
			frg::intrusive_rcu_list_hook<MemoryObserver>,
			&MemoryObserver::hook_
		>
	> observers_;
};

// Selects how the discard path treats dirty page contents.
enum class DiscardMode : uint8_t {
	// The page is not being discarded.
	none,
	// Dirty contents are dropped without writeback (truncation semantics).
	dropDirty,
	// Dirty contents are still written back before the entry is erased
	// (invalidation semantics).
	keepDirty,
};

// View on some pages of memory. This is the "frontend" part of a memory object.
struct MemoryView : RcuProtected {
protected:
	MemoryView(frg::intrusive_shared_ptr<EvictionQueue, Allocator> evictionQueue = {})
	: evictionQueue_{std::move(evictionQueue)} { }

	~MemoryView() = default;

	EvictionQueue *evictionQueue() {
		return evictionQueue_.get();
	}

public:
	// Add/remove memory observers. These will be notified of page evictions.
	void addObserver(MemoryObserver *observer) {
		if(evictionQueue_)
			evictionQueue_->addObserver(observer);
	}

	coroutine<void> removeObserver(MemoryObserver *observer) {
		if(evictionQueue_)
			co_await evictionQueue_->removeObserver(observer);
	}

	// Returns the current size of the memory object.
	// At time of mapping, a new mapping must contained within the memory object's size.
	// However, the memory object's size may change at any time (even when mappings exist).
	virtual size_t getLength() = 0;

	virtual coroutine<frg::expected<Error>> resize(size_t newLength);

	virtual coroutine<frg::expected<Error, smarter::shared_ptr<MemoryView>>> fork(
		smarter::shared_ptr<Hierarchy> hierarchy
	);

	virtual coroutine<frg::expected<Error>> copyTo(uintptr_t offset,
			const void *pointer, size_t size,
			FetchFlags flags = 0);

	virtual coroutine<frg::expected<Error>> copyFrom(uintptr_t offset,
			void *pointer, size_t size,
			FetchFlags flags = 0);

	// Acquire/release a lock on a memory range.
	// While a lock is active, results of peekRange() stay consistent.
	// Locks do *not* force all pages to be available, but once a page is available
	// (e.g. due to touchRange()), it cannot be evicted until the lock is released.
	virtual Error lockRange(uintptr_t offset, size_t size) = 0;
	virtual void unlockRange(uintptr_t offset, size_t size) = 0;

	// Optimistically returns the physical memory that backs a range of memory.
	// The frame backs the range until the range is evicted, but without a lock (see lockRange())
	// it may only be accessed within the RCU critical section that called peekRange(),
	// unless a use count is taken in that section; see CachePage::useCount.
	// Note that:
	// - The offset is not necessarily page aligned.
	// - The offset is not necessarily within the memory object's current size.
	virtual PhysicalRange peekRange(uintptr_t offset, FetchFlags flags) = 0;

	// Makes a range of memory available for peekRange().
	// The sizeHint parameter is a hint; the implementation may affect fewer bytes.
	// Returns the number of bytes that were actually affected.
	// Note that:
	// - The offset and sizeHint are not necessarily page aligned.
	// - The offset is not necessarily within the memory object's current size.
	virtual coroutine<frg::expected<Error, size_t>>
	touchRange(uintptr_t offset, size_t sizeHint, FetchFlags flags) = 0;

	// Tries to synchronously accesses the range at a given offset.
	// Invokes fn(ptr, chunk) on the range if it is present with chunk <= size.
	// Callers do not need to participate in the eviction protocol but they need to handle
	// failures due to missing pages (and call touchRange() as needed).
	// Returns chunk if the range is successfully accessed or zero if the page at offset is missing.
	// With fetchRequireMutable, the page is mutable and is marked dirty if fn reports so.
	virtual std::expected<size_t, Error> accessRange(uintptr_t offset, size_t size,
			FetchFlags flags, PageAccessFn fn) = 0;

	virtual coroutine<frg::expected<Error, MemoryNotification>> pollNotification();

	// Called (e.g. by user space) to update a range after loading or writeback.
	virtual Error updateRange(ManageRequest type, size_t offset, size_t length);

	// Marks present pages in a range as dirty.
	virtual Error markDirtyRange(size_t offset, size_t length);

	virtual coroutine<frg::expected<Error>> writebackFence(uintptr_t offset, size_t size);

	virtual coroutine<frg::expected<Error>> invalidateRange(uintptr_t offset, size_t size,
			DiscardMode mode);

	virtual Error setIndirection(size_t slot, smarter::shared_ptr<MemoryView> view,
			uintptr_t offset, size_t size, CachingFlags flags);

	coroutine<frg::expected<Error>>
	touchFullRange(uintptr_t offset, size_t size, FetchFlags flags);

private:
	frg::intrusive_shared_ptr<EvictionQueue, Allocator> evictionQueue_;
};

struct SliceRange {
	MemoryView *view;
	uintptr_t displacement;
	size_t size;
};

struct MemorySlice : RcuProtected {
private:
	struct CtorToken {};

public:
	static std::expected<smarter::shared_ptr<MemorySlice>, Error> create(
			smarter::shared_ptr<MemoryView> view, ptrdiff_t view_offset, size_t view_size,
			CachingFlags cachingFlags = 0);

	MemorySlice(CtorToken, smarter::shared_ptr<MemoryView> view,
			ptrdiff_t view_offset, size_t view_size, CachingFlags cachingFlags);

	smarter::shared_ptr<MemoryView> getView() {
		return _view;
	}

	CachingFlags getCachingFlags() const {
		return cachingFlags_;
	}

	uintptr_t offset() { return _viewOffset; }
	size_t length() { return _viewSize; }

private:
	smarter::shared_ptr<MemoryView> _view;
	ptrdiff_t _viewOffset;
	size_t _viewSize;
	CachingFlags cachingFlags_;
};

coroutine<frg::expected<Error>> copyBetweenViews(
		MemoryView *destView, uintptr_t destOffset,
		MemoryView *srcView, uintptr_t srcOffset, size_t size);

// ----------------------------------------------------------------------------------

struct ImmediateMemory;

smarter::shared_ptr<MemoryView> getZeroMemory();

// Memory that is allocated by the kernel and never swapped out.
// In contrast to most other memory objects, it can be accessed synchronously.
struct ImmediateMemory final : MemoryView {
private:
	struct CtorToken {};

public:
	static std::expected<smarter::shared_ptr<ImmediateMemory>, Error>
	create(size_t length);

	ImmediateMemory(CtorToken);
	ImmediateMemory(const ImmediateMemory &) = delete;
	~ImmediateMemory();

	ImmediateMemory &operator= (const ImmediateMemory &) = delete;

	size_t getLength() override;
	coroutine<frg::expected<Error>> resize(size_t newLength) override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	PhysicalRange peekRange(uintptr_t offset, FetchFlags flags) override;
	coroutine<frg::expected<Error, size_t>>
			touchRange(uintptr_t offset, size_t sizeHint, FetchFlags flags) override;
	std::expected<size_t, Error> accessRange(uintptr_t offset, size_t size,
			FetchFlags flags, PageAccessFn fn) override;

	template<typename T>
	T *accessImmediate(uintptr_t offset) {
		static_assert(sizeof(T) <= kPageSize); // Otherwise, the assert below will always fail.
		auto misalign = offset & (kPageSize - 1);
		assert(misalign + sizeof(T) <= kPageSize);

		auto index = offset >> kPageShift;
		assert(index < _physicalPages.size());
		PageAccessor accessor{_physicalPages[index]};
		return reinterpret_cast<T *>(
				reinterpret_cast<std::byte *>(accessor.get()) + misalign);
	}

	void writeImmediate(uintptr_t offset, void *pointer, size_t size) {
		size_t progress = 0;
		while(progress < size) {
			auto misalign = (offset + progress) & (kPageSize - 1);
			auto chunk = frg::min(size - progress, kPageSize - misalign);

			auto index = (offset + progress) >> kPageShift;
			assert(index < _physicalPages.size());
			PageAccessor accessor{_physicalPages[index]};
			memcpy(reinterpret_cast<std::byte *>(accessor.get()) + misalign,
					reinterpret_cast<std::byte *>(pointer) + progress, chunk);
			progress += chunk;
		}
	}

	void readImmediate(uintptr_t offset, void *pointer, size_t size) {
		size_t progress = 0;
		while(progress < size) {
			auto misalign = (offset + progress) & (kPageSize - 1);
			auto chunk = frg::min(size - progress, kPageSize - misalign);

			auto index = (offset + progress) >> kPageShift;
			assert(index < _physicalPages.size());
			PageAccessor accessor{_physicalPages[index]};
			memcpy(reinterpret_cast<std::byte *>(pointer) + progress,
					reinterpret_cast<std::byte *>(accessor.get()) + misalign, chunk);
			progress += chunk;
		}
	}

public:
	// Contract: set by the code that constructs this object.
	smarter::borrowed_ptr<ImmediateMemory> selfPtr;
private:
	frg::ticket_spinlock _mutex;

	frg::vector<PhysicalAddr, KernelAlloc> _physicalPages;
};

struct ImmediateWindow {
	friend void swap(ImmediateWindow &x, ImmediateWindow &y) {
		using std::swap;
		swap(x._memory, y._memory);
		swap(x._base, y._base);
		swap(x._size, y._size);
	}

	ImmediateWindow() : _base{nullptr}, _size{0} { }

	ImmediateWindow(smarter::shared_ptr<ImmediateMemory> memory);

	ImmediateWindow(const ImmediateWindow &) = delete;

	ImmediateWindow(ImmediateWindow &&other)
	: ImmediateWindow{} {
		swap(*this, other);
	}

	ImmediateWindow &operator=(ImmediateWindow other) {
		swap(*this, other);
		return *this;
	}

	~ImmediateWindow();

	std::byte *bytes_data(size_t offset = 0) {
		return reinterpret_cast<std::byte *>(_base) + offset;
	}

	template<typename T>
	T *access(uintptr_t offset) {
		return std::launder(reinterpret_cast<T *>(bytes_data(offset)));
	}

private:
	smarter::shared_ptr<ImmediateMemory> _memory;
	void *_base;
	size_t _size;
};

struct HardwareMemory final : MemoryView {
private:
	struct CtorToken {};

public:
	static std::expected<smarter::shared_ptr<HardwareMemory>, Error> create(
			PhysicalAddr base, size_t length, CachingMode cache_mode);

	HardwareMemory(CtorToken, PhysicalAddr base, size_t length, CachingMode cache_mode);
	HardwareMemory(const HardwareMemory &) = delete;
	~HardwareMemory();

	HardwareMemory &operator= (const HardwareMemory &) = delete;

	size_t getLength() override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	PhysicalRange peekRange(uintptr_t offset, FetchFlags flags) override;
	coroutine<frg::expected<Error, size_t>>
			touchRange(uintptr_t offset, size_t sizeHint, FetchFlags flags) override;
	std::expected<size_t, Error> accessRange(uintptr_t offset, size_t size,
			FetchFlags flags, PageAccessFn fn) override;

private:
	PhysicalAddr _base;
	size_t _length;
	CachingMode _cacheMode;
};

struct AllocatedMemory final : MemoryView {
private:
	struct CtorToken {};

public:
	static std::expected<smarter::shared_ptr<AllocatedMemory>, Error> create(
		smarter::shared_ptr<Hierarchy> hierarchy,
		size_t length,
		int addressBits = 64,
		size_t chunkSize = kPageSize,
		size_t chunkAlign = kPageSize
	);

	AllocatedMemory(
		CtorToken,
		smarter::borrowed_ptr<Hierarchy> hierarchy,
		size_t length,
		int addressBits,
		size_t chunkSize,
		size_t chunkAlign
	);

	AllocatedMemory(const AllocatedMemory &) = delete;
	~AllocatedMemory();

	AllocatedMemory &operator= (const AllocatedMemory &) = delete;

	size_t getLength() override;
	coroutine<frg::expected<Error>> resize(size_t newLength) override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	PhysicalRange peekRange(uintptr_t offset, FetchFlags flags) override;
	coroutine<frg::expected<Error, size_t>>
			touchRange(uintptr_t offset, size_t sizeHint, FetchFlags flags) override;
	std::expected<size_t, Error> accessRange(uintptr_t offset, size_t size,
			FetchFlags flags, PageAccessFn fn) override;

public:
	// Contract: set by the code that constructs this object.
	smarter::borrowed_ptr<AllocatedMemory> selfPtr;
private:
	frg::ticket_spinlock _mutex;

	// Kept alive by the HierarchyAllocator that allocated this AllocatedMemory.
	smarter::borrowed_ptr<Hierarchy> _hierarchy;
	frg::vector<PhysicalAddr, AccountAllocator> _physicalChunks;
	int _addressBits;
	size_t _chunkSize, _chunkAlign;
};

struct ManagedSpace : CacheBundle {
	enum class LoadState : uint8_t {
		// Page contents are not valid.
		missing,
		// Page contents are valid and page is returned from peekRange().
		present,
	};

	enum class TxState : uint8_t {
		// Page is not in a queue.
		// Valid in LoadState::missing. Valid in LoadState::present with lockCount > 0.
		none,
		// Page is in _initializationList.
		// Valid in LoadState::missing.
		wantInitialization,
		// Page is not in a queue but waiting for updateRange() to mark it as initialized.
		// Valid in LoadState::missing.
		initialization,
		// Page is in _dirtyList, waiting to obtain a slot.
		// Valid in LoadState::present.
		dirty,
		// Page is in _writebackList.
		// Valid in LoadState::present.
		wantWriteback,
		// Page is not in a queue but waiting for updateRange() to mark it as clean.
		// Valid in LoadState::present.
		writeback,
		// Page is in the memory reclaimer's LRU queue.
		// Page is owned by the MemoryReclaimer.
		// Valid in LoadState::present with lockCount == 0 and useCount == 0.
		inReclaimer,
		// Page has been selected for reclaimation and is awaiting an RCU grace period.
		// Page is owned by the ManagedSpace reclamation logic.
		// Valid in LoadState::present.
		performReclaim,
		// Page will not be reclaimed but is still awaiting an RCU grace period.
		// Page is owned by the ManagedSpace reclamation logic.
		// Valid in LoadState::present.
		avertReclaim,
		// Page is in _discardList, waiting to be picked up by the reclamation coroutine.
		// Page is owned by the ManagedSpace reclamation logic.
		// Valid in any LoadState, with or without a frame,
		// with lockCount == 0 and useCount == 0.
		discardQueued,
		// Page has been picked up for discarding and is awaiting an RCU grace period.
		// Page is owned by the ManagedSpace reclamation logic.
		// Valid in any LoadState, with or without a frame,
		// with lockCount == 0 and useCount == 0.
		performDiscard,
		// Page will not be discarded in this iteration but is still awaiting an RCU grace period.
		// The page may be in the _discardList (on discardQueued -> avertDiscard transitions).
		// Page is owned by the ManagedSpace reclamation logic.
		// Valid in any LoadState, with or without a frame.
		avertDiscard,
		// Page is in _invalidationList and waiting for existing mappings to be invalidated.
		// Page is owned by the invalidation coroutine.
		// Valid in any LoadState, with or without a frame.
		// Never entered on SwapSpaces (slot identities cannot be translated back to view offsets).
		invalidation,
	};

	// Type of transaction that a TransactionMonitor is attached to.
	enum class MonitorType : uint8_t {
		initialization,
		writeback,
		discard,
	};

	// Struct that is attached to ManagedPage for the duration of
	// a single transaction (i.e., initialization or writeback) or of a discard.
	// At most one monitor per type can be attached at a time; the attached monitors
	// form a chain headed by ManagedPage::monitors.
	// For MonitorType::initialization:
	// * Attached by a waiter in TxState::wantInitialization or TxState::initialization.
	// * Completed when leaving TxState::initialization.
	// For MonitorType::writeback:
	// * Attached by a waiter to any page with unwritten data; see ManagedPage::hasUnwrittenData().
	// * Completed when leaving TxState::writeback (or when the dirty data is dropped).
	// For MonitorType::discard (waiting for a discarded page's entry to be erased):
	// * Attached by a waiter to any page that has `discarded` set, in any TxState;
	//   it stays attached across intermediate transactions.
	// * Completed by the reclamation coroutine when the entry is erased.
	struct TransactionMonitor final : frg::intrusive_rc {
		explicit TransactionMonitor(MonitorType type)
		: type{type} { }

		MonitorType type;
		async::oneshot_primitive event;
		// Next monitor attached to the same ManagedPage.
		TransactionMonitor *chainNext{nullptr};
		frg::default_list_hook<TransactionMonitor> pendingHook;
	};

	using MonitorPendingList = frg::intrusive_list<
		TransactionMonitor,
		frg::locate_member<
			TransactionMonitor,
			frg::default_list_hook<TransactionMonitor>,
			&TransactionMonitor::pendingHook
		>
	>;

	// Identity of a swap page that currently holds no slot.
	static constexpr uint64_t noSlot = ~uint64_t{0};

	// State of a swap slot that the swap daemon accesses through BackingMemory.
	// An entry exists (keyed by identity) while the slot is locked; updateRange() fails on such slots.
	struct SlotState {
		// Locks taken through BackingMemory (lockRange() and in-flight accessRange() calls).
		unsigned int locks = 0;
	};

	struct ManagedPage {
		ManagedPage(ManagedSpace *bundle, uint64_t identity) {
			cachePage.bundle = bundle;
			cachePage.identity = identity;
		}

		ManagedPage(const ManagedPage &) = delete;

		ManagedPage &operator= (const ManagedPage &) = delete;

		// Allocates and attaches a monitor of the given type; the type must not be attached yet.
		frg::intrusive_shared_ptr<TransactionMonitor, Allocator> attachMonitor(MonitorType type);
		// Returns the attached monitor of the given type, or null.
		frg::intrusive_shared_ptr<TransactionMonitor, Allocator> findMonitor(MonitorType type);
		// Returns the attached monitor of the given type, attaching one if there is none yet.
		frg::intrusive_shared_ptr<TransactionMonitor, Allocator> requireMonitor(MonitorType type);
		// Detaches and returns the monitor of the given type, or null.
		frg::intrusive_shared_ptr<TransactionMonitor, Allocator> detachMonitor(MonitorType type);
		// Whether a monitor of the given type is attached.
		bool hasMonitor(MonitorType type);
		// Whether the page holds data that has not reached the backing store yet.
		bool hasUnwrittenData();
		// Whether the page holds a slot of the backing store (i.e., has an identity).
		// Always true on file-backed spaces; on swap spaces, slots are transient.
		bool hasSlot() { return cachePage.identity != noSlot; }

		PhysicalAddr physical = PhysicalAddr(-1);
		LoadState loadState{LoadState::missing};
		TxState transactionState{TxState::none};
		// Whether the page is dirty even after a pending writeback completes.
		// Can only be true in LoadState::present and TxState::writeback, avertReclaim,
		// invalidation or avertDiscard.
		// Eventually causes another writeback (unless the page is discarded without writeback).
		bool stillDirty{false};
		// Whether the backing store's copy of the page matches its last in-memory contents.
		// Maintained by markDirty()/updateRange().
		bool swapCopyValid{false};
		// The page is being torn down - see discardPage(). Set once, never cleared.
		bool discarded{false};
		// How dirty contents are treated during the discard.
		// DiscardMode::none until discarded is set. Fixed by the first discardPage() call.
		DiscardMode discardMode{DiscardMode::none};
		unsigned int lockCount = 0;
		CachePage cachePage;
		// Head of the chain of monitors attached to this page's in-flight transactions.
		// The page owns one reference (= refcount) of each of these monitors.
		TransactionMonitor *monitors{nullptr};
		// Bitmask (indexed by MonitorType) of the attached monitor types.
		uint8_t attachedMonitors{0};
	};

	static std::expected<smarter::shared_ptr<ManagedSpace>, Error> create(
			smarter::shared_ptr<Hierarchy> hierarchy, size_t length, bool readahead);

	ManagedSpace(smarter::borrowed_ptr<Hierarchy> hierarchy, size_t length, bool readahead);
	~ManagedSpace();

	// dispose() hook for allocate_rcu_shared().
	coroutine<void> dispose();

	void incrementUses(CachePage *page) override;
	void decrementUses(CachePage *page) override;
	void markDirty(CachePage *page) override;

	// Called under mutex before a dirty page transitions to writeback.
	// Ensures that the page holds a slot of the backing store.
	// Returning false leaves the page on _dirtyList until a slot becomes available.
	virtual bool claimSlot(ManagedPage *page);

	// Releases the page's slot (if it holds one) since its disk copy became stale.
	// Unblocks the drain coroutine if a slot was released; the caller raises _dirtyEvent.
	// Called under mutex.
	virtual void _releaseSlot(ManagedPage *page);

	// Installs a frame for a page that is currently missing (and in no transaction):
	// - registers it in the pfn-db,
	// - makes the page present,
	// - hands it to the dirty pipeline if its dirty,
	// - hands it to the reclaimer LRU if its clean and unreferenced.
	// Must be called under mutex.
	// The caller must raise _dirtyEvent/_expediteEvent as requested.
	// Precondition: !page->discarded.
	// Precondition: The backing store holds no copy of the page (i.e., page->swapCopyValid is false).
	//               Pages that have one are populated by initializePage() instead.
	// Precondition: If !dirty, the frame is zero-filled.
	//               This is needed since non-dirty pages can be reclaimed and would be re-created by zero-filling.
	// Precondition: The caller owns the fully initialized frame, which is not in the pfn-db yet.
	// Precondition: The caller charged the frame to hierarchy.
	void installPage(ManagedPage *page, PhysicalAddr physical, bool dirty,
			unsigned int extraLockCount, bool &raiseDirty, bool &raiseExpedite);

	// Attaches a frame to a missing page.
	// Faulting threads attach frames before they request initialization via the manage protocol,
	// such that the manager does not need to allocate them.
	// Must be called under mutex.
	// Precondition: The page is missing and has no frame.
	// Precondition: The caller owns the zero-filled frame, which is charged to hierarchy
	//               and not in the pfn-db yet.
	void attachFrame(ManagedPage *page, PhysicalAddr physical);

	// Discards the given page. The entry is either immediately erased
	// or once the in-flight transaction is completed. The frames are freed by
	// the reclamation behind an RCU grace period.
	// Idempotent: discarding an already discarded page is a no-op (i.e., the first call fixes the mode).
	// Must be called under mutex.
	// The caller must raise the appended monitors and _dirtyEvent/_discardEvent/_expediteEvent as requested.
	void discardPage(ManagedPage *pit, DiscardMode mode, bool &raiseDirty, bool &raiseDiscard,
			bool &raiseExpedite, MonitorPendingList &pendingMonitors);

	// Discards a single page and raises the resulting monitors and events.
	// Must be called outside of locks.
	void discardPageAndRaise(ManagedPage *page, DiscardMode mode);

	// Moves a present page into TxState::dirty / _dirtyList.
	// Arms the writeback deadline. Discarded pages expedite the writeback.
	// Sets raiseExpedite to true if _expediteEvent needs to be raised (and doesn't modify it otherwise).
	// Callers must hold mutex.
	void _enqueueDirty(ManagedPage *page, bool &raiseExpedite);

	// Removes a page from _dirtyList (does not change the transaction state).
	// Disarms the writeback deadline once the list runs empty.
	// Callers must hold mutex.
	void _dequeueDirty(ManagedPage *page);

	// Queues a discarded page for the reclamation coroutine, which erases its entry.
	// Must be called under mutex with transactionState == TxState::none.
	// Sets raiseDiscard to true if _discardEvent needs to be raised (and doesn't modify it otherwise).
	void _disposeDiscarded(ManagedPage *page, bool &raiseDiscard);

	// Raises (and releases the references of) the given detached monitors.
	// Must be called without holding mutex.
	static void _raiseMonitors(MonitorPendingList &pendingMonitors);

	// Completes the given management requests (as collected by _progressManagement()).
	// Must be called outside of locks.
	static void _raiseManagement(ManageList &pendingManagement);

	// Notifies the subclass that a discarded page's entry is about to be erased.
	// Sets raiseDirty to true if _dirtyEvent needs to be raised and doesn't modify it otherwise.
	// Called under mutex.
	virtual void _pageDiscarded(ManagedPage *page, bool &raiseDirty);

	// Unblocks the drain coroutine after slots became available.
	void _wakeDrain();

	coroutine<void> _runReclaimLoop();
	coroutine<void> _runDrainLoop();
	coroutine<void> _runInvalidationLoop();

	Error lockPages(uintptr_t offset, size_t size);
	void unlockPages(uintptr_t offset, size_t size);

	// Per-page counterparts of lockPages()/unlockPages().
	// Must be called under mutex.
	void lockPage(ManagedPage *page);
	// Sets raiseDiscard to true if _discardEvent needs to be raised (and doesn't modify it otherwise).
	// Must be called under mutex.
	void unlockPage(ManagedPage *page, bool &raiseDiscard);

	// Unlocks a page locked by lockPage(), marking it dirty first if requested.
	// Must be called outside of locks.
	void unlockPageAndRaise(ManagedPage *page, bool dirty);

	// Lock the page's slot for the swap daemon (instead of lockPage()/unlockPage()).
	// The page must be handed to the manager (see isHandedToManager()) when it is locked.
	// Must be called under mutex.
	void _lockSlot(ManagedPage *page);
	void _unlockSlot(ManagedPage *page);
	// Must be called outside of locks.
	void _unlockSlotAndRaise(ManagedPage *page, bool dirty);

	// Per-page counterpart of markDirty().
	// Sets needsEvent/needsExpedite if _dirtyEvent/_expediteEvent need to be raised.
	// Must be called under mutex.
	void markDirtyPage(ManagedPage *page, bool &needsEvent, bool &needsExpedite);

	// Implements MemoryView::accessRange() for the page returned by lookup.
	// The lookup function runs under mutex.
	// Must be called outside of locks.
	template<typename F>
	std::expected<size_t, Error> accessPage(F lookup, uintptr_t misalign,
			size_t size, bool isMutable, PageAccessFn fn) {
		ManagedPage *page;
		PhysicalAddr physical;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&mutex);

			auto lookupOutcome = lookup();
			if(!lookupOutcome)
				return std::unexpected{lookupOutcome.error()};
			page = *lookupOutcome;
			if(!page || page->loadState != LoadState::present)
				return 0;
			assert(page->physical != PhysicalAddr(-1));
			lockPage(page);
			physical = page->physical;
		}

		auto chunk = frg::min(size, kPageSize - misalign);
		PageAccessResult result;
		{
			PageAccessor accessor{physical};
			result = fn(reinterpret_cast<uint8_t *>(accessor.get()) + misalign, chunk);
		}
		assert(!result.dirty || isMutable);
		unlockPageAndRaise(page, result.dirty);
		return chunk;
	}

	// Returns the frame of a present page (averting an in-flight reclamation or discard),
	// or PhysicalAddr(-1) if the page is missing.
	// Must be called under mutex.
	PhysicalAddr peekPage(ManagedPage *page);

	// Makes a present page recently used (or averts its in-flight reclamation or discard).
	// Returns false if the page is missing.
	// Must be called under mutex.
	bool touchPresentPage(ManagedPage *page);

	// Frames that the faulting thread provides for readahead.
	struct ReadaheadFrames {
		// initializePage() resets the entries that it attaches to -1.
		std::span<PhysicalAddr> physicals;
		// Set by initializePage() to the number of frames that readahead needs beyond physicals.
		size_t missing = 0;
	};

	// Requests the initialization of a missing page via the manage protocol.
	// Fails on fetchDisallowBacking.
	// The faulting thread provides the page's frame such that the manager does not need to
	// allocate it: if the page has no frame, freshPhysical is attached (and reset to -1).
	// If freshPhysical is -1 in that case, returns a null monitor without requesting initialization.
	// Also requests readahead (if enabled) for the readahead pages that already have a frame
	// or that readaheadFrames provides one for; the others are counted in readaheadFrames->missing.
	// A null monitor also requests no readahead, but readaheadFrames->missing is still set.
	// readaheadFrames can be null if readahead is disabled.
	// Returns the monitor that is raised once the page is initialized.
	// Must be called under mutex.
	// The caller must raise the appended management requests before awaiting the monitor.
	// Precondition: on a SwapSpace, the page has a valid disk copy (i.e., page->swapCopyValid is true).
	// Precondition: freshPhysical and all entries of readaheadFrames->physicals are zero-filled,
	//               charged to hierarchy and not in the pfn-db yet.
	std::expected<frg::intrusive_shared_ptr<TransactionMonitor, Allocator>, Error>
	initializePage(ManagedPage *page, FetchFlags flags, PhysicalAddr &freshPhysical,
			ReadaheadFrames *readaheadFrames, ManageList &pendingManagement);

	void submitManagement(ManageNode *node);
	void _progressManagement(ManageList &pending);

	// Returns the page with the given identity, or null.
	// Must be called under mutex.
	ManagedPage *findPage(uint64_t index);
	// Returns the page with the given identity, creating a fresh one if there is none.
	// Must be called under mutex.
	ManagedPage *findOrInsertPage(uint64_t index);
	// Whether the page is in an initialization or writeback transaction.
	// On swap spaces, the daemon may only reach such pages through BackingMemory.
	// Must be called under mutex.
	bool isHandedToManager(ManagedPage *page);

	// Erases the page's entry (if it holds a slot) and frees the page.
	// The caller must ensure that lockCount == 0, useCount == 0, and that no RCU reader
	// can still hold a frame it obtained from the page; see CachePage::useCount.
	// Must be called under mutex.
	void _erasePage(ManagedPage *page);

	smarter::borrowed_ptr<ManagedSpace> selfPtr;

	// Kept alive by the HierarchyAllocator that allocated this ManagedSpace.
	smarter::borrowed_ptr<Hierarchy> hierarchy;

	frg::ticket_spinlock mutex;

	// Heap-allocated page objects, indexed by identity. File-backed spaces own their pages;
	// on swap spaces, the attached views own them and only pages that hold a slot are indexed.
	frg::rcu_radixtree<ManagedPage *, AccountAllocator, RcuPolicy> pages;

	size_t numPages;
	bool readahead;
	static constexpr size_t numReadaheadPages = 3;

	// Whether this space is a SwapSpace.
	bool isSwapSpace = false;

	// Delay before dirty pages enter writeback. Longer delays allow for more coalescing.
	uint64_t writebackDelayNanos = 200'000'000;

	// Queue that BackingMemory/FrontalMemory mappings observe.
	frg::intrusive_shared_ptr<EvictionQueue, Allocator> _evictQueue;

	CachePagesList _dirtyList;

	CachePagesList _initializationList;

	CachePagesList _writebackList;

	// Discarded pages waiting to be picked up by the reclamation coroutine.
	// These pages are either in TxState::discardQueued or TxState::avertDiscard.
	// Protected by mutex.
	CachePagesList _discardList;

	// Discarded pages whose remaining mappings the invalidation coroutine breaks.
	// These pages are in TxState::invalidation.
	// Like _discardList, the owning coroutine is woken by _discardEvent.
	// Protected by mutex.
	CachePagesList _invalidationList;

	// See SlotState. Only used on swap spaces.
	// Protected by mutex.
	frg::rcu_radixtree<SlotState, AccountAllocator, RcuPolicy> _slotStates;

	ManageList _managementQueue;

	async::recurring_event _dirtyEvent;

	async::recurring_event _discardEvent;

	// Set by the drain coroutine when dirty pages exist but none of them could obtain a slot.
	// While this is set, _dirtyEvent does not wake the drain coroutine.
	// Protected by mutex.
	bool _drainBlocked = false;

	// Deadline (for getClockNanos()) at which the drain coroutine starts writeback. Zero when unarmed.
	// Protected by mutex.
	uint64_t _writebackDeadline = 0;

	// Causes _writebackDeadline to be ignored (e.g., in case of memory pressure).
	// Cleared by the drain pass that acts on it.
	// Protected by mutex.
	bool _writebackExpedited = false;

	// Wakes the drain coroutine after _writebackExpedited has been set.
	async::recurring_event _expediteEvent;

	// Makes the reclaim/drain/invalidation loop exit at their next wait.
	// Protected by mutex.
	bool _stopLoops = false;

	// Counts the reclaim/drain/invalidation loops that have not exited yet.
	async::wait_group _runningLoops{0};
};
static_assert(HasDispose<ManagedSpace>);

// Backing store for swappable anonymous memory.
// A slot is a page-sized position in the backing store, identified by its swap offset.
// A page obtains a slot when it is first written back and holds it while its disk copy
// is valid; a page that is dirtied again releases its slot.
// Swap offsets are split into a region (e.g., a swap file) and the slot within the region.
struct SwapSpace final : ManagedSpace, RcuProtected {
	// Number of low bits of a swap offset (in pages) that select the slot within its region.
	static constexpr int regionShift = 32;
	// Swap offsets of all regions fit into the backing memory (see backingMemoryLength).
	static constexpr unsigned int maxRegions = 1 << (62 - regionShift - kPageShift);
	// The last offset of each region stays unused, such that runs of consecutive offsets
	// (and thus fused manage requests) never cross region boundaries.
	static constexpr size_t maxRegionSlots = (size_t{1} << regionShift) - 1;

	static std::expected<smarter::shared_ptr<SwapSpace>, Error> create(
			smarter::shared_ptr<Hierarchy> hierarchy);

	SwapSpace(smarter::borrowed_ptr<Hierarchy> hierarchy);
	~SwapSpace();

	bool claimSlot(ManagedPage *page) override;
	void _releaseSlot(ManagedPage *page) override;
	void _pageDiscarded(ManagedPage *page, bool &raiseDirty) override;

	// Sets the capacity of the given region to numSlots, i.e., makes the offsets
	// [region << regionShift, (region << regionShift) + numSlots) available as slots.
	// Only attaching a region (i.e., changing its capacity from zero) is supported so far.
	coroutine<std::expected<void, Error>> setRegionCapacity(unsigned int region, size_t numSlots);

	// Allocates a swap page without a slot or a physical page frame.
	// Returns null if the page cannot be allocated (which currently never happens).
	// The page is fresh (missing, unlocked, not discarded, no disk copy, i.e., fit for installPage())
	// and owned by the caller, who must eventually discard it.
	// Must be called under mutex.
	ManagedPage *allocatePage();

private:
	// Slot allocator of an attached region.
	struct Region {
		Region(unsigned int regionIndex, size_t numSlots, AccountAllocator alloc);

		Region(const Region &) = delete;
		Region &operator= (const Region &) = delete;

		unsigned int regionIndex;
		size_t numSlots;
		frg::vector<int8_t, AccountAllocator> metadata;
		BuddyAccessor accessor;
		frg::rbtree_hook treeNode;
	};

	struct RegionLess {
		bool operator() (const Region &a, const Region &b) {
			return a.regionIndex < b.regionIndex;
		}
	};

	using RegionTree = frg::rbtree<Region, &Region::treeNode, RegionLess>;

	// Returns the attached region with the given index, or null.
	// Must be called under mutex.
	Region *_findRegion(unsigned int regionIndex);

	// Allocates the lowest free swap offset (in pages, not bytes) of the lowest region
	// that has a free slot.
	// Must be called under mutex.
	frg::optional<uint64_t> _allocateOffset();

	// Frees the given swap offset (in pages not bytes).
	// Must be called under mutex.
	void _freeOffset(uint64_t offset);

	// Attached regions, ordered by index. Protected by mutex.
	RegionTree _regions;
};
static_assert(HasDispose<SwapSpace>);

// Static size of BackingMemory views.
// It also bounds the ManagedSpace size that is visible to FrontalMemory
// (otherwise, some pages would be unreachable through the backing view).
inline constexpr size_t backingMemoryLength = size_t{1} << 62;

static_assert((size_t{SwapSpace::maxRegions} << (SwapSpace::regionShift + kPageShift))
		== backingMemoryLength);

struct BackingMemory final : MemoryView {
private:
	struct CtorToken {};

public:
	static std::expected<smarter::shared_ptr<BackingMemory>, Error> create(
			smarter::shared_ptr<ManagedSpace> managed);

	BackingMemory(CtorToken, smarter::shared_ptr<ManagedSpace> managed)
	: MemoryView{managed->_evictQueue}, _managed{std::move(managed)} { }

	BackingMemory(const BackingMemory &) = delete;

	BackingMemory &operator= (const BackingMemory &) = delete;

	size_t getLength() override;
	coroutine<frg::expected<Error>> resize(size_t newLength) override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	PhysicalRange peekRange(uintptr_t offset, FetchFlags flags) override;
	coroutine<frg::expected<Error, size_t>>
			touchRange(uintptr_t offset, size_t sizeHint, FetchFlags flags) override;
	std::expected<size_t, Error> accessRange(uintptr_t offset, size_t size,
			FetchFlags flags, PageAccessFn fn) override;
	coroutine<frg::expected<Error, MemoryNotification>> pollNotification() override;
	Error updateRange(ManageRequest type, size_t offset, size_t length) override;
	Error markDirtyRange(size_t offset, size_t length) override;
	coroutine<frg::expected<Error>> writebackFence(uintptr_t offset, size_t size) override;
	coroutine<frg::expected<Error>> invalidateRange(uintptr_t offset, size_t size,
			DiscardMode mode) override;

private:
	smarter::shared_ptr<ManagedSpace> _managed;
};

struct FrontalMemory final : MemoryView {
private:
	struct CtorToken {};

public:
	static std::expected<smarter::shared_ptr<FrontalMemory>, Error> create(
			smarter::shared_ptr<ManagedSpace> managed);

	FrontalMemory(CtorToken, smarter::shared_ptr<ManagedSpace> managed)
	: MemoryView{managed->_evictQueue}, _managed{std::move(managed)} { }

	FrontalMemory(const FrontalMemory &) = delete;

	FrontalMemory &operator= (const FrontalMemory &) = delete;

	size_t getLength() override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	PhysicalRange peekRange(uintptr_t offset, FetchFlags flags) override;
	coroutine<frg::expected<Error, size_t>>
			touchRange(uintptr_t offset, size_t sizeHint, FetchFlags flags) override;
	std::expected<size_t, Error> accessRange(uintptr_t offset, size_t size,
			FetchFlags flags, PageAccessFn fn) override;

public:
	// Contract: set by the code that constructs this object.
	smarter::borrowed_ptr<FrontalMemory> selfPtr;
private:
	smarter::shared_ptr<ManagedSpace> _managed;
};

// Anonymous memory backed by a SwapSpace.
// The view translates its own page indices to lazily allocated swap pages.
// Frames and per-page state are owned by the SwapSpace.
// The view's hierarchy is charged as swap for each of its swap pages (whether or not the page holds
// a slot), the SwapSpace's hierarchy for the frames.
struct SwappableMemory final : MemoryView {
private:
	struct CtorToken {};

public:
	static std::expected<smarter::shared_ptr<SwappableMemory>, Error> create(
			smarter::shared_ptr<Hierarchy> hierarchy, smarter::shared_ptr<SwapSpace> space,
			size_t length);

	SwappableMemory(CtorToken, smarter::borrowed_ptr<Hierarchy> hierarchy,
			smarter::shared_ptr<SwapSpace> space, size_t length);
	~SwappableMemory();

	SwappableMemory(const SwappableMemory &) = delete;
	SwappableMemory &operator= (const SwappableMemory &) = delete;

	size_t getLength() override;
	coroutine<frg::expected<Error>> resize(size_t newLength) override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	PhysicalRange peekRange(uintptr_t offset, FetchFlags flags) override;
	coroutine<frg::expected<Error, size_t>>
			touchRange(uintptr_t offset, size_t sizeHint, FetchFlags flags) override;
	std::expected<size_t, Error> accessRange(uintptr_t offset, size_t size,
			FetchFlags flags, PageAccessFn fn) override;

public:
	// Contract: set by the code that constructs this object.
	smarter::borrowed_ptr<SwappableMemory> selfPtr;

private:
	// Returns the swap page backing the given view page index, allocating one on demand.
	// Returns null if SwapSpace::allocatePage() does.
	// Must be called under the SwapSpace mutex.
	ManagedSpace::ManagedPage *_translate(uint64_t index);

	// Unlock counterpart of lockRange().
	// Must be called under the SwapSpace mutex.
	void _unlockPagesLocked(uintptr_t offset, size_t size, bool &raiseDiscard);

	// Kept alive by the HierarchyAllocator that allocated this SwappableMemory.
	smarter::borrowed_ptr<Hierarchy> _hierarchy;
	smarter::shared_ptr<SwapSpace> _space;
	size_t _length;

	// Maps page index -> swap page. The swap pages are owned by _space.
	frg::rcu_radixtree<ManagedSpace::ManagedPage *, AccountAllocator, RcuPolicy> _table;
};

struct IndirectMemory final : MemoryView {
private:
	struct CtorToken {};

public:
	static std::expected<smarter::shared_ptr<IndirectMemory>, Error> create(size_t numSlots);

	IndirectMemory(CtorToken, size_t numSlots);
	IndirectMemory(const IndirectMemory &) = delete;
	~IndirectMemory();

	IndirectMemory &operator= (const IndirectMemory &) = delete;

	size_t getLength() override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	PhysicalRange peekRange(uintptr_t offset, FetchFlags flags) override;
	coroutine<frg::expected<Error, size_t>>
			touchRange(uintptr_t offset, size_t sizeHint, FetchFlags flags) override;
	std::expected<size_t, Error> accessRange(uintptr_t offset, size_t size,
			FetchFlags flags, PageAccessFn fn) override;

	Error setIndirection(size_t slot, smarter::shared_ptr<MemoryView> memory,
			uintptr_t offset, size_t size, CachingFlags flags) override;

private:
	struct IndirectionSlot {
		IndirectionSlot(IndirectMemory *owner, size_t slot,
				smarter::shared_ptr<MemoryView> memory,
				uintptr_t offset, size_t size, CachingFlags flags)
		: owner{owner}, slot{slot}, memory{std::move(memory)}, offset{offset},
			size{size}, flags{flags} { }

		IndirectMemory *owner;
		size_t slot;
		smarter::shared_ptr<MemoryView> memory;
		uintptr_t offset;
		size_t size;
		CachingFlags flags;
	};

	frg::ticket_spinlock mutex_;
	frg::vector<smarter::shared_ptr<IndirectionSlot>, KernelAlloc> indirections_;
};

enum class CowState {
	null,
	inProgress,
	hasCopy
};

struct CowPage {
	~CowPage();

	// For non-swappable views the physical field holds the owned frame.
	// For swappable views the swapPage field holds the swap page that owns the
	// frame and tracks residency and dirtiness instead.
	// Exactly one of these fields will be valid if state == CowState::hasCopy.
	PhysicalAddr physical = -1;
	ManagedSpace::ManagedPage *swapPage = nullptr;
	CowState state = CowState::null;
	unsigned int lockCount = 0;
};

struct CopyOnWriteMemory final : MemoryView /*, MemoryObserver */ {
private:
	struct CtorToken {};

public:
	static std::expected<smarter::shared_ptr<CopyOnWriteMemory>, Error> create(
			smarter::shared_ptr<Hierarchy> hierarchy, smarter::shared_ptr<SwapSpace> space,
			smarter::shared_ptr<MemoryView> view, uintptr_t offset, size_t length);

	CopyOnWriteMemory(CtorToken, smarter::shared_ptr<Hierarchy> hierarchy,
			smarter::shared_ptr<SwapSpace> space, smarter::shared_ptr<MemoryView> view,
			uintptr_t offset, size_t length);
	CopyOnWriteMemory(const CopyOnWriteMemory &) = delete;

	~CopyOnWriteMemory();

	CopyOnWriteMemory &operator= (const CopyOnWriteMemory &) = delete;

	size_t getLength() override;
	coroutine<frg::expected<Error, smarter::shared_ptr<MemoryView>>> fork(
		smarter::shared_ptr<Hierarchy> hierarchy
	) override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	PhysicalRange peekRange(uintptr_t offset, FetchFlags flags) override;
	coroutine<frg::expected<Error, size_t>>
			touchRange(uintptr_t offset, size_t sizeHint, FetchFlags flags) override;
	std::expected<size_t, Error> accessRange(uintptr_t offset, size_t size,
			FetchFlags flags, PageAccessFn fn) override;

private:
	std::expected<void, Error> tryChargePages_(size_t n);
	void unchargePages_(size_t n);

	// Allocate/free an owned copy of a page.
	coroutine<std::expected<PhysicalAddr, Error>> _allocateCopy(FetchFlags flags);
	void _freeCopy(PhysicalAddr physical);

public:
	// Contract: set by the code that constructs this object.
	smarter::borrowed_ptr<CopyOnWriteMemory> selfPtr;
private:
	// Page-level operations on materialized pages that hide whether the view is swappable.
	// Lock order: _mutex is taken before the swap space's mutex.

	// Returns the frame of a materialized page, or PhysicalAddr(-1) if it is swapped out.
	// Must be called under _mutex.
	PhysicalAddr _getResident(CowPage *page);

	// Ensures the materialized page is resident, no-op for unswappable views.
	// The caller must keep the page alive.
	coroutine<frg::expected<Error>> _ensureResident(CowPage *page, FetchFlags flags);

	// Locks/unlocks a page. For swappable views locks of materialized pages are
	// mirrored into the swap page.
	// Must be called under _mutex.
	void _lockPage(CowPage *page);
	void _unlockPage(CowPage *page, bool &raiseDiscard);

	// Copies the content of a materialized page into the given frame.
	coroutine<frg::expected<Error>> _copyFromCowPage(CowPage *src, PhysicalAddr dst, FetchFlags flags);

	// Publishes the given frame as the page's content (state becomes hasCopy).
	// Swappable views install the frame dirty into the swap space and transfer the lock count.
	// Must be called under _mutex unless the page is not visible to other threads yet.
	frg::expected<Error> _publishCopy(CowPage *page, PhysicalAddr physical,
			bool &raiseDirty, bool &raiseExpedite);

	// Attaches a page frame for the given page and performs the copy
	// from the shared page (if any) or the root view.
	// Precondition: the caller has moved the page to CowState::inProgress.
	// Postcondition: Moves the page into hasCopy state on success,
	//                or moves it back into null state on failure.
	coroutine<frg::expected<Error>> _materializePage(uintptr_t offset,
			smarter::shared_ptr<CowPage> cowPage, smarter::shared_ptr<CowPage> sharedPage,
			FetchFlags flags);

	frg::ticket_spinlock _mutex;

	smarter::shared_ptr<Hierarchy> _hierarchy;
	// Constant after construction.
	smarter::shared_ptr<MemoryView> _view;
	// Constant after construction.
	uintptr_t _viewOffset;
	size_t _length;
	// Invariant: _chargedPages is equal to the number of pages in _ownedPages that have a page frame attached
	//            plus the number of pages in _sharedPages.
	size_t _chargedPages{0};
	// Swap space that backs private copies, null if the view is not swappable.
	smarter::shared_ptr<SwapSpace> _space;
	frg::rcu_radixtree<smarter::shared_ptr<CowPage>, KernelAlloc, RcuPolicy> _ownedPages;
	frg::rcu_radixtree<smarter::shared_ptr<CowPage>, KernelAlloc, RcuPolicy> _sharedPages;
	async::recurring_event _copyEvent;
};

FutexRealm *getGlobalFutexRealm();

} // namespace thor
