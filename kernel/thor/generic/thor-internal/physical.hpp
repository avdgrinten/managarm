#pragma once

#include <atomic>
#include <expected>
#include <new>

#include <async/algorithm.hpp>
#include <async/cancellation.hpp>
#include <async/oneshot-event.hpp>
#include <eir/interface.hpp>
#include <frg/list.hpp>
#include <frg/optional.hpp>
#include <frg/rbtree.hpp>
#include <frg/spinlock.hpp>
#include <frg/manual_box.hpp>
#include <physical-buddy.hpp>
#include <thor-internal/arch-generic/paging-consts.hpp>
#include <thor-internal/elf-notes.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/mm-base.hpp>
#include <thor-internal/types.hpp>

namespace thor {

extern ManagarmElfNote<MemoryLayout> memoryLayoutNote;
extern ManagarmElfNote<PhysicalMemory> physicalMemoryNote;

inline uintptr_t directPhysicalOffset() {
	return memoryLayoutNote->directPhysical;
}

inline void *mapDirectPhysical(PhysicalAddr physical) {
	assert(physical < 0x4000'0000'0000);
	return reinterpret_cast<void *>(directPhysicalOffset() + physical);
}

inline PhysicalAddr reverseDirectPhysical(void *pointer) {
	return reinterpret_cast<uintptr_t>(pointer) - directPhysicalOffset();
}

// List of unused physical pages; each page stores the address of the next one at offset 0.
// Does not own the pages: they must be freed or handed on before the chain is destroyed.
struct PageChain {
	friend void swap(PageChain &a, PageChain &b) {
		using std::swap;
		swap(a.head_, b.head_);
		swap(a.tail_, b.tail_);
		swap(a.size_, b.size_);
	}

	PageChain() = default;

	PageChain(const PageChain &) = delete;

	PageChain(PageChain &&other)
	: PageChain{} {
		swap(*this, other);
	}

	~PageChain() {
		assert(!size_);
	}

	PageChain &operator= (PageChain other) {
		swap(*this, other);
		return *this;
	}

	bool empty() {
		return !size_;
	}

	size_t size() {
		return size_;
	}

	void push(PhysicalAddr physical) {
		new (mapDirectPhysical(physical)) PhysicalAddr{head_};
		if(!size_)
			tail_ = physical;
		head_ = physical;
		size_++;
	}

	PhysicalAddr pop() {
		assert(size_);
		auto physical = head_;
		head_ = *reinterpret_cast<PhysicalAddr *>(mapDirectPhysical(physical));
		if(!--size_)
			tail_ = static_cast<PhysicalAddr>(-1);
		return physical;
	}

	// Moves the pages of other to the front of this chain.
	void splice(PageChain other) {
		if(!other.size_)
			return;
		*reinterpret_cast<PhysicalAddr *>(mapDirectPhysical(other.tail_)) = head_;
		if(!size_)
			tail_ = other.tail_;
		head_ = other.head_;
		size_ += other.size_;
		other.head_ = static_cast<PhysicalAddr>(-1);
		other.tail_ = static_cast<PhysicalAddr>(-1);
		other.size_ = 0;
	}

private:
	PhysicalAddr head_ = static_cast<PhysicalAddr>(-1);
	PhysicalAddr tail_ = static_cast<PhysicalAddr>(-1);
	size_t size_ = 0;
};

// Determines the caching mode with which the given physical range may be mapped,
// based on the firmware memory map. Ranges that the firmware memory map does not
// describe are assumed to be device memory. Returns null if parts of the range
// require different caching modes (mixing them in one mapping is not meaningful).
frg::optional<CachingMode> determineFirmwareCachingMode(PhysicalAddr physical, size_t size);

struct PageAccessor {
	friend void swap(PageAccessor &a, PageAccessor &b) {
		using std::swap;
		swap(a._pointer, b._pointer);
	}

	PageAccessor()
	: _pointer{nullptr} { }

	PageAccessor(PhysicalAddr physical) {
		assert(physical != PhysicalAddr(-1) && "trying to access invalid physical page");
		assert(!(physical & 0xFFF) && "physical page is not aligned");
		assert(physical < 0x4000'0000'0000);
		_pointer = reinterpret_cast<void *>(directPhysicalOffset() + physical);
	}

