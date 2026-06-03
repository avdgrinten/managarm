#pragma once

#include <atomic>
#include <cstddef>
#include <expected>

#include <async/algorithm.hpp>
#include <async/oneshot-event.hpp>
#include <async/post-ack.hpp>
#include <async/recurring-event.hpp>
#include <frg/list.hpp>
#include <frg/rcu_radixtree.hpp>
#include <frg/shared_ptr.hpp>
#include <frg/vector.hpp>
#include <frg/expected.hpp>
#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/futex.hpp>
#include <thor-internal/hierarchy.hpp>
#include <thor-internal/types.hpp>
#include <thor-internal/pfn-db.hpp>
#include <thor-internal/rcu.hpp>

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

enum class EvictMode {
	none,
	// Evicts all pages in a range.
	breakRange,
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

struct EvictionQueue {
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
	auto fenceEphemeral() {
		return mechanism_.post(RangeToEvict{EvictMode::fenceEphemeral, 0, 0});
	}
	auto fenceDirty() {
		return mechanism_.post(RangeToEvict{EvictMode::fenceDirty, 0, 0});
	}

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

// View on some pages of memory. This is the "frontend" part of a memory object.
struct MemoryView {
protected:
	MemoryView(EvictionQueue *associatedEvictionQueue = nullptr)
	: associatedEvictionQueue_{associatedEvictionQueue} { }

	~MemoryView() = default;

public:
	// Add/remove memory observers. These will be notified of page evictions.
	void addObserver(MemoryObserver *observer) {
		if(associatedEvictionQueue_)
			associatedEvictionQueue_->addObserver(observer);
	}

	void removeObserver(MemoryObserver *observer) {
		if(associatedEvictionQueue_)
			associatedEvictionQueue_->removeObserver(observer);
	}

	// Returns the current size of the memory object.
	// At time of mapping, a new mapping must contained within the memory object's size.
	// However, the memory object's size may change at any time (even when mappings exist).
	virtual size_t getLength() = 0;

	virtual coroutine<frg::expected<Error>> resize(size_t newLength);

	virtual coroutine<frg::expected<Error, smarter::shared_ptr<MemoryView>>> fork();

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

	virtual coroutine<frg::expected<Error, MemoryNotification>> pollNotification();

	// Called (e.g. by user space) to update a range after loading or writeback.
	virtual Error updateRange(ManageRequest type, size_t offset, size_t length);

	virtual coroutine<frg::expected<Error>> writebackFence(uintptr_t offset, size_t size);

	virtual coroutine<frg::expected<Error>> invalidateRange(uintptr_t offset, size_t size);

	virtual Error setIndirection(size_t slot, smarter::shared_ptr<MemoryView> view,
			uintptr_t offset, size_t size, CachingFlags flags);

	coroutine<frg::expected<Error>>
	touchFullRange(uintptr_t offset, size_t size, FetchFlags flags);

	// ----------------------------------------------------------------------------------
	// Memory eviction.
	// ----------------------------------------------------------------------------------

	bool canEvictMemory() {
		return associatedEvictionQueue_;
	}

	auto pollEviction(MemoryObserver *observer, async::cancellation_token ct) {
		return async::transform(observer->agent_.poll(std::move(ct)),
			[] (async::post_ack_handle<RangeToEvict> handle) {
				return Eviction{std::move(handle)};
			}
		);
	}

private:
	EvictionQueue *associatedEvictionQueue_;
};

struct SliceRange {
	MemoryView *view;
	uintptr_t displacement;
	size_t size;
};

struct MemorySlice {
	MemorySlice(smarter::shared_ptr<MemoryView> view,
			ptrdiff_t view_offset, size_t view_size, CachingFlags cachingFlags = 0);

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
	static std::expected<smarter::shared_ptr<ImmediateMemory>, Error>
	create(size_t length);

	ImmediateMemory();
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
	HardwareMemory(PhysicalAddr base, size_t length, CachingMode cache_mode);
	HardwareMemory(const HardwareMemory &) = delete;
	~HardwareMemory();

	HardwareMemory &operator= (const HardwareMemory &) = delete;

