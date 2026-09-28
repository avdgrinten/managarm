#pragma once

#include <atomic>
#include <cstddef>
#include <expected>

#include <async/algorithm.hpp>
#include <async/oneshot-event.hpp>
#include <async/post-ack.hpp>
#include <async/recurring-event.hpp>
#include <async/wait-group.hpp>
#include <frg/list.hpp>
#include <frg/rcu_btree.hpp>
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
	// Page is currently being evicted (not in LRU list, but in bundle list).
	static constexpr uint32_t reclaimPosted = 0x02;
	// Page has been evicted (neither in the LRU, nor in the bundle list).
	static constexpr uint32_t reclaimInflight = 0x04;

	// CacheBundle that owns this page.
	CacheBundle *bundle = nullptr;

	// Identity of the page as part of the bundle.
	// Bundles can use this field however they like.
	uint64_t identity = 0;

	// Hooks for LRU lists.
	frg::default_list_hook<CachePage> listHook;

	uint32_t flags = 0;
	uint8_t generation = 0;
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

	virtual void incrementUses(CachePage *page) = 0;
	virtual void decrementUses(CachePage *page) = 0;

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

	// Points to the newest generation.
	// This is cyclically incremented on rotation.
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

using CachingFlags = uint32_t;
inline constexpr CachingFlags cacheWriteCombine = 1;

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
	// Waits until all temporary references to pages disappear. No range is specified.
	// CachePages with a useCount of zero can be reclaimed after this fence.
	fenceEphemeral,
	// Waits until dirty pages have been marked as clean. No range is specified.
	// Dirty pages can be written back after this fence.
	fenceDirty,
};

struct RangeToEvict {
	EvictMode mode;
	uintptr_t offset;
	size_t size;
};

struct Eviction {
	Eviction() = default;

	Eviction(async::post_ack_handle<RangeToEvict> handle)
	: handle_{std::move(handle)} { }

	explicit operator bool () {
		return static_cast<bool>(handle_);
	}

	EvictMode mode() { return handle_->mode; }
	uintptr_t offset() { return handle_->offset; }
	uintptr_t size() { return handle_->size; }

	void done() {
		handle_.ack();
	}

private:
	async::post_ack_handle<RangeToEvict> handle_;
};

struct MemoryObserver {
	friend struct MemoryView;
	friend struct EvictionQueue;

	frg::default_list_hook<MemoryObserver> listHook;

private:
	async::post_ack_agent<RangeToEvict> agent_;
};

struct EvictionQueue final : frg::intrusive_rc {
	void addObserver(MemoryObserver *observer) {
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		observer->agent_.attach(&mechanism_);
		observers_.push_back(observer);
		numObservers_++;
	}

	void removeObserver(MemoryObserver *observer) {
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		observer->agent_.detach();
		auto it = observers_.iterator_to(observer);
		observers_.erase(it);
		numObservers_--;
	}

	auto pollEviction(MemoryObserver *observer, async::cancellation_token ct) {
		return observer->agent_.poll(std::move(ct));
	}

	auto breakRange(uintptr_t offset, size_t size) {
		return mechanism_.post(RangeToEvict{EvictMode::breakRange, offset, size});
	}
	auto cleanRange(uintptr_t offset, size_t size) {
		return mechanism_.post(RangeToEvict{EvictMode::cleanRange, offset, size});
	}
	auto fenceEphemeral() {
		return mechanism_.post(RangeToEvict{EvictMode::fenceEphemeral, 0, 0});
	}
	auto fenceDirty() {
		return mechanism_.post(RangeToEvict{EvictMode::fenceDirty, 0, 0});
	}

	frg::default_list_hook<EvictionQueue> attachHook;

private:
	frg::ticket_spinlock mutex_;

	frg::intrusive_list<
		MemoryObserver,
		frg::locate_member<
			MemoryObserver,
			frg::default_list_hook<MemoryObserver>,
			&MemoryObserver::listHook
		>
	> observers_;

	size_t numObservers_ = 0;
	async::post_ack_mechanism<RangeToEvict> mechanism_;
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