	PageAccessor(const PageAccessor &) = delete;

	PageAccessor(PageAccessor &&other)
	: PageAccessor{} {
		swap(*this, other);
	}

	~PageAccessor() { }

	PageAccessor &operator= (PageAccessor other) {
		swap(*this, other);
		return *this;
	}

	explicit operator bool () {
		return _pointer;
	}

	void *get() {
		return _pointer;
	}

private:
	void *_pointer;
};

struct PhysicalWindow {
	PhysicalWindow() = default;
	PhysicalWindow(PhysicalAddr physical, size_t size, CachingMode caching = CachingMode::null);
	~PhysicalWindow();

	PhysicalWindow(const PhysicalWindow &) = delete;

	PhysicalWindow(PhysicalWindow &&other)
	: PhysicalWindow() {
		swap(*this, other);
	}

	friend void swap(PhysicalWindow &x, PhysicalWindow &y) {
		using std::swap;
		swap(x.window_, y.window_);
		swap(x.pages_, y.pages_);
		swap(x.size_, y.size_);
	}

	PhysicalWindow &operator= (PhysicalWindow other) {
		swap(*this, other);
		return *this;
	}

	explicit operator bool () {
		return window_;
	}

	void *get() {
		return window_;
	}

private:
	void *window_ = nullptr;
	size_t pages_ = 0;
	size_t size_ = 0;
};

// Functions for debugging kernel page access:
// Deny all access to the physical mapping.
void poisonPhysicalAccess(PhysicalAddr physical);
// Deny write access to the physical mapping.
void poisonPhysicalWriteAccess(PhysicalAddr physical);


// What an allocation is used for. Decides which buddy roots may serve it.
enum class PhysicalKind {
	// Frames that can be evicted and moved to different physical pages (e.g., ManagedSpace pages).
	evictable,
	// Frames that are never evicted or moved (e.g., kernel data structures).
	pinned,
	// Frames of multi-page contiguous allocations. Like pinned, never evicted or moved.
	contiguous,
};

// Class of a buddy root, i.e., the PhysicalKinds that it admits.
// Roots can be converted from evictableOnly to one of the other classes.
// However, we forbid transitions between the other classes;
// for example, contiguousReserve -> admitsPinned is forbidden
// to make it easier to satisfy contiguous allocations.
enum class RootClass : uint8_t {
	// Can only satisfy evictable allocations.
	evictableOnly,
	// Can only satisfy contiguous allocations.
	// May hold evictable frames that were allocated prior to conversion.
	contiguousReserve,
	// Can satisfy evictable, contiguous and pinned allocations.
	admitsPinned,
};

// Whether the current context may block for memory allocations.
// True below ipl::noSchedule if ExecutorContext::mayBlockForMemory is set,
// i.e., for threads and the general WQ fiber.
bool canBlockForMemory();

// Floor (see AllocationPolicy::floor) of allocations that the current context blocks for.
// Zero if the context cannot block (see canBlockForMemory()).
size_t currentContextFloor();

// Wakes the reclaimer since PhysicalChunkAllocator::pressureShortfall() may have become non-zero.
// Implemented by the reclaimer. Callers must not hold the physical allocator's lock.
void handleReclaimPressure();

class PhysicalChunkAllocator {
	typedef frg::ticket_spinlock Mutex;

	struct WaitNode {
		WaitNode(AllocationPolicy policy, PhysicalKind kind, size_t numPages)
		: cls{policy.cls}, kind{kind}, numPages{numPages}, floor{policy.floor} { }

		WaitNode(const WaitNode &) = delete;

		WaitNode &operator= (const WaitNode &) = delete;

		AllocationClass cls;
		PhysicalKind kind;
		size_t numPages;
		// Starts out as AllocationPolicy::floor. startWait_() adds the floor of cls.
		size_t floor;
		// Arrival order among nodes of equal floor. Set by startWait_().
		uint64_t seq = 0;
		// Protected by _mutex.
		bool queued = false;
		// Hooks the node into waitTree_. Protected by _mutex.
		frg::rbtree_hook treeHook;
		// Hooks the node into a WaitList of served nodes. Protected by _mutex.
		frg::default_list_hook<WaitNode> hook;
		// Written by the code path that dequeues the node.
		// Protected by _mutex.
		std::expected<PageChain, Error> result{std::unexpected{Error::noMemory}};
		// Raised when the wait completes.
		async::oneshot_primitive event;
	};