	size_t getLength() override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	PhysicalRange peekRange(uintptr_t offset, FetchFlags flags) override;
	coroutine<frg::expected<Error, size_t>>
			touchRange(uintptr_t offset, size_t sizeHint, FetchFlags flags) override;

private:
	PhysicalAddr _base;
	size_t _length;
	CachingMode _cacheMode;
};

struct AllocatedMemory final : MemoryView {
	AllocatedMemory(smarter::shared_ptr<Hierarchy> hierarchy, size_t length,
			int addressBits = 64,
			size_t chunkSize = kPageSize, size_t chunkAlign = kPageSize);
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
		// Page is in _dirtyList (or the draining coroutine's local pending list),
		// waiting for a fenceDirty() to complete before being promoted to _writebackList.
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
		// Page has been selected for reclaimation and is awaiting fenceEphemeral().
		// Page is owned by the ManagedSpace reclamation logic.
		// Valid in LoadState::present.
		performReclaim,
		// Page will not be reclaimed but is still awaiting fenceEphemeral().
		// Page is owned by the ManagedSpace reclamation logic.
		// Valid in LoadState::present.
		avertReclaim,
	};

	// Struct that is attached to ManagedPage for the duration of
	// a single transaction (i.e., initialization, writeback, or forced eviction).
	// For initialization:
	// * Attached when entering TxState::wantInitialization.
	// * Completed when leaving TxState::initialization.
	// For writeback:
	// * Attached when entering TxState::wantWriteback.
	// * Completed when leaving TxState::writeback.
	// For forced eviction (invalidateRange() on a page already in TxState::performReclaim):
	// * Attached by invalidateRange() while the page is in TxState::performReclaim.
	// * Completed by the eviction coroutine after transitioning to LoadState::missing.
	struct TransactionMonitor final : frg::intrusive_rc {
		async::oneshot_primitive event;
		frg::default_list_hook<TransactionMonitor> pendingHook;
	};

	struct ManagedPage {
		ManagedPage(ManagedSpace *bundle, uint64_t identity) {
			cachePage.bundle = bundle;
			cachePage.identity = identity;
		}

		ManagedPage(const ManagedPage &) = delete;

		ManagedPage &operator= (const ManagedPage &) = delete;

		PhysicalAddr physical = PhysicalAddr(-1);
		LoadState loadState{LoadState::missing};
		TxState transactionState{TxState::none};
		// Whether the page is dirty even after a pending writeback completes.
		// Can only be true in LoadState::present and TxState::writeback.
		// If set in TxState::performReclaim or TxState::avertReclaim, causes transition to dirty.
		bool stillDirty{false};
		// Set by invalidateRange() when the page is already in TxState::performReclaim,
		// to request that the eviction coroutine raise monitor after completing
		// the transition to LoadState::missing.
		bool forceInvalidation{false};
		unsigned int lockCount = 0;
		CachePage cachePage;
		frg::intrusive_shared_ptr<TransactionMonitor, Allocator> monitor;
	};

	ManagedSpace(smarter::shared_ptr<Hierarchy> hierarchy, size_t length, bool readahead);
	~ManagedSpace();

	void incrementUses(CachePage *page) override;
	void decrementUses(CachePage *page) override;
	void markDirty(CachePage *page) override;

	Error lockPages(uintptr_t offset, size_t size);
	void unlockPages(uintptr_t offset, size_t size);

	void submitManagement(ManageNode *node);
	void _progressManagement(ManageList &pending);

	smarter::borrowed_ptr<ManagedSpace> selfPtr;

	smarter::shared_ptr<Hierarchy> hierarchy;

	frg::ticket_spinlock mutex;

	frg::rcu_radixtree<ManagedPage, KernelAlloc, RcuPolicy> pages;

	size_t numPages;
	bool readahead;

	EvictionQueue _evictQueue;

	frg::intrusive_list<
		CachePage,
		frg::locate_member<
			CachePage,
			frg::default_list_hook<CachePage>,
			&CachePage::listHook
		>
	> _dirtyList;

	frg::intrusive_list<
		CachePage,
		frg::locate_member<
			CachePage,
			frg::default_list_hook<CachePage>,
			&CachePage::listHook
		>
	> _initializationList;

	frg::intrusive_list<
		CachePage,
		frg::locate_member<
			CachePage,
			frg::default_list_hook<CachePage>,
			&CachePage::listHook
		>
	> _writebackList;

	ManageList _managementQueue;

	async::recurring_event _dirtyEvent;
};

struct BackingMemory final : MemoryView {
public:
	BackingMemory(smarter::shared_ptr<ManagedSpace> managed)
	: MemoryView{&managed->_evictQueue}, _managed{std::move(managed)} { }

	BackingMemory(const BackingMemory &) = delete;

	BackingMemory &operator= (const BackingMemory &) = delete;