	void removeObserver(MemoryObserver *observer) {
		if(evictionQueue_)
			evictionQueue_->removeObserver(observer);
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
	// Result stays valid until the range is evicted.
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

	// ----------------------------------------------------------------------------------
	// Memory eviction.
	// ----------------------------------------------------------------------------------

	bool canEvictMemory() {
		return static_cast<bool>(evictionQueue_);
	}

	auto pollEviction(MemoryObserver *observer, async::cancellation_token ct) {
		return async::transform(observer->agent_.poll(std::move(ct)),
			[] (async::post_ack_handle<RangeToEvict> handle) {
				return Eviction{std::move(handle)};
			}
		);
	}

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
		smarter::shared_ptr<Hierarchy> hierarchy,
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

	smarter::shared_ptr<Hierarchy> _hierarchy;
	frg::vector<PhysicalAddr, KernelAlloc> _physicalChunks;
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
		// Page is in _dirtyList, waiting to claim swap budget.
		// Valid in LoadState::present.
		dirty,
		// Page is on the drain coroutine's local pending list.
		// It has claimed swap budget and awaits the fenceDirty() before it's moved to _writebackList.
		// Valid in LoadState::present.
		pendingWriteback,
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
		// Page has been selected for reclaimation and is awaiting fenceEphemeral().
		// Page is owned by the ManagedSpace reclamation logic.
		// Valid in LoadState::present.
		performReclaim,
		// Page will not be reclaimed but is still awaiting fenceEphemeral().
		// Page is owned by the ManagedSpace reclamation logic.
		// Valid in LoadState::present.
		avertReclaim,
		// Page is in _discardList, waiting to be picked up by the reclamation coroutine.
		// Page is owned by the ManagedSpace reclamation logic.
		// Valid in any LoadState, with or without a frame,
		// with lockCount == 0 and useCount == 0.
		discardQueued,
		// Page has been picked up for discarding and is awaiting fenceEphemeral().
		// Page is owned by the ManagedSpace reclamation logic.
		// Valid in any LoadState, with or without a frame,
		// with lockCount == 0 and useCount == 0.
		performDiscard,
		// Page will not be discarded in this iteration but is still awaiting fenceEphemeral().
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
		// Whether swap budget was claimed for this page.
		// Owned by SwapSpace, not used by ManagedSpace.
		bool swapBudgetClaimed{false};
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

	ManagedSpace(smarter::shared_ptr<Hierarchy> hierarchy, size_t length, bool readahead);
	~ManagedSpace();

	// dispose() hook for allocate_rcu_shared().
	coroutine<void> dispose();

	void incrementUses(CachePage *page) override;
	void decrementUses(CachePage *page) override;
	void markDirty(CachePage *page) override;

	// Called under mutex before a dirty page transitions to writeback.
	// Returning false leaves the page on _dirtyList until swap budget becomes available.
	virtual bool claimSwapBudget(ManagedPage *page);

	// Installs a frame for a page that is currently missing (and in no transaction):
	// - registers it in the pfn-db,
	// - makes the page present (charging it to hierarchy),
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
	void installPage(ManagedPage *page, PhysicalAddr physical, bool dirty,
			unsigned int extraLockCount, bool &raiseDirty, bool &raiseExpedite);

	// Discards the given page. The entry is either immediately erased
	// or once the in-flight transaction is completed. The frames are freed by
	// the reclamation behind a fenceEphemeral().
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

	// Unblocks the drain coroutine after the swap budget has grown.
	void _wakeDrain();

	// Registers/deregisters the queue of an attached view, the space's fences are posted
	// on all registered queues. The managed space takes a reference to the queue and releases
	// it on detach.
	void attachQueue(EvictionQueue *queue);
	void detachQueue(EvictionQueue *queue);

	coroutine<void> _runReclaimLoop();
	coroutine<void> _runDrainLoop();
	coroutine<void> _runInvalidationLoop();

	// Post the given fence on every attached queue and await all acknowledgements.
	coroutine<void> _fenceEphemeral();
	coroutine<void> _fenceDirty();
	coroutine<void> _fenceAll(EvictMode mode);

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

	// Requests the initialization of a missing page via the manage protocol.
	// Fails on fetchDisallowBacking.
	// Returns the monitor that is raised once the page is initialized.
	// Must be called under mutex.
	// The caller must raise the appended management requests before awaiting the monitor.
	// Precondition: on a SwapSpace, the page has a valid disk copy (i.e., page->swapCopyValid is true).
	std::expected<frg::intrusive_shared_ptr<TransactionMonitor, Allocator>, Error>
	initializePage(ManagedPage *page, FetchFlags flags, ManageList &pendingManagement);

	void submitManagement(ManageNode *node);
	void _progressManagement(ManageList &pending);

	smarter::borrowed_ptr<ManagedSpace> selfPtr;

	smarter::shared_ptr<Hierarchy> hierarchy;

	frg::ticket_spinlock mutex;

	frg::rcu_radixtree<ManagedPage, KernelAlloc, RcuPolicy> pages;

	size_t numPages;
	bool readahead;

	// Whether this space is a SwapSpace.
	bool isSwapSpace = false;

	// Delay before dirty pages enter writeback. Longer delays allow for more coalescing.
	uint64_t writebackDelayNanos = 200'000'000;

	// Queue that BackingMemory/FrontalMemory mappings observe.
	frg::intrusive_shared_ptr<EvictionQueue, Allocator> _evictQueue;

	// Queues of all views whose mappings must observe this space's fences (always
	// including _evictQueue). Protected by mutex.
	frg::intrusive_list<
		EvictionQueue,
		frg::locate_member<
			EvictionQueue,
			frg::default_list_hook<EvictionQueue>,
			&EvictionQueue::attachHook
		>
	> _attachedQueues;

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

	ManageList _managementQueue;

	async::recurring_event _dirtyEvent;

	async::recurring_event _discardEvent;

	// Set by the drain coroutine when dirty pages exist but none of them could claim swap budget.
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

// Backing store for swappable anonymous memory].
// Pages are keyed by swap offset, the kernel allocates offsets lazily on behalf of the attached views.
struct SwapSpace final : ManagedSpace, RcuProtected {
	static std::expected<smarter::shared_ptr<SwapSpace>, Error> create(
			smarter::shared_ptr<Hierarchy> hierarchy);

	SwapSpace(smarter::shared_ptr<Hierarchy> hierarchy);

	bool claimSwapBudget(ManagedPage *page) override;
	void _pageDiscarded(ManagedPage *page, bool &raiseDirty) override;

	void setBudget(size_t numSlots);

	// Allocates a swap page (at the lowest free swap offset) without a phyiscal page frame.
	// Returns null if the swap space is exhausted.
	// The page is fresh (missing, unlocked, not discarded, no disk copy, i.e., fit for installPage())
	// and owned by the caller, who must eventually discard it.
	// Must be called under mutex.
	ManagedPage *allocatePage();

private:
	// Allocates the lowest free swap offset (in pages, not bytes).
	// Must be called under mutex.
	frg::optional<uint64_t> _allocateOffset();

	// Frees the given swap offset (in pages not bytes).
	// Must be called under mutex.
	void _freeOffset(uint64_t offset);

	frg::vector<int8_t, KernelAlloc> _buddyMetadata;
	BuddyAccessor _buddyAccessor;

	// Protected by mutex.
	size_t _budget = 0;

	// Protected by mutex.
	size_t _budgetClaimed = 0;
};
static_assert(HasDispose<SwapSpace>);

// Static size of BackingMemory views.
// It also bounds the ManagedSpace size that is visible to FrontalMemory
// (otherwise, some pages would be unreachable through the backing view).
inline constexpr size_t backingMemoryLength = size_t{1} << 62;

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
// The view translates its own page indices to lazily allocated swap offsets.
// Frames and per-page state are owned by the SwapSpace.
// The view's hierarchy is charged for the swap slots, the SwapSpace's hierarchy for the frames.
struct SwappableMemory final : MemoryView {
private:
	struct CtorToken {};

public:
	static std::expected<smarter::shared_ptr<SwappableMemory>, Error> create(
			smarter::shared_ptr<Hierarchy> hierarchy, smarter::shared_ptr<SwapSpace> space,
			size_t length);

	SwappableMemory(CtorToken, smarter::shared_ptr<Hierarchy> hierarchy,
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
	// Returns null if the swap space is exhausted.
	// Must be called under the SwapSpace mutex.
	ManagedSpace::ManagedPage *_translate(uint64_t index);

	// Unlock counterpart of lockRange().
	// Must be called under the SwapSpace mutex.
	void _unlockPagesLocked(uintptr_t offset, size_t size, bool &raiseDiscard);

	smarter::shared_ptr<Hierarchy> _hierarchy;
	smarter::shared_ptr<SwapSpace> _space;
	size_t _length;

	// Maps page index -> swap page. The swap pages are owned by _space.
	frg::rcu_radixtree<ManagedSpace::ManagedPage *, KernelAlloc, RcuPolicy> _table;
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
			size{size}, flags{flags}, observer{} { }

		// Offsets of the first and last byte of this slot within the IndirectMemory.
		uintptr_t first() const { return uintptr_t{slot} << 32; }
		uintptr_t last() const { return first() + size - 1; }

		IndirectMemory *owner;
		size_t slot;
		smarter::shared_ptr<MemoryView> memory;
		uintptr_t offset;
		size_t size;
		CachingFlags flags;
		MemoryObserver observer;
		smarter::weak_ptr<IndirectionSlot> selfPtr;
	};

	// Lock-free lookup of the slot that contains the given offset.
	smarter::shared_ptr<IndirectionSlot> resolve_(uintptr_t offset);

	// Drops the reference of slots_ after the slot was erased from slots_.
	// Must not be called with mutex_ held.
	void releaseSlot_(IndirectionSlot *indirection);

	size_t numSlots_;
	// Only protects writers to slots_; readers use RCU.
	frg::ticket_spinlock mutex_;
	// Slots are keyed by last(), such that lower_bound() finds the slot containing an offset.
	// Each slot in this tree holds a reference; slots are freed via RCU (allocate_rcu_shared()).
	frg::rcu_btree<uintptr_t, IndirectionSlot *, KernelAlloc, RcuPolicy> slots_;
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
	// Callers must hold _mutex.
	void chargePages_(size_t n);
	// Callers must hold _mutex.
	void unchargePages_(size_t n);

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