	using WaitList = frg::intrusive_list<
		WaitNode,
		frg::locate_member<
			WaitNode,
			frg::default_list_hook<WaitNode>,
			&WaitNode::hook
		>
	>;

	struct WaitNodeLess {
		bool operator() (const WaitNode &a, const WaitNode &b) {
			if(a.floor != b.floor)
				return a.floor < b.floor;
			return a.seq < b.seq;
		}
	};

	// The first node owns all pinnable pages above its floor until it is served.
	using WaitTree = frg::rbtree<WaitNode, &WaitNode::treeHook, WaitNodeLess>;

	// We call an allocation fungible if it consists only of single (i.e., non-contiguous) pages
	// with no additional address restrictions.
	// Callers can block for fungible allocations.
	static bool isFungibleAllocation_(size_t size, PhysicalKind kind, int addressBits) {
		return size == kPageSize && addressBits >= 64 && kind != PhysicalKind::contiguous;
	}

public:
	// Guaranteed by BuddyAccessor::suitableOrder().
	static constexpr size_t maxRootsPerRegion = 64;
	static constexpr size_t numRootClasses = 3;

	PhysicalChunkAllocator();

	void bootstrapRegion(PhysicalAddr address,
			int order, size_t numRoots, int8_t *buddyTree);
	// Assigns the initial class of each root: roots that Eir allocated from admit pinned frames,
	// the lowest free roots that cover reserveBytes admit contiguous frames, all others are
	// evictableOnly.
	// Must be called after bootstrapRegion() and before the first allocation.
	void assignRootClasses(size_t reserveBytes);

	// Allocates a naturally aligned power-of-two sized chunk.
	std::expected<PhysicalAddr, Error> tryAllocate(AllocationPolicy policy, size_t size,
			PhysicalKind kind, int addressBits = 64);

	// Asynchronously performs a fungible allocation.
	// Completes with Error::cancelled on cancellation.
	// Precondition: isFungibleAllocation_() is true.
	auto allocateAwaitable(AllocationPolicy policy, PhysicalKind kind,
			async::cancellation_token ct) {
		return async::let(
			[=] { return WaitNode{policy, kind, 1}; },
			[this, ct] (WaitNode &node) {
				startWait_(&node);
				return async::with_cancel_cb(
					async::transform(
						node.event.wait(),
						[&node] -> std::expected<PhysicalAddr, Error> {
							if(!node.result)
								return std::unexpected{node.result.error()};
							return node.result->pop();
						}
					),
					[this, &node] {
						cancelWait_(&node);
					},
					ct
				);
			}
		);
	}

	// Like allocateAwaitable() but blocks.
	// Cancelled on thread termination (returning Error::cancelled); fibers are never cancelled.
	std::expected<PhysicalAddr, Error> allocateBlocking(AllocationPolicy policy,
			PhysicalKind kind);

	// Like tryAllocate() but for chains of fungible pages.
	std::expected<PageChain, Error> tryAllocateChain(AllocationPolicy policy, PhysicalKind kind,
			size_t numPages);

	// Like allocateAwaitable() but for chains of fungible pages.
	auto allocateChainAwaitable(AllocationPolicy policy, PhysicalKind kind, size_t numPages,
			async::cancellation_token ct) {
		return async::let(
			[=] { return WaitNode{policy, kind, numPages}; },
			[this, ct] (WaitNode &node) {
				startWait_(&node);
				return async::with_cancel_cb(
					async::transform(
						node.event.wait(),
						[&node] { return std::move(node.result); }
					),
					[this, &node] {
						cancelWait_(&node);
					},
					ct
				);
			}
		);
	}

	// Like allocateBlocking() but for chains of fungible pages.
	std::expected<PageChain, Error> allocateChainBlocking(AllocationPolicy policy,
			PhysicalKind kind, size_t numPages);