	size_t getLength() override;
	coroutine<frg::expected<Error>> resize(size_t newLength) override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	PhysicalRange peekRange(uintptr_t offset, FetchFlags flags) override;
	coroutine<frg::expected<Error, size_t>>
			touchRange(uintptr_t offset, size_t sizeHint, FetchFlags flags) override;
	coroutine<frg::expected<Error, MemoryNotification>> pollNotification() override;
	Error updateRange(ManageRequest type, size_t offset, size_t length) override;
	coroutine<frg::expected<Error>> writebackFence(uintptr_t offset, size_t size) override;
	coroutine<frg::expected<Error>> invalidateRange(uintptr_t offset, size_t size) override;

private:
	smarter::shared_ptr<ManagedSpace> _managed;
};

struct FrontalMemory final : MemoryView {
public:
	FrontalMemory(smarter::shared_ptr<ManagedSpace> managed)
	: MemoryView{&managed->_evictQueue}, _managed{std::move(managed)} { }

	FrontalMemory(const FrontalMemory &) = delete;

	FrontalMemory &operator= (const FrontalMemory &) = delete;

	size_t getLength() override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	PhysicalRange peekRange(uintptr_t offset, FetchFlags flags) override;
	coroutine<frg::expected<Error, size_t>>
			touchRange(uintptr_t offset, size_t sizeHint, FetchFlags flags) override;

public:
	// Contract: set by the code that constructs this object.
	smarter::borrowed_ptr<FrontalMemory> selfPtr;
private:
	smarter::shared_ptr<ManagedSpace> _managed;
};

struct IndirectMemory final : MemoryView {
	IndirectMemory(size_t numSlots);
	IndirectMemory(const IndirectMemory &) = delete;
	~IndirectMemory();

	IndirectMemory &operator= (const IndirectMemory &) = delete;

	size_t getLength() override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	PhysicalRange peekRange(uintptr_t offset, FetchFlags flags) override;
	coroutine<frg::expected<Error, size_t>>
			touchRange(uintptr_t offset, size_t sizeHint, FetchFlags flags) override;

	Error setIndirection(size_t slot, smarter::shared_ptr<MemoryView> memory,
			uintptr_t offset, size_t size, CachingFlags flags) override;

private:
	struct IndirectionSlot {
		IndirectionSlot(IndirectMemory *owner, size_t slot,
				smarter::shared_ptr<MemoryView> memory,
				uintptr_t offset, size_t size, CachingFlags flags)
		: owner{owner}, slot{slot}, memory{std::move(memory)}, offset{offset},
			size{size}, flags{flags}, observer{} { }

		IndirectMemory *owner;
		size_t slot;
		smarter::shared_ptr<MemoryView> memory;
		uintptr_t offset;
		size_t size;
		CachingFlags flags;
		MemoryObserver observer;
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

	PhysicalAddr physical = -1;
	CowState state = CowState::null;
	unsigned int lockCount = 0;
	// Non-owning; the owning CopyOnWriteMemory/CowChain keeps the Hierarchy alive.
	Hierarchy *hierarchy = nullptr;
};

struct CowChain {
	CowChain();

	~CowChain();

// TODO: Either this private again or make this class POD-like.
	frg::ticket_spinlock _mutex;

	frg::rcu_radixtree<smarter::shared_ptr<CowPage>, KernelAlloc, RcuPolicy> _pages;
};

struct CopyOnWriteMemory final : MemoryView /*, MemoryObserver */ {
public:
	CopyOnWriteMemory(smarter::shared_ptr<Hierarchy> hierarchy,
			smarter::shared_ptr<MemoryView> view,
			uintptr_t offset, size_t length,
			smarter::shared_ptr<CowChain> chain = nullptr);
	CopyOnWriteMemory(const CopyOnWriteMemory &) = delete;

	~CopyOnWriteMemory();

	CopyOnWriteMemory &operator= (const CopyOnWriteMemory &) = delete;

	size_t getLength() override;
	coroutine<frg::expected<Error, smarter::shared_ptr<MemoryView>>> fork() override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	PhysicalRange peekRange(uintptr_t offset, FetchFlags flags) override;
	coroutine<frg::expected<Error, size_t>>
			touchRange(uintptr_t offset, size_t sizeHint, FetchFlags flags) override;

public:
	// Contract: set by the code that constructs this object.
	smarter::borrowed_ptr<CopyOnWriteMemory> selfPtr;
private:
	frg::ticket_spinlock _mutex;

	smarter::shared_ptr<Hierarchy> _hierarchy;
	smarter::shared_ptr<MemoryView> _view;
	uintptr_t _viewOffset;
	size_t _length;
	smarter::shared_ptr<CowChain> _copyChain;
	frg::rcu_radixtree<smarter::shared_ptr<CowPage>, KernelAlloc, RcuPolicy> _ownedPages;
	async::recurring_event _copyEvent;
	EvictionQueue _evictQueue;
};

FutexRealm *getGlobalFutexRealm();

} // namespace thor