	// Allocates a naturally aligned power-of-two sized chunk in the kernel class at the current context's floor.
	// Blocks for fungible pages if possible (i.e., if canBlockForMemory() is true).
	// Falls back to the emergency class.
	std::expected<PhysicalAddr, Error> allocate(size_t size, PhysicalKind kind,
			int addressBits = 64);

	// Like allocate() for PhysicalKind::pinned but returns -1 on failure.
	PhysicalAddr allocate(size_t size, int addressBits = 64);

	// Like allocate() but for a single fungible page; never fails.
	// If the emergency class is exhausted, contexts that can block wait for memory
	// (even if the thread is terminated) while other contexts panic.
	PhysicalAddr allocateInfallible(PhysicalKind kind);

	// kind must match the kind that the chunk was allocated with.
	void free(PhysicalAddr address, size_t size, PhysicalKind kind);

	// Like free() for PhysicalKind::pinned.
	void free(PhysicalAddr address, size_t size) {
		free(address, size, PhysicalKind::pinned);
	}

	size_t numTotalPages() {
		return _totalPages.load(std::memory_order_relaxed);
	}
	size_t numUsedPages() {
		return _usedPages.load(std::memory_order_relaxed);
	}
	size_t numFreePages() {
		return _freePages.load(std::memory_order_relaxed);
	}

	// Pinnable pages that the reclaimer should free: an allocation left less than
	// reclaimResumeHeadroom_ above its floor, and the resulting reclaim target is not met yet.
	size_t pressureShortfall();

private:
	void updateFloors_();

	// Called by allocateAwaitable() to start the wait.
	void startWait_(WaitNode *node);
	// Called by allocateAwaitable() to cancel the wait.
	void cancelWait_(WaitNode *node);

	// Callers must hold _mutex.
	size_t floorFor_(AllocationClass cls);

	// Called for allocations of all classes (successful or not).
	// Raises reclaimTarget_ if the allocation left less than reclaimResumeHeadroom_ above its floor.
	// Sets kickReclaim to true if handleReclaimPressure() needs to be called after dropping _mutex (and doesn't modify it otherwise).
	// Callers must hold _mutex.
	void noteAllocation_(AllocationPolicy policy, bool &kickReclaim);

	// Clears reclaimTarget_ if it is met, such that noteAllocation_() can raise a new target.
	// Callers must hold _mutex.
	void updateReclaimTarget_();

	// Floor that an allocation with the given floor must respect.
	// Allocations must not overtake a waiter with an equal or lower floor, so they get no
	// pinnable pages at all while such a waiter exists.
	// Callers must hold _mutex.
	size_t effectiveFloor_(size_t floor);

	// Free pages outside of contiguousReserve roots. Every kind can use these pages,
	// so floors are measured in them.
	// Callers must hold _mutex.
	size_t pinnablePages_();

	// Pinnable pages beyond the floor, i.e., the ones that allocateChunk_() may take.
	// floor is the number of pinnable pages that must stay free.
	// Callers must hold _mutex.
	size_t pinnableBudget_(size_t floor);

	// Whether an allocation of numPages fungible pages will succeed or not.
	// Exact: if this returns true, then allocateFungible_() must succeed.
	// Callers must hold _mutex.
	bool canAllocateFungible_(size_t numPages, size_t floor);

	// Allocate numPages fungible pages.
	// Callers must hold _mutex.
	PageChain allocateFungible_(PhysicalKind kind, size_t numPages, size_t floor);

	// Never takes pinnable pages that the floor holds back.
	// Callers must hold _mutex.
	PhysicalAddr allocateChunk_(size_t size, PhysicalKind kind, int addressBits, size_t floor);

	// Callers must hold _mutex.
	void freeChunk_(PhysicalAddr address, size_t size, PhysicalKind kind);

	// Completes waiting allocations in the order of waitTree_ until the first node
	// cannot be served.
	// Callers must hold _mutex.
	void serveWaiters_(WaitList &served);

	// Must be called without holding _mutex.
	static void completeServed_(WaitList &served);

	// Logs the state of the allocator and panics.
	// Must be called without holding _mutex.
	[[noreturn]] void panicOutOfMemory_(PhysicalKind kind, void *caller);

	struct Region {
		PhysicalAddr physicalBase;
		PhysicalAddr regionSize;
		BuddyAccessor buddyAccessor;
		// Protected by _mutex.
		RootClass rootClasses[maxRootsPerRegion];
		// Protected by _mutex.
		uint32_t usedPagesOfRoot[maxRootsPerRegion];
		// Pages of PhysicalKind::pinned and PhysicalKind::contiguous. Protected by _mutex.
		uint32_t unevictablePagesOfRoot[maxRootsPerRegion];

		size_t pagesPerRoot() {
			return size_t{1} << buddyAccessor.tableOrder();
		}
		size_t rootOfAddress(PhysicalAddr address) {
			return (address - physicalBase) >> (buddyAccessor.tableOrder() + kPageShift);
		}
	};

	struct Allocation {
		Region *region;
		PhysicalAddr physical;
	};

	// Allocates from the roots that satisfy rootFilter(region, rootIndex).
	// Prefers regions above 4 GiB so that low memory stays available for
	// allocations that are constrained to a limited address width (e.g., 32-bit DMA).
	// Callers must hold _mutex.
	template<typename F>
	frg::optional<Allocation> search_(int order, int addressBits, F rootFilter);

	// Allocate from roots of a specific class.
	// Callers must hold _mutex.
	frg::optional<Allocation> allocateInClass_(int order, int addressBits, RootClass cls);

	// Allocates from an evictableOnly root and converts that root to the given class.
	// Takes at most budget pinnable pages.
	// Callers must hold _mutex.
	frg::optional<Allocation> allocateByConverting_(int order, int addressBits, RootClass to,
			size_t pinnableBudget);

	// Callers must hold _mutex.
	void convertRoot_(Region &region, size_t rootIndex, RootClass to);

	// Callers must hold _mutex.
	void accountAllocation_(Allocation allocation, size_t numPages, PhysicalKind kind);

	Mutex _mutex;

	Region _allRegions[eirMaxMemoryRegions];
	int _numRegions = 0;
	bool _classesAssigned = false;
	// Indexed by RootClass. Set by assignRootClasses(). Protected by _mutex.
	size_t freePagesOfClass_[numRootClasses] = {};

	// Floors in pages; see AllocationClass. Set by updateFloors_().
	size_t kernelFloor_ = 0;
	size_t userFloor_ = 0;

	// Headroom in pinnable pages above the floor of an allocation at which reclaim
	// starts and stops; see noteAllocation_(). Set by updateFloors_().
	size_t reclaimResumeHeadroom_ = 0;
	size_t reclaimSuspendHeadroom_ = 0;
	// Allocations whose AllocationPolicy::floor exceeds this do not raise the reclaim target.
	// Set by updateFloors_().
	size_t maxAssistedFloor_ = 0;

	// Pinnable pages that the reclaimer works towards, or zero if there is no target.
	// Protected by _mutex.
	size_t reclaimTarget_ = 0;

	// Waiting allocations, served in the order of WaitNodeLess. Protected by _mutex.
	WaitTree waitTree_;
	// Next WaitNode::seq. Protected by _mutex.
	uint64_t nextSeq_ = 0;

	std::atomic<size_t> _totalPages{0};
	std::atomic<size_t> _usedPages{0};
	std::atomic<size_t> _freePages{0};
};

extern constinit frg::manual_box<PhysicalChunkAllocator> physicalAllocator;

// Caches free physical pages that back the kernel's slab allocator.
// Filled in contexts that can potentially block and consumed from the slab allocator (which cannot block).
struct PageMagazine {
	PageMagazine() = default;

	PageMagazine(const PageMagazine &) = delete;

	PageMagazine &operator= (const PageMagazine &) = delete;

	~PageMagazine();

	// Allocates the wanted pages in the kernel class at the current context's floor.
	// Blocks for them if possible (i.e., if canBlockForMemory() is true). Falls back to the emergency class.
	std::expected<void, Error> refill();

	// Like refill() but never fails; see PhysicalChunkAllocator::allocateInfallible().
	void refillInfallible();

	PageChain pages;
	// Number of pages that the last failed map() was missing.
	size_t wanted = 0;
};

} // namespace thor
