#include <array>
#include <bit>

#include <async/scope.hpp>
#include <frg/cmdline.hpp>
#include <frg/scope_exit.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/arch-generic/asid.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/memory-view.hpp>
#include <thor-internal/pfn-db.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/timer.hpp>

namespace thor {

namespace {
	constexpr bool logUsage = false;
	constexpr bool logReclaim = false;
	constexpr bool logUncaching = false;

	// The following flags are debugging options to debug the correctness of various components.
	bool tortureUncaching = false;
	constexpr bool disableUncaching = false;

	// Number of pages that BackingMemory::invalidateRange() discards per critical section.
	constexpr size_t discardChunkSize = 512;

	// Number of page entries that BackingMemory::writebackFence() scans per critical section.
	constexpr size_t fenceChunkSize = 512;

	// Number of page entries that BackingMemory::markDirtyRange() scans per critical section.
	constexpr size_t markDirtyChunkSize = 512;
}

// --------------------------------------------------------
// Frame allocation
// --------------------------------------------------------

namespace {
	// Synchronous callers wait at their own floor; asynchronous ones at the floor of the owner.
	AllocationPolicy userAllocationPolicy(Hierarchy *owner, FetchFlags flags) {
		if(flags & fetchSynchronous)
			return {AllocationClass::user, currentContextFloor()};
		return {AllocationClass::user, owner->memoryFloor()};
	}
}

coroutine<std::expected<PhysicalAddr, Error>>
chargeAndAllocate(Hierarchy *hierarchy, PhysicalKind kind, FetchFlags flags) {
	assert(currentIpl() == ipl::exceptionalWork);

	if(auto charge = hierarchy->tryChargeMemory(kPageSize); !charge)
		co_return std::unexpected{charge.error()};
	auto physical = co_await physicalAllocator->allocateAwaitable(
			userAllocationPolicy(hierarchy, flags), kind, async::cancellation_token{});
	if(!physical) {
		hierarchy->unchargeMemory(kPageSize);
		co_return std::unexpected{physical.error()};
	}
	co_return *physical;
}

std::expected<PhysicalAddr, Error>
tryChargeAndAllocate(Hierarchy *hierarchy, size_t size, PhysicalKind kind, int addressBits,
		FetchFlags flags) {
	if(auto charge = hierarchy->tryChargeMemory(size); !charge)
		return std::unexpected{charge.error()};
	auto physical = physicalAllocator->tryAllocate(
			userAllocationPolicy(hierarchy, flags), size, kind, addressBits);
	if(!physical) {
		hierarchy->unchargeMemory(size);
		return std::unexpected{physical.error()};
	}
	return *physical;
}

void deallocateAndUncharge(Hierarchy *hierarchy, PhysicalAddr physical, size_t size,
		PhysicalKind kind) {
	physicalAllocator->free(physical, size, kind);
	hierarchy->unchargeMemory(size);
}

// --------------------------------------------------------
// EvictionQueue
// --------------------------------------------------------

coroutine<void> EvictionQueue::removeObserver(MemoryObserver *observer) {
	auto n = observer->inflightEvictions_.fetch_or(MemoryObserver::detachedBit,
			std::memory_order_acq_rel);
	assert(!(n & MemoryObserver::detachedBit));
	// The last endEviction_() raises the event; without in-flight evictions nobody does.
	if(n)
		co_await observer->evictionsDrainedEvent_.wait();

	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&mutex_);

	observers_.erase(observer);
}

coroutine<void> EvictionQueue::dispatch_(EvictMode mode, uintptr_t offset, size_t size) {
	assert(currentIpl() == ipl::exceptionalWork);

	auto runEviction = [&] (MemoryObserver *observer) {
		return async::transform(observer->evict(mode, offset, size), [observer] {
			// End the eviction under the RCU guard: the observer may be freed after an RCU grace
			// period as soon as removeObserver() sees the last in-flight eviction end.
			ScheduleGuard rcuGuard;
			observer->endEviction_();
		});
	};

	// Evictions only start once the RCU guard is dropped since evict() must not run under it.
	co_await async::dynamic_deferred_scope<decltype(runEviction(nullptr))>(Allocator{},
			[&] (auto &add) {
		ScheduleGuard rcuGuard;

		for(auto observer : observers_) {
			if(offset + size <= observer->observedOffset_
					|| offset >= observer->observedOffset_ + observer->observedSize_)
				continue;
			// Observers that are being removed refuse to begin an eviction and can be skipped.
			if(!observer->tryBeginEviction_())
				continue;
			add(runEviction(observer));
		}
	});
}

bool EvictionQueue::isObserved(uintptr_t offset, size_t size) {
	ScheduleGuard rcuGuard;

	for(auto observer : observers_) {
		if(offset + size <= observer->observedOffset_
				|| offset >= observer->observedOffset_ + observer->observedSize_)
			continue;
		if(observer->inflightEvictions_.load(std::memory_order_acquire)
				& MemoryObserver::detachedBit)
			continue;
		return true;
	}
	return false;
}

// --------------------------------------------------------
// Reclaim implementation.
// --------------------------------------------------------

struct MemoryReclaimer {
	void registerBundle(const smarter::shared_ptr<CacheBundle> &bundle) {
		bundle->selfPtr_ = bundle;

		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		// Generations are aligned across bundles such that eviction can compare them.
		bundle->newestGen_ = newestGen_;
		// Note that this push_back() publishes the bundle since traversal does not hold mutex_.
		bundleList_.push_back(bundle.get());
	}

	void unregisterBundle(CacheBundle *bundle) {
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		{
			auto pageLock = frg::guard(&bundle->reclaimMutex_);
			for(unsigned int i = 0; i < CacheBundle::numGenerations; i++)
				assert(bundle->genLists_[i].empty());
			assert(bundle->_reclaimList.empty());
		}
		bundleList_.erase(bundle);
	}

	void addPage(CachePage *page) {
		auto *bundle = page->bundle;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&bundle->reclaimMutex_);

			assert(!(page->flags & CachePage::reclaimRegistered));

			page->generation = bundle->newestGen_;
			bundle->genLists_[bundle->newestGen_].push_back(page);
			bundle->genSizes_[bundle->newestGen_]++;
			page->flags |= CachePage::reclaimRegistered;
			numInGenerations_.fetch_add(1, std::memory_order_relaxed);
		}

		rotationTurnaround_.fetch_add(1, std::memory_order_relaxed);
		if (shouldRotate_())
			wakeEvent_.raise();
	}

	void removePage(CachePage *page) {
		auto *bundle = page->bundle;
		bool averted = false;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&bundle->reclaimMutex_);

			assert(page->flags & CachePage::reclaimRegistered);

			if(page->flags & CachePage::reclaimPosted) {
				auto it = bundle->_reclaimList.iterator_to(page);
				bundle->_reclaimList.erase(it);
				page->flags &= ~CachePage::reclaimPosted;
				averted = true;
			}else{
				auto g = generationOf_(bundle, page);
				auto it = bundle->genLists_[g].iterator_to(page);
				bundle->genLists_[g].erase(it);
				bundle->genSizes_[g]--;
				numInGenerations_.fetch_sub(1, std::memory_order_relaxed);
			}
			page->flags &= ~CachePage::reclaimRegistered;
		}

		// The bundle did not take the page via takePosted() yet.
		if(averted)
			resolveOutstanding_(1);
	}

	void bumpPage(CachePage *page) {
		auto *bundle = page->bundle;
		bool averted = false;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&bundle->reclaimMutex_);

			assert(page->flags & CachePage::reclaimRegistered);

			if(page->flags & CachePage::reclaimPosted) {
				auto it = bundle->_reclaimList.iterator_to(page);
				bundle->_reclaimList.erase(it);

				page->flags &= ~CachePage::reclaimPosted;
				page->generation = bundle->newestGen_;
				bundle->genLists_[bundle->newestGen_].push_back(page);
				bundle->genSizes_[bundle->newestGen_]++;
				numInGenerations_.fetch_add(1, std::memory_order_relaxed);
				averted = true;
			}else if(page->generation != bundle->newestGen_) {
				auto g = generationOf_(bundle, page);
				auto it = bundle->genLists_[g].iterator_to(page);
				bundle->genLists_[g].erase(it);
				bundle->genSizes_[g]--;
				page->generation = bundle->newestGen_;
				bundle->genLists_[bundle->newestGen_].push_back(page);
				bundle->genSizes_[bundle->newestGen_]++;
			}
		}

		// The bundle did not take the page via takePosted() yet.
		if(averted)
			resolveOutstanding_(1);

		rotationTurnaround_.fetch_add(1, std::memory_order_relaxed);
		if (shouldRotate_())
			wakeEvent_.raise();
	}

	void handlePressure() {
		wakeEvent_.raise();
	}

	auto awaitReclaim(CacheBundle *bundle, async::cancellation_token ct = {}) {
		return async::transform(
			bundle->_reclaimEvent.async_wait_if(
				[bundle] {
					auto irqLock = frg::guard(&irqMutex());
					auto lock = frg::guard(&bundle->reclaimMutex_);
					return bundle->_reclaimList.empty();
				},
				ct),
			[] (auto) { }
		);
	}

	// Takes the pages that were posted to the bundle.
	// The pages are no longer registered but they stay outstanding until the bundle reports them to completeReclaim().
	void takePosted(CacheBundle *bundle, CachePagesList &out) {
		assert(out.empty());
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&bundle->reclaimMutex_);

		out.splice(out.end(), bundle->_reclaimList);

		for(auto page : out) {
			assert(page->flags & CachePage::reclaimRegistered);
			assert(page->flags & CachePage::reclaimPosted);

			page->flags &= ~(CachePage::reclaimRegistered | CachePage::reclaimPosted);
		}
	}

	// Reports the outcome for pages that the bundle took via takePosted().
	// Averted pages are those that the bundle did not free (e.g., since they are in use or dirty).
	void completeReclaim(size_t numEvicted, size_t numAverted) {
		numEvicted_.fetch_add(numEvicted, std::memory_order_relaxed);
		resolveOutstanding_(numEvicted + numAverted);
	}

	void runReclaimFiber() {
		KernelFiber::run([this] {
			if (disableUncaching)
				return;
			while(true) {
				auto totalPages = physicalAllocator->numTotalPages();
				auto usedPages = physicalAllocator->numUsedPages();

				if(logReclaim) {
					infoLogger() << "thor: " << (usedPages * kPageSize / 1024)
							<< " KiB / " << (totalPages * kPageSize / 1024)
							<< " KiB in use" << frg::endlog;
				}

				if(tortureUncaching) {
					evictPages_(~size_t{0});
				}else if(physicalAllocator->pressureShortfall()) {
					if(!reclaimShortfall_()) {
						// Retrying immediately would not find more pages either.
						if(logReclaim)
							infoLogger() << "thor: Reclamation makes no progress" << frg::endlog;
						KernelFiber::asyncBlockCurrent(
							generalTimerEngine()->sleepFor(stalledReclaimBackoffNs));
					}
				}

				// Start a new generation when the number of page bumps crosses a threshold.
				if(shouldRotate_())
					rotateGenerations_();

				if(tortureUncaching) {
					KernelFiber::asyncBlockCurrent(generalTimerEngine()->sleepFor(10'000'000));
					continue;
				}

				// handleReclaimPressure() raises wakeEvent_ from allocation paths.
				// We only wait for this event since racing it against other senders would run their cancellation there.
				KernelFiber::asyncBlockCurrent(async::transform(
					wakeEvent_.async_wait_if([&] -> bool {
						return !shouldRotate_() && !physicalAllocator->pressureShortfall();
					}),
					[] (auto) {}
				));
			}
		});
	}

private:
	// CachePage::generation of pages in the oldest generation. This generation saturates,
	// so its index (which moves on rotation) is not stored in the pages.
	static constexpr uint8_t oldestGeneration = 0xFF;
	static_assert(oldestGeneration >= CacheBundle::numGenerations);

	// Pages to post per round of reclaimShortfall_().
	// Bounds over-eviction, since the shortfall is re-measured between batches.
	// Also ensure that evictPages_() does not do too much work without dropping locks.
	static constexpr size_t maxEvictBatch = 2048;
	// Rounds without evicted pages after which reclaimShortfall_() gives up.
	static constexpr unsigned int maxRoundsWithoutProgress = 4;
	// Time that the reclaimer waits after reclaimShortfall_() gave up.
	static constexpr uint64_t stalledReclaimBackoffNs = 1'000'000'000;

	// Posts pages until pressureShortfall() drops to zero.
	// Each round waits until the bundles have evicted or averted all posted pages.
	// Returns false if no progress could be made.
	bool reclaimShortfall_() {
		unsigned int roundsWithoutProgress = 0;
		while(true) {
			auto shortfall = physicalAllocator->pressureShortfall();
			if(!shortfall)
				return true;

			auto evictedBefore = numEvicted_.load(std::memory_order_relaxed);
			auto posted = evictPages_(frg::min(shortfall, maxEvictBatch));
			if(!posted)
				return false;
			if(logReclaim)
				infoLogger() << "thor: Reclamation posts " << posted << " pages" << frg::endlog;

			while(numOutstanding_.load(std::memory_order_acquire)) {
				KernelFiber::asyncBlockCurrent(async::transform(
					feedbackEvent_.async_wait_if([this] -> bool {
						return numOutstanding_.load(std::memory_order_acquire);
					}),
					[] (auto) {}
				));
			}

			// Pages that the bundles averted (e.g., since they are in use) do not count as progress.
			if(numEvicted_.load(std::memory_order_relaxed) != evictedBefore) {
				roundsWithoutProgress = 0;
			}else if(++roundsWithoutProgress >= maxRoundsWithoutProgress) {
				return false;
			}
		}
	}

	// Posts up to n pages to the bundles' reclaim lists, starting at the oldest generation.
	// Each bundle contributes in proportion to its share of that generation.
	// Returns the number of posted pages.
	size_t evictPages_(size_t n) {
		size_t posted = 0;
		unsigned int emptyRotations = 0;
		while(posted < n && numInGenerations_.load(std::memory_order_relaxed)) {
			unsigned int oldest;
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&mutex_);
				oldest = (newestGen_ + 1) % CacheBundle::numGenerations;
			}

			size_t total = 0;
			auto bundle = pinNextBundle_(nullptr);
			while(bundle) {
				{
					auto irqLock = frg::guard(&irqMutex());
					auto lock = frg::guard(&bundle->reclaimMutex_);
					total += bundle->genSizes_[oldest];
				}
				bundle = pinNextBundle_(bundle.get());
			}

			if(!total) {
				// Start a new generation once the oldest one is empty.
				// The counters are only approximate, so do not rotate forever.
				if(++emptyRotations > CacheBundle::numGenerations)
					break;
				rotateGenerations_();
				continue;
			}
			emptyRotations = 0;

			auto wanted = frg::min(n - posted, total);
			size_t seen = 0;
			bundle = pinNextBundle_(nullptr);
			while(bundle) {
				size_t share = 0;
				{
					auto irqLock = frg::guard(&irqMutex());
					auto lock = frg::guard(&bundle->reclaimMutex_);

					auto size = bundle->genSizes_[oldest];
					// Splitting by cumulative size keeps the sum of the shares at or below wanted.
					// Sizes only shrink after total was computed, so seen does not exceed total.
					auto before = wanted * seen / total;
					seen += size;
					share = wanted * seen / total - before;
					assert(share <= size);

					auto &genList = bundle->genLists_[oldest];
					for(size_t i = 0; i < share; i++) {
						auto page = genList.pop_front();
						assert(page->flags & CachePage::reclaimRegistered);
						assert(!(page->flags & CachePage::reclaimPosted));
						page->flags |= CachePage::reclaimPosted;
						bundle->_reclaimList.push_back(page);
					}
					bundle->genSizes_[oldest] -= share;
					numInGenerations_.fetch_sub(share, std::memory_order_relaxed);
					numOutstanding_.fetch_add(share, std::memory_order_relaxed);
				}

				if(share) {
					posted += share;
					bundle->_reclaimEvent.raise();
				}

				// Drops the previous pin outside of the RCU section.
				bundle = pinNextBundle_(bundle.get());
			}
		}
		return posted;
	}

	// Starts a new generation in all bundles. This does not evict pages:
	// the pages of the oldest generation are merged into the next older one.
	void rotateGenerations_() {
		rotationTurnaround_.store(0, std::memory_order_relaxed);

		unsigned int newest;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&mutex_);
			newest = (newestGen_ + 1) % CacheBundle::numGenerations;
			newestGen_ = newest;
		}

		auto bundle = pinNextBundle_(nullptr);
		while(bundle) {
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&bundle->reclaimMutex_);

				// Bundles that registered concurrently may already be at the new generation.
				if(bundle->newestGen_ != newest) {
					// The slot after newest is the oldest.
					// This becomes the newest generation after rotation.
					auto g = (bundle->newestGen_ + 1) % CacheBundle::numGenerations;
					// Rotations are serialized on the reclaimer fiber, so bundles are at most one behind.
					assert(g == newest);
					auto next = (g + 1) % CacheBundle::numGenerations;
					// next becomes the oldest generation. The pages of g are already labeled as such,
					// so only one generation's worth of pages is relabeled.
					for(auto page : bundle->genLists_[next])
						page->generation = oldestGeneration;
					auto &genList = bundle->genLists_[g];
					// Keep the pages of the oldest generation in front.
					genList.splice(genList.end(), bundle->genLists_[next]);
					bundle->genLists_[next].splice(bundle->genLists_[next].end(), genList);
					bundle->genSizes_[next] += bundle->genSizes_[g];
					bundle->genSizes_[g] = 0;
					bundle->newestGen_ = g;
				}
			}

			// Drops the previous pin outside of the RCU section.
			bundle = pinNextBundle_(bundle.get());
		}
	}

	// Pins the first live bundle after previous (or the first live bundle if previous is null).
	// Precondition: previous must be pinned by the caller, such that it is still linked.
	smarter::shared_ptr<CacheBundle> pinNextBundle_(CacheBundle *previous) {
		ScheduleGuard rcuGuard;

		auto it = bundleList_.begin();
		if(previous) {
			it = bundleList_.iterator_to(previous);
			++it;
		}
		// Dying bundles fail to lock() and can be skipped.
		for(; it != bundleList_.end(); ++it) {
			auto next = (*it)->selfPtr_.lock();
			if(next)
				return next;
		}
		return {};
	}

	void resolveOutstanding_(size_t n) {
		if(!n)
			return;
		if(numOutstanding_.fetch_sub(n, std::memory_order_acq_rel) == n)
			feedbackEvent_.raise();
	}

	// Generation index of a page. Callers must hold the bundle's reclaimMutex_.
	unsigned int generationOf_(CacheBundle *bundle, CachePage *page) {
		if(page->generation == oldestGeneration)
			return (bundle->newestGen_ + 1) % CacheBundle::numGenerations;
		return page->generation;
	}

	bool shouldRotate_() {
		auto threshold = numInGenerations_.load(std::memory_order_relaxed)
				/ CacheBundle::numGenerations;
		return rotationTurnaround_.load(std::memory_order_relaxed) >= frg::max(threshold, size_t{1});
	}

	frg::ticket_spinlock mutex_;

	// Newest generation of all bundles; see CacheBundle::newestGen_.
	// Protected by mutex_.
	unsigned int newestGen_ = 0;

	// Number of pages in the generation lists of all bundles.
	std::atomic<size_t> numInGenerations_{0};
	// Number of posted pages that were neither evicted nor averted yet.
	std::atomic<size_t> numOutstanding_{0};
	// Number of posted pages that the bundles evicted. Only grows.
	std::atomic<size_t> numEvicted_{0};
	// Raised when numOutstanding_ drops to zero.
	async::recurring_event feedbackEvent_;

	// Protected against modification by mutex_.
	frg::intrusive_rcu_list<
		CacheBundle,
		frg::locate_member<
			CacheBundle,
			frg::intrusive_rcu_list_hook<CacheBundle>,
			&CacheBundle::reclaimerHook_
		>
	> bundleList_;

	// Number of pages bumped since the last generation rotation.
	std::atomic<size_t> rotationTurnaround_{0};

	// Raised when shouldRotate_() becomes true and by handleReclaimPressure().
	async::recurring_event wakeEvent_;
};

static frg::manual_box<MemoryReclaimer> globalReclaimer;
// Allocations can apply pressure before globalReclaimer is initialized.
static std::atomic<bool> globalReclaimerReady{false};

static initgraph::Task initReclaim{&globalInitEngine, "generic.init-reclaim",
	initgraph::Requires{getFibersAvailableStage()},
	[] {
		frg::array args = {
			frg::option{"thor.torture-uncaching", frg::store_true(tortureUncaching)},
		};
		frg::parse_arguments(getKernelCmdline(), args);
		if(tortureUncaching)
			infoLogger() << "thor: torture-uncaching is enabled" << frg::endlog;

		globalReclaimer.initialize();
		globalReclaimerReady.store(true, std::memory_order_release);
		globalReclaimer->runReclaimFiber();
	}
};

void handleReclaimPressure() {
	if(!globalReclaimerReady.load(std::memory_order_acquire))
		return;
	globalReclaimer->handlePressure();
}

// --------------------------------------------------------
// MemoryView.
// --------------------------------------------------------

coroutine<frg::expected<Error>> MemoryView::resize(size_t newSize) {
	assert(currentIpl() == ipl::exceptionalWork);
	(void)newSize;
	co_return Error::illegalObject;
}

coroutine<frg::expected<Error, smarter::shared_ptr<MemoryView>>> MemoryView::fork(smarter::shared_ptr<Hierarchy>) {
	assert(currentIpl() == ipl::exceptionalWork);
	co_return Error::illegalObject;
}

namespace {

// The do*() helpers perform the synchronous part of the copy functions.
// They must not be inlined into the coroutines: otherwise, clang can keep a function_ref to a
// closure on the native stack across a co_await (llvm/llvm-project#149604, fixed in LLVM 22).

// Copies until the end of the range or until a page is missing.
// Returns the number of bytes that were copied.
[[gnu::noinline]] std::expected<size_t, Error> doCopyTo(MemoryView *view, uintptr_t offset,
		const void *pointer, size_t size, FetchFlags flags) {
	size_t progress = 0;
	while(progress < size) {
		auto outcome = view->accessRange(offset + progress, size - progress,
				flags | fetchRequireMutable, [&] (void *page, size_t chunk) {
			assert(chunk <= size - progress);
			memcpy(page, reinterpret_cast<const uint8_t *>(pointer) + progress, chunk);
			return PageAccessResult{.dirty = true};
		});
		if(!outcome)
			return std::unexpected{outcome.error()};
		if(!*outcome)
			break;
		progress += *outcome;
	}
	return progress;
}

// Like doCopyTo() but in the opposite direction.
[[gnu::noinline]] std::expected<size_t, Error> doCopyFrom(MemoryView *view, uintptr_t offset,
		void *pointer, size_t size, FetchFlags flags) {
	size_t progress = 0;
	while(progress < size) {
		auto outcome = view->accessRange(offset + progress, size - progress, flags,
				[&] (void *page, size_t chunk) {
			assert(chunk <= size - progress);
			memcpy(reinterpret_cast<uint8_t *>(pointer) + progress, page, chunk);
			return PageAccessResult{};
		});
		if(!outcome)
			return std::unexpected{outcome.error()};
		if(!*outcome)
			break;
		progress += *outcome;
	}
	return progress;
}

// Like doCopyTo() but sets srcMissing to indicate which of the two pages is missing.
[[gnu::noinline]] std::expected<size_t, Error> doCopyBetweenViews(
		MemoryView *destView, uintptr_t destOffset,
		MemoryView *srcView, uintptr_t srcOffset, size_t size, bool &srcMissing) {
	size_t progress = 0;
	while(progress < size) {
		// The source access is nested into the destination access.
		std::expected<size_t, Error> srcOutcome{0};
		auto destOutcome = destView->accessRange(destOffset + progress, size - progress,
				fetchRequireMutable, [&] (void *destPage, size_t destChunk) {
			assert(destChunk <= size - progress);
			srcOutcome = srcView->accessRange(srcOffset + progress, destChunk, fetchNone,
					[&] (void *srcPage, size_t srcChunk) {
				assert(srcChunk <= destChunk);
				memcpy(destPage, srcPage, srcChunk);
				return PageAccessResult{};
			});
			return PageAccessResult{.dirty = srcOutcome && *srcOutcome};
		});
		if(!destOutcome)
			return std::unexpected{destOutcome.error()};
		if(!*destOutcome) {
			srcMissing = false;
			break;
		}
		if(!srcOutcome)
			return std::unexpected{srcOutcome.error()};
		if(!*srcOutcome) {
			srcMissing = true;
			break;
		}
		progress += *srcOutcome;
	}
	return progress;
}

} // anonymous namespace

coroutine<frg::expected<Error>> MemoryView::copyTo(uintptr_t offset,
		const void *pointer, size_t size,
		FetchFlags flags) {
	// fetchRequireMutable is managed by this function.
	assert(!(flags & fetchRequireMutable));
	assert(currentIpl() == ipl::exceptionalWork);

	size_t progress = 0;
	while(true) {
		auto outcome = doCopyTo(this, offset + progress,
				reinterpret_cast<const uint8_t *>(pointer) + progress, size - progress, flags);
		if(!outcome)
			co_return outcome.error();
		progress += *outcome;
		if(progress == size)
			break;
		auto fetchOffset = (offset + progress) & ~(kPageSize - 1);
		FRG_CO_TRY(co_await touchRange(fetchOffset, kPageSize, flags | fetchRequireMutable));
	}

	co_return {};
}

coroutine<frg::expected<Error>> MemoryView::copyFrom(uintptr_t offset,
		void *pointer, size_t size,
		FetchFlags flags) {
	// fetchRequireMutable is managed by this function.
	assert(!(flags & fetchRequireMutable));
	assert(currentIpl() == ipl::exceptionalWork);

	size_t progress = 0;
	while(true) {
		auto outcome = doCopyFrom(this, offset + progress,
				reinterpret_cast<uint8_t *>(pointer) + progress, size - progress, flags);
		if(!outcome)
			co_return outcome.error();
		progress += *outcome;
		if(progress == size)
			break;
		auto fetchOffset = (offset + progress) & ~(kPageSize - 1);
		FRG_CO_TRY(co_await touchRange(fetchOffset, kPageSize, flags));
	}

	co_return {};
}

coroutine<frg::expected<Error>>
MemoryView::touchFullRange(uintptr_t offset, size_t size, FetchFlags flags) {
	size_t progress = 0;
	while (progress < size) {
		auto chunk = FRG_CO_TRY(co_await touchRange(offset + progress, size - progress, flags));
		progress += chunk;
	}
	co_return {};
}

Error MemoryView::updateRange(ManageRequest, size_t, size_t) {
	return Error::illegalObject;
}

Error MemoryView::markDirtyRange(size_t, size_t) {
	return Error::illegalObject;
}

coroutine<frg::expected<Error>> MemoryView::writebackFence(uintptr_t, size_t) {
	co_return {};
}

coroutine<frg::expected<Error>> MemoryView::invalidateRange(uintptr_t, size_t, DiscardMode) {
	co_return {};
}

coroutine<frg::expected<Error, MemoryNotification>> MemoryView::pollNotification() {
	co_return Error::illegalObject;
}

Error MemoryView::setIndirection(size_t, smarter::shared_ptr<MemoryView>,
		uintptr_t, size_t, CachingFlags) {
	return Error::illegalObject;
}

coroutine<frg::expected<Error>> copyBetweenViews(
		MemoryView *destView, uintptr_t destOffset,
		MemoryView *srcView, uintptr_t srcOffset, size_t size) {
	size_t progress = 0;
	while(true) {
		bool srcMissing = false;
		auto outcome = doCopyBetweenViews(destView, destOffset + progress,
				srcView, srcOffset + progress, size - progress, srcMissing);
		if(!outcome)
			co_return outcome.error();
		progress += *outcome;
		if(progress == size)
			break;
		if(srcMissing) {
			auto fetchOffset = (srcOffset + progress) & ~(kPageSize - 1);
			FRG_CO_TRY(co_await srcView->touchRange(fetchOffset, kPageSize, fetchNone));
		} else {
			auto fetchOffset = (destOffset + progress) & ~(kPageSize - 1);
			FRG_CO_TRY(co_await destView->touchRange(fetchOffset, kPageSize, fetchRequireMutable));
		}
	}

	co_return {};
}

// --------------------------------------------------------
// getZeroMemory()
// --------------------------------------------------------

namespace {

struct ZeroMemory final : MemoryView {
private:
	struct CtorToken {};

public:
	static std::expected<smarter::shared_ptr<ZeroMemory>, Error> create() {
		auto ptr = allocate_rcu_shared<ZeroMemory>(*kernelAlloc, CtorToken{});
		ptr->selfPtr = ptr;
		return ptr;
	}

	ZeroMemory(CtorToken) {
		_zeroPage = physicalAllocator->allocateInfallible(PhysicalKind::pinned);
		PageAccessor accessor{_zeroPage};
		memset(accessor.get(), 0, kPageSize);

		globalPfnDb().insert(_zeroPage, PfnDescriptor::otherPage());
	}
	ZeroMemory(const ZeroMemory &) = delete;
	~ZeroMemory() = default;

	ZeroMemory &operator= (const ZeroMemory &) = delete;

	size_t getLength() override {
		return size_t{1} << 46;
	}

	coroutine<frg::expected<Error>> copyFrom(uintptr_t, void *buffer, size_t size,
			FetchFlags) override {
		memset(buffer, 0, size);
		co_return {};
	}

	Error lockRange(uintptr_t, size_t) override {
		return Error::success;
	}

	void unlockRange(uintptr_t, size_t) override {
		// Do nothing.
	}

	PhysicalRange peekRange(uintptr_t offset, FetchFlags flags) override {
		if(flags & fetchRequireMutable)
			return PhysicalRange{};

		auto misalign = offset & (kPageSize - 1);

		return PhysicalRange{
			.physical = _zeroPage + misalign,
			.size = kPageSize - misalign,
			.cachingMode = CachingMode::null,
			.isMutable = false
		};
	}

	std::expected<size_t, Error> accessRange(uintptr_t offset, size_t size,
			FetchFlags flags, PageAccessFn fn) override {
		if(flags & fetchRequireMutable)
			return std::unexpected{Error::badPermissions};

		auto misalign = offset & (kPageSize - 1);
		auto chunk = frg::min(size, kPageSize - misalign);
		PageAccessor accessor{_zeroPage};
		fn(reinterpret_cast<uint8_t *>(accessor.get()) + misalign, chunk);
		return chunk;
	}

	coroutine<frg::expected<Error, size_t>>
	touchRange(uintptr_t offset, size_t, FetchFlags flags) override {
		if(flags & fetchRequireMutable)
			co_return Error::badPermissions;

		auto misalign = offset & (kPageSize - 1);

		co_return kPageSize - misalign;
	}

public:
	// Contract: set by the code that constructs this object.
	smarter::borrowed_ptr<ZeroMemory> selfPtr;

private:
	PhysicalAddr _zeroPage;
};

}

smarter::shared_ptr<MemoryView> getZeroMemory() {
	static frg::eternal<smarter::shared_ptr<ZeroMemory>> singleton = [] {
		auto memoryOutcome = ZeroMemory::create();
		if(!memoryOutcome)
			panicLogger() << "thor: Failed to create zero memory" << frg::endlog;
		return std::move(*memoryOutcome);
	}();
	return singleton.get();
}

// --------------------------------------------------------
// ImmediateMemory
// --------------------------------------------------------

std::expected<smarter::shared_ptr<ImmediateMemory>, Error>
ImmediateMemory::create(size_t length) {
	auto ptr = allocate_rcu_shared<ImmediateMemory>(*kernelAlloc, CtorToken{});
	ptr->selfPtr = ptr;

	auto numPages = (length + kPageSize - 1) >> kPageShift;
	ptr->_physicalPages.resize(numPages, PhysicalAddr(-1));
	for(size_t i = 0; i < numPages; ++i) {
		auto physical = physicalAllocator->allocate(kPageSize, 64);
		if(physical == PhysicalAddr(-1))
			return std::unexpected{Error::noMemory};

		PageAccessor accessor{physical};
		memset(accessor.get(), 0, kPageSize);

		globalPfnDb().insert(physical, PfnDescriptor::otherPage());
		ptr->_physicalPages[i] = physical;
	}
	return ptr;
}

ImmediateMemory::ImmediateMemory(CtorToken)
: _physicalPages{*kernelAlloc} { }

ImmediateMemory::~ImmediateMemory() {
	for(size_t i = 0; i < _physicalPages.size(); ++i) {
		if(_physicalPages[i] == PhysicalAddr(-1))
			continue;
		globalPfnDb().erase(_physicalPages[i]);
		physicalAllocator->free(_physicalPages[i], kPageSize);
	}
}

coroutine<frg::expected<Error>> ImmediateMemory::resize(size_t newSize) {
	assert(currentIpl() == ipl::exceptionalWork);

	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		size_t currentNumPages = _physicalPages.size();
		size_t newNumPages = (newSize + kPageSize - 1) >> kPageShift;
		assert(newNumPages >= currentNumPages);
		_physicalPages.resize(newNumPages, PhysicalAddr(-1));
		for(size_t i = currentNumPages; i < newNumPages; ++i) {
			auto physical = physicalAllocator->allocate(kPageSize, 64);
			if(physical == PhysicalAddr(-1))
				co_return Error::noMemory;

			PageAccessor accessor{physical};
			memset(accessor.get(), 0, kPageSize);

			globalPfnDb().insert(physical, PfnDescriptor::otherPage());
			_physicalPages[i] = physical;
		}
	}

	co_return {};
}

Error ImmediateMemory::lockRange(uintptr_t, size_t) {
	return Error::success;
}

void ImmediateMemory::unlockRange(uintptr_t, size_t) {
	// Do nothing.
}

PhysicalRange ImmediateMemory::peekRange(uintptr_t offset, FetchFlags) {
	auto index = offset >> kPageShift;
	auto misalign = offset & (kPageSize - 1);

	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	if(index >= _physicalPages.size())
		return PhysicalRange{};

	return PhysicalRange{
		.physical = _physicalPages[index] + misalign,
		.size = kPageSize - misalign,
		.cachingMode = CachingMode::null,
		.isMutable = true
	};
}

std::expected<size_t, Error> ImmediateMemory::accessRange(uintptr_t offset, size_t size,
		FetchFlags, PageAccessFn fn) {
	auto index = offset >> kPageShift;
	auto misalign = offset & (kPageSize - 1);

	PhysicalAddr physical;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		if(index >= _physicalPages.size())
			return std::unexpected{Error::fault};
		physical = _physicalPages[index];
	}
	if(physical == PhysicalAddr(-1))
		return std::unexpected{Error::fault};

	auto chunk = frg::min(size, kPageSize - misalign);
	PageAccessor accessor{physical};
	fn(reinterpret_cast<uint8_t *>(accessor.get()) + misalign, chunk);
	return chunk;
}

coroutine<frg::expected<Error, size_t>>
ImmediateMemory::touchRange(uintptr_t offset, size_t, FetchFlags) {
	assert(currentIpl() == ipl::exceptionalWork);

	auto index = offset >> kPageShift;
	auto misalign = offset & (kPageSize - 1);

	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	if(index >= _physicalPages.size())
		co_return Error::fault;

	co_return kPageSize - misalign;
}

size_t ImmediateMemory::getLength() {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	return _physicalPages.size() * kPageSize;
}

// --------------------------------------------------------
// ImmediateWindow
// --------------------------------------------------------

ImmediateWindow::ImmediateWindow(smarter::shared_ptr<ImmediateMemory> memory)
: _memory{std::move(memory)} {
	_size = _memory->getLength();
	_base = KernelVirtualMemory::global().allocate(_size);
	assert(_base);
	for(size_t offset = 0; offset < _size; offset += kPageSize) {
		auto physicalRange = _memory->peekRange(offset, fetchRequireMutable);
		assert(physicalRange.isMutable);
		KernelPageSpace::global().mapSingle4k(
			reinterpret_cast<VirtualAddr>(_base) + offset,
			physicalRange.physical,
			page_access::write,
			CachingMode::null
		);
	}
}

ImmediateWindow::~ImmediateWindow() {
	if(!_base)
		return;
	for(size_t offset = 0; offset < _size; offset += kPageSize)
		KernelPageSpace::global().unmapSingle4k(reinterpret_cast<VirtualAddr>(_base) + offset);
	spawnOnWorkQueue(
		*kernelAlloc,
		WorkQueue::criticalQueue().lock(),
		[](smarter::shared_ptr<ImmediateMemory> memory, void *base, size_t size) -> coroutine<void> {
			// The pages are still accessible until shootdown completes, so keep the shared_ptr around.
			(void)memory;
			co_await shootdown(
				&KernelPageSpace::global(),
				reinterpret_cast<VirtualAddr>(base),
				size,
				WorkQueue::criticalQueue().get()
			);
			KernelVirtualMemory::global().deallocate(base, size);
		}(std::move(_memory), _base, _size)
	);
}

// --------------------------------------------------------
// HardwareMemory
// --------------------------------------------------------

std::expected<smarter::shared_ptr<HardwareMemory>, Error> HardwareMemory::create(
		PhysicalAddr base, size_t length, CachingMode cache_mode) {
	auto ptr = allocate_rcu_shared<HardwareMemory>(*kernelAlloc, CtorToken{},
			base, length, cache_mode);
	return ptr;
}

HardwareMemory::HardwareMemory(CtorToken, PhysicalAddr base, size_t length, CachingMode cache_mode)
: _base{base}, _length{length}, _cacheMode{cache_mode} {
	assert(!(base % kPageSize));
	assert(!(length % kPageSize));

	for(PhysicalAddr pa = _base; pa < _base + _length; pa += kPageSize)
		globalPfnDb().insertOrExchange(pa, [](frg::optional<PfnDescriptor> descriptor) {
			if(!descriptor)
				return PfnDescriptor::hardwarePage(1);
			assert(descriptor->isHardware());
			return PfnDescriptor::hardwarePage(descriptor->hardwareRefCount() + 1);
		});
}

HardwareMemory::~HardwareMemory() {
	for(PhysicalAddr pa = _base; pa < _base + _length; pa += kPageSize)
		globalPfnDb().exchangeOrErase(pa, [](PfnDescriptor descriptor) -> frg::optional<PfnDescriptor> {
			assert(descriptor.isHardware());
			auto refCount = descriptor.hardwareRefCount();
			assert(refCount > 0);
			if(refCount == 1)
				return frg::null_opt;
			return PfnDescriptor::hardwarePage(refCount - 1);
		});
}

Error HardwareMemory::lockRange(uintptr_t, size_t) {
	// Hardware memory is "always locked".
	return Error::success;
}

void HardwareMemory::unlockRange(uintptr_t, size_t) {
	// Hardware memory is "always locked".
}

PhysicalRange HardwareMemory::peekRange(uintptr_t offset, FetchFlags) {
	if(offset >= _length)
		return PhysicalRange{};

	return PhysicalRange{
		.physical = _base + offset,
		.size = _length - offset,
		.cachingMode = _cacheMode,
		.isMutable = true
	};
}

std::expected<size_t, Error> HardwareMemory::accessRange(uintptr_t, size_t,
		FetchFlags, PageAccessFn) {
	// The direct physical mapping only covers RAM, so users need to map hardware memory.
	return std::unexpected{Error::illegalObject};
}

coroutine<frg::expected<Error, size_t>>
HardwareMemory::touchRange(uintptr_t offset, size_t, FetchFlags) {
	assert(currentIpl() == ipl::exceptionalWork);

	if(offset >= _length)
		co_return Error::fault;

	co_return _length - offset;
}

size_t HardwareMemory::getLength() {
	return _length;
}

// --------------------------------------------------------
// AllocatedMemory
// --------------------------------------------------------

std::expected<smarter::shared_ptr<AllocatedMemory>, Error> AllocatedMemory::create(
	smarter::shared_ptr<Hierarchy> hierarchy, size_t length, int addressBits, size_t chunkSize, size_t chunkAlign
) {
	smarter::borrowed_ptr<Hierarchy> hierarchyPtr{hierarchy};
	auto ptr = allocate_rcu_shared<AllocatedMemory>(HierarchyAllocator{std::move(hierarchy)},
			CtorToken{}, hierarchyPtr, length, addressBits, chunkSize, chunkAlign);
	ptr->selfPtr = ptr;
	return ptr;
}

AllocatedMemory::AllocatedMemory(
	CtorToken, smarter::borrowed_ptr<Hierarchy> hierarchy, size_t desiredLngth, int addressBits, size_t desiredChunkSize, size_t chunkAlign
)
: _hierarchy{std::move(hierarchy)}, _physicalChunks{AccountAllocator{_hierarchy->controller()}},
		_addressBits{addressBits}, _chunkAlign{chunkAlign} {
	static_assert(sizeof(unsigned long) == sizeof(uint64_t), "Fix use of __builtin_clzl");
	_chunkSize = size_t(1) << (64 - __builtin_clzl(desiredChunkSize - 1));
	if(_chunkSize != desiredChunkSize)
		urgentLogger() << "Physical allocation of size " << (void *)desiredChunkSize
				<< " rounded up to power of 2" << frg::endlog;

	size_t length = (desiredLngth + (_chunkSize - 1)) & ~(_chunkSize - 1);
	if(length != desiredLngth)
		urgentLogger() << "Memory length " << (void *)desiredLngth
				<< " rounded up to chunk size " << (void *)_chunkSize
				<< frg::endlog;

	assert(_chunkSize % kPageSize == 0);
	assert(_chunkAlign % kPageSize == 0);
	assert(_chunkSize % _chunkAlign == 0);
	_physicalChunks.resize(length / _chunkSize, PhysicalAddr(-1));
}

AllocatedMemory::~AllocatedMemory() {
	// TODO: This destructor takes a lock. This is potentially unexpected.
	// Rework this to only schedule the deallocation but not actually perform it?
	if(logUsage)
		infoLogger() << "thor: Releasing AllocatedMemory ("
				<< (physicalAllocator->numUsedPages() * 4) << " KiB in use)" << frg::endlog;
	for(size_t i = 0; i < _physicalChunks.size(); ++i) {
		if(_physicalChunks[i] != PhysicalAddr(-1)) {
			for(size_t pg = 0; pg < _chunkSize; pg += kPageSize)
				globalPfnDb().erase(_physicalChunks[i] + pg);
			deallocateAndUncharge(_hierarchy.get(), _physicalChunks[i], _chunkSize,
					_chunkSize > kPageSize ? PhysicalKind::contiguous : PhysicalKind::pinned);
		}
	}
	if(logUsage)
		infoLogger() << "thor:     ("
				<< (physicalAllocator->numUsedPages() * 4) << " KiB in use)" << frg::endlog;
}

coroutine<frg::expected<Error>> AllocatedMemory::resize(size_t newSize) {
	assert(currentIpl() == ipl::exceptionalWork);

	{
		auto irq_lock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		if (newSize % _chunkSize)
			co_return Error::illegalArgs;
		size_t numChunks = newSize / _chunkSize;
		// TODO: Support shrinking of AllocatedMemory.
		if (numChunks < _physicalChunks.size())
			co_return Error::illegalArgs;
		_physicalChunks.resize(numChunks, PhysicalAddr(-1));
	}
	co_return {};
}

Error AllocatedMemory::lockRange(uintptr_t, size_t) {
	// For now, we do not evict "anonymous" memory. TODO: Implement eviction here.
	return Error::success;
}

void AllocatedMemory::unlockRange(uintptr_t, size_t) {
	// For now, we do not evict "anonymous" memory. TODO: Implement eviction here.
}

PhysicalRange AllocatedMemory::peekRange(uintptr_t offset, FetchFlags) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	auto index = offset / _chunkSize;
	auto misalign = offset & (_chunkSize - 1);

	if(index >= _physicalChunks.size())
		return PhysicalRange{};
	if(_physicalChunks[index] == PhysicalAddr(-1))
		return PhysicalRange{};

	return PhysicalRange{
		.physical = _physicalChunks[index] + misalign,
		.size = _chunkSize - misalign,
		.cachingMode = CachingMode::null,
		.isMutable = true
	};
}

std::expected<size_t, Error> AllocatedMemory::accessRange(uintptr_t offset, size_t size,
		FetchFlags, PageAccessFn fn) {
	auto index = offset / _chunkSize;
	auto chunkOffset = offset & (_chunkSize - 1);
	auto misalign = offset & (kPageSize - 1);

	PhysicalAddr physical;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		if(index >= _physicalChunks.size())
			return std::unexpected{Error::fault};
		if(_physicalChunks[index] == PhysicalAddr(-1))
			return 0;
		physical = _physicalChunks[index] + (chunkOffset - misalign);
	}

	// Chunks are physically contiguous, so the access can span multiple pages.
	auto chunk = frg::min(size, _chunkSize - chunkOffset);
	PageAccessor accessor{physical};
	fn(reinterpret_cast<uint8_t *>(accessor.get()) + misalign, chunk);
	return chunk;
}

coroutine<frg::expected<Error, size_t>>
AllocatedMemory::touchRange(uintptr_t offset, size_t, FetchFlags flags) {
	assert(currentIpl() == ipl::exceptionalWork);

	auto index = offset / _chunkSize;
	auto misalign = offset & (_chunkSize - 1);

	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		if(index >= _physicalChunks.size())
			co_return Error::fault;
		if(_physicalChunks[index] != PhysicalAddr(-1))
			co_return _chunkSize - misalign;
	}

	auto kind = _chunkSize > kPageSize ? PhysicalKind::contiguous : PhysicalKind::pinned;
	std::expected<PhysicalAddr, Error> physicalOutcome;
	if(_chunkSize == kPageSize && _addressBits >= 64) {
		physicalOutcome = co_await chargeAndAllocate(_hierarchy.get(), kind, flags);
	}else{
		physicalOutcome = tryChargeAndAllocate(_hierarchy.get(), _chunkSize,
				kind, _addressBits, flags);
	}
	if(!physicalOutcome)
		co_return physicalOutcome.error();
	auto physical = *physicalOutcome;
	assert(!(physical & (_chunkAlign - 1)));

	for(size_t pg_progress = 0; pg_progress < _chunkSize; pg_progress += kPageSize) {
		PageAccessor accessor{physical + pg_progress};
		memset(accessor.get(), 0, kPageSize);
	}

	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		// resize() never shrinks, so index is still in bounds.
		if(_physicalChunks[index] == PhysicalAddr(-1)) {
			for(size_t pg_progress = 0; pg_progress < _chunkSize; pg_progress += kPageSize)
				globalPfnDb().insert(physical + pg_progress, PfnDescriptor::otherPage());
			_physicalChunks[index] = physical;
			physical = PhysicalAddr(-1);
		}
	}

	// Another thread published a chunk while the lock was dropped.
	if(physical != PhysicalAddr(-1))
		deallocateAndUncharge(_hierarchy.get(), physical, _chunkSize, kind);

	co_return _chunkSize - misalign;
}

size_t AllocatedMemory::getLength() {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	return _physicalChunks.size() * _chunkSize;
}

// --------------------------------------------------------
// ManagedSpace
// --------------------------------------------------------

frg::intrusive_shared_ptr<ManagedSpace::TransactionMonitor, Allocator>
ManagedSpace::ManagedPage::attachMonitor(MonitorType type) {
	auto bit = uint8_t{1} << static_cast<unsigned int>(type);
	assert(!(attachedMonitors & bit));
	attachedMonitors |= bit;
	auto monitor = frg::allocate_intrusive_shared<TransactionMonitor>(Allocator{}, type);
	ref_rc(monitor.get());
	monitor->chainNext = monitors;
	monitors = monitor.get();
	return monitor;
}

frg::intrusive_shared_ptr<ManagedSpace::TransactionMonitor, Allocator>
ManagedSpace::ManagedPage::findMonitor(MonitorType type) {
	auto bit = uint8_t{1} << static_cast<unsigned int>(type);
	if(!(attachedMonitors & bit))
		return {};
	auto ptr = monitors;
	while(ptr->type != type)
		ptr = ptr->chainNext;
	ref_rc(ptr);
	return {frg::adopt_rc, ptr};
}

frg::intrusive_shared_ptr<ManagedSpace::TransactionMonitor, Allocator>
ManagedSpace::ManagedPage::requireMonitor(MonitorType type) {
	auto monitor = findMonitor(type);
	if(!monitor)
		monitor = attachMonitor(type);
	return monitor;
}

frg::intrusive_shared_ptr<ManagedSpace::TransactionMonitor, Allocator>
ManagedSpace::ManagedPage::detachMonitor(MonitorType type) {
	auto bit = uint8_t{1} << static_cast<unsigned int>(type);
	if(!(attachedMonitors & bit))
		return {};
	attachedMonitors &= ~bit;
	auto link = &monitors;
	while((*link)->type != type)
		link = &(*link)->chainNext;
	auto ptr = *link;
	*link = ptr->chainNext;
	ptr->chainNext = nullptr;
	return {frg::adopt_rc, ptr};
}

bool ManagedSpace::ManagedPage::hasMonitor(MonitorType type) {
	auto bit = uint8_t{1} << static_cast<unsigned int>(type);
	return attachedMonitors & bit;
}

bool ManagedSpace::ManagedPage::hasUnwrittenData() {
	return transactionState == TxState::dirty
			|| transactionState == TxState::wantWriteback
			|| transactionState == TxState::writeback
			|| stillDirty;
}

std::expected<smarter::shared_ptr<ManagedSpace>, Error> ManagedSpace::create(
		smarter::shared_ptr<Hierarchy> hierarchy, size_t length, bool readahead) {
	if(length > backingMemoryLength)
		return std::unexpected{Error::illegalArgs};
	smarter::borrowed_ptr<Hierarchy> hierarchyPtr{hierarchy};
	auto self = allocate_rcu_shared<ManagedSpace>(HierarchyAllocator{std::move(hierarchy)},
			hierarchyPtr, length, readahead);
	self->selfPtr = self;
	globalReclaimer->registerBundle(self);
	self->_runningLoops.add(3);
	spawnOnWorkQueue(*kernelAlloc, WorkQueue::memoryQueue().lock(), self->_runReclaimLoop());
	spawnOnWorkQueue(*kernelAlloc, WorkQueue::memoryQueue().lock(), self->_runDrainLoop());
	spawnOnWorkQueue(*kernelAlloc, WorkQueue::memoryQueue().lock(), self->_runInvalidationLoop());
	return self;
}

ManagedSpace::ManagedSpace(smarter::borrowed_ptr<Hierarchy> hierarchy, size_t length, bool readahead)
: hierarchy{std::move(hierarchy)}, pages{AccountAllocator{this->hierarchy->controller()}},
		numPages{length >> kPageShift}, readahead{readahead},
		_evictQueue{frg::allocate_intrusive_shared<EvictionQueue>(Allocator{})},
		_slotStates{AccountAllocator{this->hierarchy->controller()}} {
	assert(!(length & (kPageSize - 1)));
}

coroutine<void> ManagedSpace::_runReclaimLoop() {
	while(true) {
		co_await async::race_and_cancel(
			[&] (async::cancellation_token ct) {
				return globalReclaimer->awaitReclaim(this, ct);
			},
			[&] (async::cancellation_token ct) {
				return async::transform(
					_discardEvent.async_wait_if([this] () -> bool {
						auto irqLock = frg::guard(&irqMutex());
						auto lock = frg::guard(&mutex);
						return !_stopLoops && _discardList.empty();
					}, ct),
					[] (auto) { }
				);
			}
		);

		CachePagesList batch;
		CachePagesList discardBatch;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&mutex);
			if(_stopLoops)
				break;

			globalReclaimer->takePosted(this, batch);

			for(auto cachePage : batch) {
				auto *page = frg::container_of(cachePage, &ManagedPage::cachePage);
				assert(page);
				assert(page->loadState == LoadState::present);
				assert(page->transactionState == TxState::inReclaimer);
				assert(!page->lockCount);
				page->transactionState = TxState::performReclaim;
			}

			discardBatch.splice(discardBatch.end(), _discardList);

			for(auto cachePage : discardBatch) {
				auto *page = frg::container_of(cachePage, &ManagedPage::cachePage);
				assert(page->discarded);
				assert(page->transactionState == TxState::discardQueued
						|| page->transactionState == TxState::avertDiscard);
				if(page->transactionState == TxState::discardQueued)
					page->transactionState = TxState::performDiscard;
			}
		}

		if(batch.empty() && discardBatch.empty())
			continue;

		// Concurrent lookups have averted the reclamation (avertReclaim/avertDiscard) by the end
		// of the grace period; see CachePage::useCount.
		co_await rcuBarrier();

		bool anyDirty = false;
		bool anyExpedite = false;
		bool anyDiscardQueued = false;
		size_t sizeFreed = 0;
		size_t numEvicted = 0;
		size_t numAverted = 0;
		MonitorPendingList pendingMonitors;
		while(!batch.empty()) {
			PhysicalAddr physical;
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&mutex);

				auto cachePage = batch.pop_front();
				auto *page = frg::container_of(cachePage, &ManagedPage::cachePage);

				if(page->discarded) {
					if(page->discardMode == DiscardMode::keepDirty && page->stillDirty) {
						// Dirty contents must still be written back before the entry is erased.
						page->stillDirty = false;
						_enqueueDirty(page, anyExpedite);
						anyDirty = true;
					} else {
						assert(!page->stillDirty || page->discardMode == DiscardMode::dropDirty);
						// The frame is being discarded so it doesn't need to be written back.
						page->stillDirty = false;
						auto writebackMonitor = page->detachMonitor(MonitorType::writeback);
						if(writebackMonitor)
							pendingMonitors.push_back(writebackMonitor.release());
						page->transactionState = TxState::none;
						_disposeDiscarded(page, anyDiscardQueued);
					}
					numAverted++;
					continue;
				}

				if(page->transactionState == TxState::avertReclaim) {
					if(page->stillDirty) {
						page->stillDirty = false;
						_enqueueDirty(page, anyExpedite);
						anyDirty = true;
					} else if(page->lockCount
							|| page->cachePage.useCount.load(std::memory_order_relaxed)) {
						page->transactionState = TxState::none;
					} else {
						globalReclaimer->addPage(&page->cachePage);
						page->transactionState = TxState::inReclaimer;
					}
					numAverted++;
					continue;
				}
				assert(page->transactionState == TxState::performReclaim);

				assert(!page->lockCount);
				assert(page->physical != PhysicalAddr(-1));
				physical = page->physical;

				page->loadState = LoadState::missing;
				page->transactionState = TxState::none;
				page->physical = PhysicalAddr(-1);
			}

			globalPfnDb().erase(physical);
			deallocateAndUncharge(hierarchy.get(), physical, kPageSize, PhysicalKind::evictable);
			sizeFreed += kPageSize;
			numEvicted++;
		}
		globalReclaimer->completeReclaim(numEvicted, numAverted);

		while(!discardBatch.empty()) {
			PhysicalAddr physical;
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&mutex);

				auto cachePage = discardBatch.pop_front();
				auto *page = frg::container_of(cachePage, &ManagedPage::cachePage);
				assert(page->discarded);
				assert(page->transactionState == TxState::performDiscard
						|| page->transactionState == TxState::avertDiscard);

				if(page->transactionState == TxState::avertDiscard) {
					if(page->discardMode == DiscardMode::keepDirty && page->stillDirty) {
						// Dirty contents must still be written back before the entry is erased.
						page->stillDirty = false;
						_enqueueDirty(page, anyExpedite);
						anyDirty = true;
					} else {
						assert(!page->stillDirty || page->discardMode == DiscardMode::dropDirty);
						page->stillDirty = false;
						auto writebackMonitor = page->detachMonitor(MonitorType::writeback);
						if(writebackMonitor)
							pendingMonitors.push_back(writebackMonitor.release());
						page->transactionState = TxState::none;
						_disposeDiscarded(page, anyDiscardQueued);
					}
					continue;
				}

				assert(!page->lockCount);
				assert(!page->cachePage.useCount.load(std::memory_order_relaxed));
				physical = page->physical;

				auto monitor = page->detachMonitor(MonitorType::discard);
				if(monitor)
					pendingMonitors.push_back(monitor.release());

				if(physical != PhysicalAddr(-1))
					globalPfnDb().erase(physical);
				_pageDiscarded(page, anyDirty);
				_erasePage(page);
			}

			if(physical != PhysicalAddr(-1)) {
				deallocateAndUncharge(hierarchy.get(), physical, kPageSize, PhysicalKind::evictable);
				sizeFreed += kPageSize;
			}
		}

		_raiseMonitors(pendingMonitors);
		if(anyDirty)
			_dirtyEvent.raise();
		if(anyExpedite)
			_expediteEvent.raise();
		if(anyDiscardQueued)
			_discardEvent.raise();

		if(logUncaching)
			infoLogger() << frg::fmt(
				"thor: Reclamation freed 0x{:x} bytes",
				sizeFreed
			)<< frg::endlog;
	}
	_runningLoops.done();
}

coroutine<void> ManagedSpace::_runDrainLoop() {
	while(true) {
		co_await _dirtyEvent.async_wait_if([this] () -> bool {
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&mutex);
			return !_stopLoops && (_dirtyList.empty() || _drainBlocked);
		});

		// Delay the writeback such that further dirty pages can accumulate
		// and _progressManagement() can fuse larger requests.
		while(true) {
			uint64_t deadline;
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&mutex);
				if(_writebackExpedited || _stopLoops)
					break;
				deadline = _writebackDeadline;
			}
			// Skip the deadline under memory pressure such that dirty pages become reclaimable immediately.
			// TODO: Pressure is only sampled before we sleep. The reclaimer could expedite the writeback
			//       instead, since it is woken by handleReclaimPressure().
			if(tortureUncaching || physicalAllocator->pressureShortfall())
				break;
			if(!deadline || getClockNanos() >= deadline)
				break;
			co_await async::race_and_cancel(
				[&] (async::cancellation_token ct) {
					return async::transform(
						generalTimerEngine()->sleep(deadline, ct),
						[] (auto) { }
					);
				},
				[&] (async::cancellation_token ct) {
					return async::transform(
						_expediteEvent.async_wait_if([this] () -> bool {
							auto irqLock = frg::guard(&irqMutex());
							auto lock = frg::guard(&mutex);
							return !_stopLoops && !_writebackExpedited;
						}, ct),
						[] (auto) { }
					);
				}
			);
		}

		ManageList mgmtPending;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&mutex);
			if(_stopLoops)
				break;

			// However we left the delay, this pass serves the pending expedite request.
			_writebackExpedited = false;

			bool anyPromoted = false;
			auto it = _dirtyList.begin();
			while(it != _dirtyList.end()) {
				auto *cp = *it++;
				auto *page = frg::container_of(cp, &ManagedPage::cachePage);
				assert(page->transactionState == TxState::dirty);
				assert(!page->discarded || page->discardMode == DiscardMode::keepDirty);
				// Dirty pages hold no slot, so once a claim fails, all further claims fail too.
				if(!claimSlot(page))
					break;
				_dequeueDirty(page);
				page->transactionState = TxState::wantWriteback;
				_writebackList.push_back(cp);
				anyPromoted = true;
			}

			if(!anyPromoted && !_dirtyList.empty())
				_drainBlocked = true;
			if(anyPromoted)
				_progressManagement(mgmtPending);
		}

		while(!mgmtPending.empty()) {
			auto node = mgmtPending.pop_front();
			node->completionEvent.raise();
		}
	}
	_runningLoops.done();
}

coroutine<void> ManagedSpace::_runInvalidationLoop() {
	assert(!isSwapSpace);
	while(true) {
		co_await _discardEvent.async_wait_if([this] () -> bool {
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&mutex);
			return !_stopLoops && _invalidationList.empty();
		});

		CachePagesList batch;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&mutex);
			if(_stopLoops)
				break;

			batch.splice(batch.end(), _invalidationList);
		}

		CachePagesList processed;
		while(!batch.empty()) {
			// Coalesce runs of consecutive identities into a single eviction each.
			auto index = batch.front()->identity;
			size_t count = 0;
			while(!batch.empty() && batch.front()->identity == index + count) {
				processed.push_back(batch.pop_front());
				count++;
			}
			co_await _evictQueue->breakRange(index << kPageShift, count << kPageShift);
		}

		bool raiseDiscard = false;
		bool anyDirty = false;
		bool anyExpedite = false;
		MonitorPendingList pendingMonitors;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&mutex);

			while(!processed.empty()) {
				auto cachePage = processed.pop_front();
				auto *page = frg::container_of(cachePage, &ManagedPage::cachePage);
				assert(page->discarded);
				assert(page->transactionState == TxState::invalidation);
				if(page->discardMode == DiscardMode::keepDirty && page->stillDirty) {
					// Breaking the mappings revealed dirty contents. Write them back.
					page->stillDirty = false;
					_enqueueDirty(page, anyExpedite);
					anyDirty = true;
				} else {
					assert(!page->stillDirty || page->discardMode == DiscardMode::dropDirty);
					page->stillDirty = false;
					auto writebackMonitor = page->detachMonitor(MonitorType::writeback);
					if(writebackMonitor)
						pendingMonitors.push_back(writebackMonitor.release());
					page->transactionState = TxState::none;
					_disposeDiscarded(page, raiseDiscard);
				}
			}
		}
		_raiseMonitors(pendingMonitors);
		if(raiseDiscard)
			_discardEvent.raise();
		if(anyDirty)
			_dirtyEvent.raise();
		if(anyExpedite)
			_expediteEvent.raise();
	}
	_runningLoops.done();
}

bool ManagedSpace::claimSlot(ManagedPage *) {
	// File caches write back to their backing store, so pages always hold their slot.
	return true;
}

void ManagedSpace::_releaseSlot(ManagedPage *) {}

void ManagedSpace::installPage(ManagedPage *page, PhysicalAddr physical, bool dirty,
		unsigned int extraLockCount, bool &raiseDirty, bool &raiseExpedite) {
	assert(page->loadState == LoadState::missing);
	assert(page->transactionState == TxState::none);
	assert(!page->discarded);
	assert(!page->swapCopyValid);
	assert(!isSwapSpace || !page->hasSlot());
	assert(page->physical == PhysicalAddr(-1));

	globalPfnDb().insert(physical, PfnDescriptor::cachePage(&page->cachePage));
	page->physical = physical;
	page->loadState = LoadState::present;
	page->lockCount += extraLockCount;

	if(dirty) {
		_enqueueDirty(page, raiseExpedite);
		// Mirror markDirty()'s wake filter: while the drain coroutine is blocked
		// on swap slots, a page without one cannot be promoted anyway.
		if(!_drainBlocked)
			raiseDirty = true;
	}else if(!page->lockCount
			&& !page->cachePage.useCount.load(std::memory_order_relaxed)) {
		globalReclaimer->addPage(&page->cachePage);
		page->transactionState = TxState::inReclaimer;
	}
}

void ManagedSpace::attachFrame(ManagedPage *page, PhysicalAddr physical) {
	assert(page->loadState == LoadState::missing);
	assert(page->physical == PhysicalAddr(-1));

	globalPfnDb().insert(physical, PfnDescriptor::cachePage(&page->cachePage));
	page->physical = physical;
}

void ManagedSpace::discardPage(ManagedPage *pit, DiscardMode mode, bool &raiseDirty,
		bool &raiseDiscard, bool &raiseExpedite, MonitorPendingList &pendingMonitors) {
	assert(mode != DiscardMode::none);
	if(pit->discarded)
		return;
	pit->discarded = true;
	pit->discardMode = mode;

	bool dispose = false;
	switch(pit->transactionState) {
	case TxState::none:
		dispose = true;
		break;
	case TxState::inReclaimer:
		globalReclaimer->removePage(&pit->cachePage);
		pit->transactionState = TxState::none;
		dispose = true;
		break;
	case TxState::dirty: {
		if(mode == DiscardMode::keepDirty) {
			// The page stays in _dirtyList. The writeback pipeline completes the discard.
			// Expedite writeback so that discard waiters do not sit out the writeback delay.
			if(!_writebackExpedited) {
				_writebackExpedited = true;
				raiseExpedite = true;
			}
			break;
		}
		_dequeueDirty(pit);
		auto writebackMonitor = pit->detachMonitor(MonitorType::writeback);
		if(writebackMonitor)
			pendingMonitors.push_back(writebackMonitor.release());
		pit->transactionState = TxState::none;
		dispose = true;
		break;
	}
	case TxState::wantWriteback: {
		if(mode == DiscardMode::keepDirty) {
			// updateRange() completes the discard once the writeback finishes.
			break;
		}
		_writebackList.erase(_writebackList.iterator_to(&pit->cachePage));
		auto writebackMonitor = pit->detachMonitor(MonitorType::writeback);
		if(writebackMonitor)
			pendingMonitors.push_back(writebackMonitor.release());
		pit->transactionState = TxState::none;
		dispose = true;
		break;
	}
	case TxState::wantInitialization:
	case TxState::initialization:
		// Initialization completes normally (e.g., to allow reading from already locked pages).
		// updateRange() completes the discard.
		break;
	case TxState::writeback:
		// updateRange() completes the discard.
		break;
	case TxState::performReclaim:
	case TxState::avertReclaim:
		// The reclamation coroutine completes the discard, the page
		// is on its local batch list.
		break;
	default:
		// TxState::discardQueued/performDiscard/avertDiscard/invalidation are unreachable:
		// they imply discarded, which the idempotence guard above returns on.
		assert(!"discardPage() on page in unexpected transaction state");
	}

	// Erases entries without a frame right away instead of paying for an RCU grace period.
	if(dispose) {
		assert(pit->transactionState == TxState::none);
		assert(!pit->monitors);
		if(pit->physical == PhysicalAddr(-1)
				&& !pit->lockCount
				&& !pit->cachePage.useCount.load(std::memory_order_relaxed)) {
			_pageDiscarded(pit, raiseDirty);
			_erasePage(pit);
		} else {
			_disposeDiscarded(pit, raiseDiscard);
		}
	}
}

void ManagedSpace::discardPageAndRaise(ManagedPage *page, DiscardMode mode) {
	bool raiseDirty = false;
	bool raiseDiscard = false;
	bool raiseExpedite = false;
	MonitorPendingList pendingMonitors;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex);

		discardPage(page, mode, raiseDirty, raiseDiscard, raiseExpedite, pendingMonitors);
	}
	_raiseMonitors(pendingMonitors);
	if(raiseDirty)
		_dirtyEvent.raise();
	if(raiseDiscard)
		_discardEvent.raise();
	if(raiseExpedite)
		_expediteEvent.raise();
}

void ManagedSpace::_enqueueDirty(ManagedPage *page, bool &raiseExpedite) {
	// The disk copy is stale now; the page obtains a slot again once it is written back.
	_releaseSlot(page);
	page->transactionState = TxState::dirty;
	_dirtyList.push_back(&page->cachePage);
	if(!_writebackDeadline)
		_writebackDeadline = getClockNanos() + writebackDelayNanos;
	// Discard and writebackFence() waiters must not sit out the writeback delay.
	if((page->discarded || page->hasMonitor(MonitorType::writeback))
			&& !_writebackExpedited) {
		_writebackExpedited = true;
		raiseExpedite = true;
	}
}

void ManagedSpace::_dequeueDirty(ManagedPage *page) {
	_dirtyList.erase(_dirtyList.iterator_to(&page->cachePage));
	// The deadline belongs to the batch that just drained; the next batch arms its own.
	if(_dirtyList.empty())
		_writebackDeadline = 0;
}

void ManagedSpace::_disposeDiscarded(ManagedPage *page, bool &raiseDiscard) {
	assert(page->discarded);
	assert(page->transactionState == TxState::none);
	// Only the discard monitor may outlive the page's last transaction.
	assert(!(page->attachedMonitors
			& ~(uint8_t{1} << static_cast<unsigned int>(MonitorType::discard))));
	if(page->lockCount)
		return;
	if(page->cachePage.useCount.load(std::memory_order_relaxed)) {
		// Swap slot identities cannot be translated back to view offsets;
		// hence, swap spaces cannot use TxState::invalidation.
		// Instead, discarded pages will be re-routed to _disposeDiscarded() when their useCount drops to zero.
		if(isSwapSpace)
			return;
		page->transactionState = TxState::invalidation;
		_invalidationList.push_back(&page->cachePage);
		raiseDiscard = true;
		return;
	}
	page->transactionState = TxState::discardQueued;
	_discardList.push_back(&page->cachePage);
	raiseDiscard = true;
}

void ManagedSpace::_raiseMonitors(MonitorPendingList &pendingMonitors) {
	while(!pendingMonitors.empty()) {
		frg::intrusive_shared_ptr<TransactionMonitor, Allocator> monitor{
			frg::adopt_rc, pendingMonitors.pop_front()
		};
		monitor->event.raise();
	}
}

void ManagedSpace::_raiseManagement(ManageList &pendingManagement) {
	while(!pendingManagement.empty()) {
		auto node = pendingManagement.pop_front();
		node->completionEvent.raise();
	}
}

void ManagedSpace::_pageDiscarded(ManagedPage *, bool &) {}

void ManagedSpace::_wakeDrain() {
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex);
		_drainBlocked = false;
	}
	_dirtyEvent.raise();
}

// --------------------------------------------------------
// SwapSpace
// --------------------------------------------------------

std::expected<smarter::shared_ptr<SwapSpace>, Error> SwapSpace::create(
		smarter::shared_ptr<Hierarchy> hierarchy) {
	smarter::borrowed_ptr<Hierarchy> hierarchyPtr{hierarchy};
	auto self = allocate_rcu_shared<SwapSpace>(HierarchyAllocator{std::move(hierarchy)},
			hierarchyPtr);
	self->selfPtr = self;
	globalReclaimer->registerBundle(self);
	self->_runningLoops.add(2);
	spawnOnWorkQueue(*kernelAlloc, WorkQueue::memoryQueue().lock(), self->_runReclaimLoop());
	spawnOnWorkQueue(*kernelAlloc, WorkQueue::memoryQueue().lock(), self->_runDrainLoop());
	return self;
}

SwapSpace::SwapSpace(smarter::borrowed_ptr<Hierarchy> hierarchy)
: ManagedSpace{std::move(hierarchy), size_t{maxRegions} << (regionShift + kPageShift), false} {
	isSwapSpace = true;
}

SwapSpace::~SwapSpace() {
	AccountAllocator alloc{hierarchy->controller()};
	auto region = _regions.first();
	while(region) {
		auto next = RegionTree::successor(region);
		frg::destruct(alloc, region);
		region = next;
	}
}

SwapSpace::Region::Region(unsigned int regionIndex, size_t numSlots, AccountAllocator alloc)
: regionIndex{regionIndex}, numSlots{numSlots}, metadata{alloc} {
	assert(numSlots);
	auto base = uint64_t{regionIndex} << regionShift;
	auto tableOrder = BuddyAccessor::suitableOrder(numSlots);
	auto numRoots = (numSlots + (size_t{1} << tableOrder) - 1) >> tableOrder;
	metadata.resize(BuddyAccessor::determineSize(numRoots, tableOrder));
	BuddyAccessor::initialize(metadata.data(), numRoots, tableOrder);
	accessor = BuddyAccessor{base, 0, metadata.data(), numRoots, tableOrder};

	// The table covers a multiple of 2^tableOrder slots; reserve the excess beyond numSlots
	// (decomposed into aligned blocks) so that it is never handed out.
	auto covered = size_t{numRoots} << tableOrder;
	auto index = numSlots;
	while(index < covered) {
		int order = std::countr_zero(index);
		while(index + (size_t{1} << order) > covered)
			order--;
		accessor.reserve(base + index, order);
		index += size_t{1} << order;
	}
}

bool SwapSpace::claimSlot(ManagedPage *page) {
	if(page->hasSlot())
		return true;
	auto offset = _allocateOffset();
	if(!offset)
		return false;
	page->cachePage.identity = *offset;
	pages.insert(*offset, page);
	return true;
}

void SwapSpace::_releaseSlot(ManagedPage *page) {
	if(!page->hasSlot())
		return;
	// The daemon cannot reach the slot anymore, so the slot can be reused.
	assert(!_slotStates.find(page->cachePage.identity));
	pages.erase(page->cachePage.identity);
	_freeOffset(page->cachePage.identity);
	page->cachePage.identity = noSlot;
	_drainBlocked = false;
}

void SwapSpace::_pageDiscarded(ManagedPage *page, bool &raiseDirty) {
	if(!page->hasSlot())
		return;
	_releaseSlot(page);
	raiseDirty = true;
}

coroutine<std::expected<void, Error>> SwapSpace::setRegionCapacity(unsigned int region,
		size_t numSlots) {
	if(region >= maxRegions || numSlots > maxRegionSlots)
		co_return std::unexpected{Error::illegalArgs};

	size_t capacity = 0;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex);
		if(auto existing = _findRegion(region); existing)
			capacity = existing->numSlots;
	}
	if(numSlots == capacity)
		co_return {};
	// TODO: Growing requires a larger allocator; shrinking requires the pages in the
	//       removed slots to be brought back into RAM.
	if(capacity)
		co_return std::unexpected{Error::unsupportedOperation};

	// Build the allocator outside of the mutex since it allocates its metadata.
	AccountAllocator alloc{hierarchy->controller()};
	auto attached = frg::construct<Region>(alloc, region, numSlots, alloc);
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex);
		if(auto existing = _findRegion(region); existing) {
			capacity = existing->numSlots;
		}else{
			_regions.insert(attached);
			attached = nullptr;
		}
	}
	if(attached) {
		// A concurrent call attached the region in the meantime.
		frg::destruct(*kernelAlloc, attached);
		if(numSlots != capacity)
			co_return std::unexpected{Error::unsupportedOperation};
		co_return {};
	}
	_wakeDrain();
	co_return {};
}

ManagedSpace::ManagedPage *SwapSpace::allocatePage() {
	AccountAllocator alloc{hierarchy->controller()};
	return frg::construct<ManagedPage>(alloc, this, noSlot);
}

SwapSpace::Region *SwapSpace::_findRegion(unsigned int regionIndex) {
	auto current = _regions.get_root();
	while(current) {
		if(regionIndex < current->regionIndex) {
			current = RegionTree::get_left(current);
		}else if(regionIndex > current->regionIndex) {
			current = RegionTree::get_right(current);
		}else{
			return current;
		}
	}
	return nullptr;
}

frg::optional<uint64_t> SwapSpace::_allocateOffset() {
	for(auto region = _regions.first(); region; region = RegionTree::successor(region)) {
		auto offset = region->accessor.allocate(0, 64);
		if(offset != BuddyAccessor::illegalAddress)
			return offset;
	}
	return frg::null_opt;
}

void SwapSpace::_freeOffset(uint64_t offset) {
	auto region = _findRegion(offset >> regionShift);
	assert(region);
	region->accessor.free(offset, 0);
}

coroutine<void> ManagedSpace::dispose() {
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex);
		_stopLoops = true;
	}
	_dirtyEvent.raise();
	_discardEvent.raise();
	_expediteEvent.raise();

	co_await _runningLoops.wait();

	// No views exist anymore, hence there are no waiters, managers or lock holders.
	assert(_managementQueue.empty());

	// Discarded swap pages that hold no slot are not indexed by pages.
	CachePagesList unindexed;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex);

		while(!_discardList.empty()) {
			auto cachePage = _discardList.pop_front();
			if(!frg::container_of(cachePage, &ManagedPage::cachePage)->hasSlot())
				unindexed.push_back(cachePage);
		}

		// The pages themselves are released below.
		_initializationList.clear();
		_dirtyList.clear();
		_writebackList.clear();
		_discardList.clear();
		_invalidationList.clear();
	}

	// Destroys the page and returns its frame, which the caller frees outside of the mutex.
	// Must be called under mutex.
	auto releasePage = [this] (ManagedPage *pit) -> PhysicalAddr {
		assert(!pit->lockCount);
		assert(!pit->cachePage.useCount.load(std::memory_order_relaxed));
		if(pit->transactionState == TxState::inReclaimer)
			globalReclaimer->removePage(&pit->cachePage);
		// Abandoned transactions can leave monitors attached; their waiters are gone.
		pit->detachMonitor(MonitorType::initialization);
		pit->detachMonitor(MonitorType::writeback);
		pit->detachMonitor(MonitorType::discard);
		auto physical = pit->physical;
		if(physical != PhysicalAddr(-1))
			globalPfnDb().erase(physical);
		_erasePage(pit);
		return physical;
	};

	while(!unindexed.empty()) {
		PhysicalAddr physical = PhysicalAddr(-1);
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&mutex);

			physical = releasePage(frg::container_of(unindexed.pop_front(),
					&ManagedPage::cachePage));
		}
		if(physical != PhysicalAddr(-1))
			deallocateAndUncharge(hierarchy.get(), physical, kPageSize, PhysicalKind::evictable);
	}

	uint64_t nextIdentity = 0;
	while(true) {
		PhysicalAddr physical = PhysicalAddr(-1);
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&mutex);

			auto it = pages.lower_bound(nextIdentity);
			if(it == pages.end())
				break;
			nextIdentity = (*it)->cachePage.identity + 1;
			physical = releasePage(*it);
		}
		if(physical != PhysicalAddr(-1))
			deallocateAndUncharge(hierarchy.get(), physical, kPageSize, PhysicalKind::evictable);
	}

	globalReclaimer->unregisterBundle(this);
}

ManagedSpace::~ManagedSpace() {
	// All cleanup has already been done by dispose().
}

ManagedSpace::ManagedPage *ManagedSpace::findPage(uint64_t index) {
	auto it = pages.find(index);
	if(!it)
		return nullptr;
	return *it;
}

ManagedSpace::ManagedPage *ManagedSpace::findOrInsertPage(uint64_t index) {
	auto [it, wasInserted] = pages.find_or_insert(index, nullptr);
	if(wasInserted) {
		AccountAllocator alloc{hierarchy->controller()};
		*it = frg::construct<ManagedPage>(alloc, this, index);
	}
	return *it;
}

bool ManagedSpace::isHandedToManager(ManagedPage *page) {
	return page->transactionState == TxState::initialization
			|| page->transactionState == TxState::writeback;
}

void ManagedSpace::_erasePage(ManagedPage *page) {
	if(page->hasSlot())
		pages.erase(page->cachePage.identity);
	frg::destruct(*kernelAlloc, page);
}

// Note: Neither offset nor size are necessarily multiples of the page size.
Error ManagedSpace::lockPages(uintptr_t offset, size_t size) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&mutex);

	for(size_t pg = 0; pg < size; pg += kPageSize) {
		size_t index = (offset + pg) / kPageSize;
		if(!isSwapSpace) {
			lockPage(findOrInsertPage(index));
			continue;
		}
		// On swap spaces, the daemon may only access pages while it serves their transactions.
		auto pit = findPage(index);
		if(!pit || !isHandedToManager(pit)) {
			// Unwind the locks we already took.
			for(size_t upg = 0; upg < pg; upg += kPageSize)
				_unlockSlot(findPage((offset + upg) / kPageSize));
			return Error::fault;
		}
		_lockSlot(pit);
	}
	return Error::success;
}

// Note: Neither offset nor size are necessarily multiples of the page size.
void ManagedSpace::unlockPages(uintptr_t offset, size_t size) {
	bool raiseDiscard = false;
	{
		auto irq_lock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex);

		for(size_t pg = 0; pg < size; pg += kPageSize) {
			size_t index = (offset + pg) / kPageSize;
			auto pit = findPage(index);
			assert(pit);
			if(isSwapSpace) {
				_unlockSlot(pit);
			}else{
				unlockPage(pit, raiseDiscard);
			}
		}
	}
	if(raiseDiscard)
		_discardEvent.raise();
}

void ManagedSpace::lockPage(ManagedPage *page) {
	page->lockCount++;
	if(page->lockCount == 1) {
		if(page->loadState == LoadState::present && page->transactionState == TxState::inReclaimer) {
			globalReclaimer->removePage(&page->cachePage);
			page->transactionState = TxState::none;
		}else if(page->transactionState == TxState::performReclaim) {
			page->transactionState = TxState::avertReclaim;
		}else if(page->transactionState == TxState::discardQueued
				|| page->transactionState == TxState::performDiscard) {
			// Handle discardQueued pages by moving them into avertDiscard.
			// This avoids raising monitors on this code path.
			page->transactionState = TxState::avertDiscard;
		}
	}
}

void ManagedSpace::unlockPageAndRaise(ManagedPage *page, bool dirty) {
	bool needsEvent = false;
	bool needsExpedite = false;
	bool raiseDiscard = false;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex);

		if(dirty)
			markDirtyPage(page, needsEvent, needsExpedite);
		unlockPage(page, raiseDiscard);
	}
	if(needsEvent)
		_dirtyEvent.raise();
	if(needsExpedite)
		_expediteEvent.raise();
	if(raiseDiscard)
		_discardEvent.raise();
}

void ManagedSpace::_lockSlot(ManagedPage *page) {
	assert(isSwapSpace);
	auto [state, wasInserted] = _slotStates.find_or_insert(page->cachePage.identity);
	state->locks++;
}

void ManagedSpace::_unlockSlot(ManagedPage *page) {
	assert(isSwapSpace);
	auto identity = page->cachePage.identity;
	auto state = _slotStates.find(identity);
	assert(state && state->locks);
	state->locks--;
	if(!state->locks)
		_slotStates.erase(identity);
}

void ManagedSpace::_unlockSlotAndRaise(ManagedPage *page, bool dirty) {
	bool needsEvent = false;
	bool needsExpedite = false;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex);

		if(dirty)
			markDirtyPage(page, needsEvent, needsExpedite);
		_unlockSlot(page);
	}
	if(needsEvent)
		_dirtyEvent.raise();
	if(needsExpedite)
		_expediteEvent.raise();
}

void ManagedSpace::unlockPage(ManagedPage *page, bool &raiseDiscard) {
	assert(page->lockCount > 0);
	page->lockCount--;
	if(!page->lockCount) {
		if(page->discarded) {
			// If a transaction is still in flight, the page stays owned by it;
			// the transaction's completion path disposes of the page instead.
			if(page->transactionState == TxState::none)
				_disposeDiscarded(page, raiseDiscard);
		} else if(page->loadState == LoadState::present
				&& page->transactionState == TxState::none
				&& !page->cachePage.useCount.load(std::memory_order_relaxed)) {
			globalReclaimer->addPage(&page->cachePage);
			page->transactionState = TxState::inReclaimer;
		}
	}
}

PhysicalAddr ManagedSpace::peekPage(ManagedPage *page) {
	if(page->loadState != LoadState::present) {
		assert(page->loadState == LoadState::missing);
		return PhysicalAddr(-1);
	}
	assert(page->physical != PhysicalAddr(-1));

	if(page->transactionState == TxState::performReclaim) {
		page->transactionState = TxState::avertReclaim;
	} else if(page->transactionState == TxState::performDiscard) {
		page->transactionState = TxState::avertDiscard;
	}

	return page->physical;
}

bool ManagedSpace::touchPresentPage(ManagedPage *page) {
	if(page->loadState != LoadState::present) {
		assert(page->loadState == LoadState::missing);
		return false;
	}
	assert(page->physical != PhysicalAddr(-1));

	if(page->transactionState == TxState::inReclaimer) {
		globalReclaimer->bumpPage(&page->cachePage);
	}else if(page->transactionState == TxState::performReclaim) {
		page->transactionState = TxState::avertReclaim;
	}else if(page->transactionState == TxState::performDiscard) {
		page->transactionState = TxState::avertDiscard;
	}

	return true;
}

std::expected<frg::intrusive_shared_ptr<ManagedSpace::TransactionMonitor, Allocator>, Error>
ManagedSpace::initializePage(ManagedPage *page, FetchFlags flags, PhysicalAddr &freshPhysical,
		ReadaheadFrames *readaheadFrames, ManageList &pendingManagement) {
	assert(page->loadState == LoadState::missing);
	assert(!isSwapSpace || (page->swapCopyValid && page->hasSlot()));
	assert(!readahead || readaheadFrames);

	if(flags & fetchDisallowBacking) {
		urgentLogger() << "thor: Backing of page is disallowed" << frg::endlog;
		return std::unexpected{Error::fault};
	}

	auto index = page->cachePage.identity;
	if(readaheadFrames)
		readaheadFrames->missing = 0;

	if(page->physical == PhysicalAddr(-1)) {
		if(freshPhysical == PhysicalAddr(-1)) {
			// Let the caller allocate the readahead frames together with the page's frame.
			if(readahead) {
				size_t numFrameless = 0;
				for(size_t i = 1; i <= numReadaheadPages && index + i < numPages; ++i) {
					auto pit = findPage(index + i);
					if(!pit || (pit->loadState == LoadState::missing
							&& pit->transactionState == TxState::none
							&& pit->physical == PhysicalAddr(-1)))
						++numFrameless;
				}
				readaheadFrames->missing = numFrameless
						- frg::min(numFrameless, readaheadFrames->physicals.size());
			}
			return frg::intrusive_shared_ptr<TransactionMonitor, Allocator>{};
		}
		attachFrame(page, freshPhysical);
		freshPhysical = PhysicalAddr(-1);
	}

	if(page->transactionState == TxState::none) {
		page->transactionState = TxState::wantInitialization;
		_initializationList.push_back(&page->cachePage);
	}

	// Perform readahead.
	// It is best effort: pages that the caller provides no frames for are skipped.
	if(readahead) {
		assert(!isSwapSpace);
		auto &physicals = readaheadFrames->physicals;
		size_t nextFrame = 0;
		for(size_t i = 1; i <= numReadaheadPages; ++i) {
			if(!(index + i < numPages))
				break;
			auto pit = findOrInsertPage(index + i);
			if(pit->loadState == LoadState::missing
					&& pit->transactionState == TxState::none) {
				if(pit->physical == PhysicalAddr(-1)) {
					if(nextFrame == physicals.size()) {
						++readaheadFrames->missing;
						continue;
					}
					attachFrame(pit, physicals[nextFrame]);
					physicals[nextFrame++] = PhysicalAddr(-1);
				}
				pit->transactionState = TxState::wantInitialization;
				_initializationList.push_back(&pit->cachePage);
			}
		}
	}

	_progressManagement(pendingManagement);

	assert(page->transactionState == TxState::wantInitialization
			|| page->transactionState == TxState::initialization);
	return page->requireMonitor(MonitorType::initialization);
}

void ManagedSpace::submitManagement(ManageNode *node) {
	ManageList pending;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex);

		_managementQueue.push_back(node);
		_progressManagement(pending);
	}

	_raiseManagement(pending);
}

void ManagedSpace::_progressManagement(ManageList &pending) {
	// For now, we prefer writeback to initialization.
	// "Proper" priorization should probably be done in the userspace driver
	// (we do not want to store per-page priorities here).

	while(!_writebackList.empty() && !_managementQueue.empty()) {
		auto page = _writebackList.front();
		auto index = page->identity;
		assert(index != noSlot);

		// Fuse the request with adjacent pages in the list.
		ptrdiff_t count = 0;
		while(!_writebackList.empty()) {
			auto fuse_cache_page = _writebackList.front();
			auto fuse_index = fuse_cache_page->identity;
			auto fuse_managed_page = frg::container_of(fuse_cache_page, &ManagedPage::cachePage);
			if(fuse_index != index + count)
				break;
			assert(fuse_managed_page->transactionState == TxState::wantWriteback);
			fuse_managed_page->transactionState = TxState::writeback;
			count++;
			_writebackList.pop_front();
		}
		assert(count);

		auto node = _managementQueue.pop_front();
		node->setup(Error::success, ManageRequest::writeback,
				index << kPageShift, count << kPageShift);
		pending.push_back(node);
	}

	while(!_initializationList.empty() && !_managementQueue.empty()) {
		auto page = _initializationList.front();
		auto index = page->identity;
		assert(index != noSlot);

		// Fuse the request with adjacent pages in the list.
		ptrdiff_t count = 0;
		while(!_initializationList.empty()) {
			auto fuse_cache_page = _initializationList.front();
			auto fuse_index = fuse_cache_page->identity;
			auto fuse_managed_page = frg::container_of(fuse_cache_page, &ManagedPage::cachePage);
			if(fuse_index != index + count)
				break;
			assert(fuse_managed_page->transactionState == TxState::wantInitialization);
			fuse_managed_page->transactionState = TxState::initialization;
			count++;
			_initializationList.pop_front();
		}
		assert(count);

		auto node = _managementQueue.pop_front();
		node->setup(Error::success, ManageRequest::initialize,
				index << kPageShift, count << kPageShift);
		pending.push_back(node);
	}
}


void ManagedSpace::incrementUses(CachePage *cachePage) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&mutex);

	auto page = frg::container_of(cachePage, &ManagedPage::cachePage);

	auto cnt = cachePage->useCount.fetch_add(1, std::memory_order_acquire);
	if(!cnt) {
		if(page->loadState == LoadState::present
				&& page->transactionState == TxState::inReclaimer) {
			globalReclaimer->removePage(cachePage);
			page->transactionState = TxState::none;
		} else if(page->transactionState == TxState::performReclaim) {
			page->transactionState = TxState::avertReclaim;
		} else if(page->transactionState == TxState::discardQueued
				|| page->transactionState == TxState::performDiscard) {
			// Handle discardQueued pages by moving them into avertDiscard.
			// This avoids raising monitors on this code path.
			page->transactionState = TxState::avertDiscard;
		}
	}
}

void ManagedSpace::decrementUses(CachePage *cachePage) {
	bool raiseDiscard = false;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex);

		auto page = frg::container_of(cachePage, &ManagedPage::cachePage);

		auto cnt = cachePage->useCount.fetch_sub(1, std::memory_order_release);
		assert(cnt > 0);
		if(cnt == 1) {
			if(page->discarded) {
				// If a transaction is still in flight, the page stays owned by it;
				// the transaction's completion path disposes of the page instead.
				if(page->transactionState == TxState::none)
					_disposeDiscarded(page, raiseDiscard);
			} else if(page->loadState == LoadState::present
					&& page->transactionState == TxState::none
					&& !page->lockCount) {
				globalReclaimer->addPage(cachePage);
				page->transactionState = TxState::inReclaimer;
			}
		}
	}
	if(raiseDiscard)
		_discardEvent.raise();
}

void ManagedSpace::markDirty(CachePage *cachePage) {
	bool needsEvent = false;
	bool needsExpedite = false;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex);

		auto page = frg::container_of(cachePage, &ManagedPage::cachePage);
		markDirtyPage(page, needsEvent, needsExpedite);
	}

	if(needsEvent)
		_dirtyEvent.raise();
	if(needsExpedite)
		_expediteEvent.raise();
}

void ManagedSpace::markDirtyPage(ManagedPage *page, bool &needsEvent, bool &needsExpedite) {
	if(page->loadState == LoadState::missing)
		return;

	// Data discarded without writeback must not re-enter the writeback machinery.
	if(page->discarded) {
		if(page->discardMode == DiscardMode::dropDirty)
			return;
		assert(page->discardMode == DiscardMode::keepDirty);
	}

	// The in-memory contents now diverge from the backing store's copy.
	page->swapCopyValid = false;

	if(page->loadState == LoadState::present
			&& (page->transactionState == TxState::none
				|| page->transactionState == TxState::inReclaimer)) {
		if(page->transactionState == TxState::inReclaimer)
			globalReclaimer->removePage(&page->cachePage);
		_enqueueDirty(page, needsExpedite);
		// While the drain coroutine is blocked on swap slots, a page without one
		// cannot be promoted anyway (_releaseSlot() unblocks the drain if it freed a slot).
		if(!_drainBlocked)
			needsEvent = true;
	} else if(page->transactionState == TxState::performReclaim
			|| page->transactionState == TxState::avertReclaim) {
		page->transactionState = TxState::avertReclaim;
		page->stillDirty = true;
	} else if(page->transactionState == TxState::writeback) {
		page->stillDirty = true;
	} else if(page->transactionState == TxState::invalidation
			|| page->transactionState == TxState::avertDiscard) {
		// Only reachable on DiscardMode::keepDirty pages.
		// The discard machinery re-routes them to the writeback pipeline.
		page->stillDirty = true;
	} else if(page->transactionState == TxState::discardQueued
			|| page->transactionState == TxState::performDiscard) {
		// Only reachable on DiscardMode::keepDirty pages without uses or locks (via markDirtyRange()).
		page->transactionState = TxState::avertDiscard;
		page->stillDirty = true;
	} else {
		assert(page->transactionState == TxState::dirty
				|| page->transactionState == TxState::wantWriteback);
	}
}

// --------------------------------------------------------
// BackingMemory
// --------------------------------------------------------

std::expected<smarter::shared_ptr<BackingMemory>, Error> BackingMemory::create(
		smarter::shared_ptr<ManagedSpace> managed) {
	auto ptr = allocate_rcu_shared<BackingMemory>(HierarchyAllocator{managed->hierarchy.lock()},
			CtorToken{}, std::move(managed));
	return ptr;
}

// Note: This resizes the ManagedSpace but it does not affect BackingMemory::getLength().
// On shrink, the caller is responsible for discarding the truncated pages
// (e.g., via invalidateRange()) before it reuses their backing store.
coroutine<frg::expected<Error>> BackingMemory::resize(size_t newSize) {
	assert(currentIpl() == ipl::exceptionalWork);
	if(_managed->isSwapSpace)
		co_return Error::illegalObject;
	if(newSize > backingMemoryLength)
		co_return Error::illegalArgs;
	if(newSize & (kPageSize - 1))
		co_return Error::illegalArgs;
	auto newPages = newSize >> kPageShift;

	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_managed->mutex);

		_managed->numPages = newPages;
	}

	co_return {};
}

Error BackingMemory::lockRange(uintptr_t offset, size_t size) {
	if(offset > backingMemoryLength || size > backingMemoryLength - offset)
		return Error::bufferTooSmall;
	return _managed->lockPages(offset, size);
}

void BackingMemory::unlockRange(uintptr_t offset, size_t size) {
	_managed->unlockPages(offset, size);
}

PhysicalRange BackingMemory::peekRange(uintptr_t offset, FetchFlags) {
	auto index = offset >> kPageShift;
	auto misalign = offset & (kPageSize - 1);

	if(offset >= backingMemoryLength)
		return PhysicalRange{};

	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_managed->mutex);

	auto pit = _managed->findPage(index);
	if(!pit || (_managed->isSwapSpace && !_managed->isHandedToManager(pit)))
		return PhysicalRange{};

	if(pit->transactionState == ManagedSpace::TxState::performReclaim) {
		pit->transactionState = ManagedSpace::TxState::avertReclaim;
	} else if(pit->transactionState == ManagedSpace::TxState::performDiscard) {
		pit->transactionState = ManagedSpace::TxState::avertDiscard;
	}

	return PhysicalRange{
		.physical = pit->physical + misalign,
		.size = kPageSize - misalign,
		.cachingMode = CachingMode::null,
		.isMutable = true
	};
}

std::expected<size_t, Error> BackingMemory::accessRange(uintptr_t offset, size_t size,
		FetchFlags flags, PageAccessFn fn) {
	auto index = offset >> kPageShift;
	auto misalign = offset & (kPageSize - 1);

	if(offset >= backingMemoryLength)
		return std::unexpected{Error::fault};

	ManagedSpace::ManagedPage *pit;
	PhysicalAddr physical;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_managed->mutex);

		pit = _managed->findPage(index);
		if(!pit || (_managed->isSwapSpace && !_managed->isHandedToManager(pit))
				|| pit->physical == PhysicalAddr(-1))
			return 0;
		if(_managed->isSwapSpace) {
			_managed->_lockSlot(pit);
		}else{
			_managed->lockPage(pit);
		}
		physical = pit->physical;
	}

	auto chunk = frg::min(size, kPageSize - misalign);
	PageAccessResult result;
	{
		PageAccessor accessor{physical};
		result = fn(reinterpret_cast<uint8_t *>(accessor.get()) + misalign, chunk);
	}
	assert(!result.dirty || (flags & fetchRequireMutable));
	if(_managed->isSwapSpace) {
		_managed->_unlockSlotAndRaise(pit, result.dirty);
	}else{
		_managed->unlockPageAndRaise(pit, result.dirty);
	}
	return chunk;
}

coroutine<frg::expected<Error, size_t>>
BackingMemory::touchRange(uintptr_t offset, size_t, FetchFlags flags) {
	assert(currentIpl() == ipl::exceptionalWork);

	auto index = offset >> kPageShift;
	auto misalign = offset & (kPageSize - 1);

	if(offset >= backingMemoryLength)
		co_return Error::fault;

	// Frame that is allocated and zeroed outside of the mutex, then attached under the mutex.
	PhysicalAddr freshPhysical(-1);
	frg::scope_exit freeFreshFrame{[&] {
		if(freshPhysical != PhysicalAddr(-1))
			deallocateAndUncharge(_managed->hierarchy.get(), freshPhysical, kPageSize,
					PhysicalKind::evictable);
	}};

	while(true) {
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&_managed->mutex);

			// On swap spaces, the daemon may only access pages while it serves their transactions.
			auto pit = _managed->isSwapSpace
					? _managed->findPage(index)
					: _managed->findOrInsertPage(index);
			if(!pit || (_managed->isSwapSpace && !_managed->isHandedToManager(pit)))
				co_return Error::fault;

			if(pit->transactionState == ManagedSpace::TxState::performReclaim) {
				pit->transactionState = ManagedSpace::TxState::avertReclaim;
			} else if(pit->transactionState == ManagedSpace::TxState::performDiscard) {
				pit->transactionState = ManagedSpace::TxState::avertDiscard;
			}

			if(pit->physical != PhysicalAddr(-1))
				co_return kPageSize - misalign;
			// The daemon only reaches pages in transactions, which always have a frame.
			assert(!_managed->isSwapSpace);

			if(freshPhysical != PhysicalAddr(-1)) {
				_managed->attachFrame(pit, freshPhysical);
				freshPhysical = PhysicalAddr(-1);
				co_return kPageSize - misalign;
			}
		}

		// Faulting threads attach frames before they request initialization (see initializePage()),
		// so this is only reached if the manager touches pages outside of initialization.
		auto physicalOutcome = co_await chargeAndAllocate(_managed->hierarchy.get(),
				PhysicalKind::evictable, flags);
		if(!physicalOutcome)
			co_return physicalOutcome.error();
		freshPhysical = *physicalOutcome;
		PageAccessor accessor{freshPhysical};
		memset(accessor.get(), 0, kPageSize);
	}
}

size_t BackingMemory::getLength() {
	return backingMemoryLength;
}

coroutine<frg::expected<Error, MemoryNotification>> BackingMemory::pollNotification() {
	ManageNode node;
	_managed->submitManagement(&node);
	co_await node.completionEvent.wait();
	if(node.error() != Error::success)
		co_return node.error();
	co_return MemoryNotification{node.type(), node.offset(), node.size()};
}

Error BackingMemory::updateRange(ManageRequest type, size_t offset, size_t length) {
	if (offset & (kPageSize - 1))
		return Error::illegalArgs;
	if (length & (kPageSize - 1))
		return Error::illegalArgs;

	ManagedSpace::MonitorPendingList pendingMonitors;
	ManageList pendingManagement;
	bool raiseDiscard = false;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_managed->mutex);

		// Validate the whole range before updating any page such that failure is atomic.
		ManagedSpace::TxState expectedState;
		if (type == ManageRequest::initialize) {
			expectedState = ManagedSpace::TxState::initialization;
		} else if (type == ManageRequest::writeback) {
			expectedState = ManagedSpace::TxState::writeback;
		} else {
			return Error::illegalArgs;
		}
		for(size_t pg = 0; pg < length; pg += kPageSize) {
			size_t index = (offset + pg) / kPageSize;
			auto pit = _managed->findPage(index);
			if(!pit || pit->transactionState != expectedState)
				return Error::illegalArgs;
		}

		// On swap spaces, the daemon must not reach the pages once their transactions complete.
		// Mappings that are attached later cannot fault the pages in: peekRange() takes the mutex.
		if(_managed->isSwapSpace) {
			for(size_t pg = 0; pg < length; pg += kPageSize) {
				size_t index = (offset + pg) / kPageSize;
				if(_managed->_slotStates.find(index))
					return Error::illegalState;
			}
			if(length && _managed->_evictQueue->isObserved(offset, length))
				return Error::illegalState;
		}

		if(type == ManageRequest::initialize) {
			for(size_t pg = 0; pg < length; pg += kPageSize) {
				size_t index = (offset + pg) / kPageSize;
				auto pit = _managed->findPage(index);
				pit->loadState = ManagedSpace::LoadState::present;
				auto monitor = pit->detachMonitor(ManagedSpace::MonitorType::initialization);
				if(monitor)
					pendingMonitors.push_back(monitor.release());
				if(pit->discarded) {
					// The page completed initialization normally but will now be discarded.
					pit->transactionState = ManagedSpace::TxState::none;
					_managed->_disposeDiscarded(pit, raiseDiscard);
				} else if (pit->lockCount || pit->cachePage.useCount.load(std::memory_order_relaxed)) {
					pit->transactionState = ManagedSpace::TxState::none;
				} else {
					globalReclaimer->addPage(&pit->cachePage);
					pit->transactionState = ManagedSpace::TxState::inReclaimer;
				}
			}
		}else{
			for(size_t pg = 0; pg < length; pg += kPageSize) {
				size_t index = (offset + pg) / kPageSize;
				auto pit = _managed->findPage(index);

				if(pit->discarded) {
					if(pit->discardMode == DiscardMode::dropDirty) {
						// Raise the monitor as usual - writebackFence() waiters hold references to it.
						auto monitor = pit->detachMonitor(ManagedSpace::MonitorType::writeback);
						if(monitor)
							pendingMonitors.push_back(monitor.release());
						// The frame is being discarded so it doesn't need to be written back.
						pit->stillDirty = false;
						pit->transactionState = ManagedSpace::TxState::none;
						_managed->_disposeDiscarded(pit, raiseDiscard);
						continue;
					}
					assert(pit->discardMode == DiscardMode::keepDirty);
				}
				if(!pit->stillDirty) {
					// The backing store now holds the page's current contents.
					pit->swapCopyValid = true;
					auto monitor = pit->detachMonitor(ManagedSpace::MonitorType::writeback);
					if(monitor)
						pendingMonitors.push_back(monitor.release());
					if(pit->discarded) {
						pit->transactionState = ManagedSpace::TxState::none;
						_managed->_disposeDiscarded(pit, raiseDiscard);
					} else if (pit->lockCount || pit->cachePage.useCount.load(std::memory_order_relaxed)) {
						pit->transactionState = ManagedSpace::TxState::none;
					} else {
						globalReclaimer->addPage(&pit->cachePage);
						pit->transactionState = ManagedSpace::TxState::inReclaimer;
					}
				}else{
					pit->stillDirty = false;
					pit->transactionState = ManagedSpace::TxState::wantWriteback;
					_managed->_writebackList.push_back(&pit->cachePage);
					auto monitor = pit->detachMonitor(ManagedSpace::MonitorType::writeback);
					if(monitor)
						pendingMonitors.push_back(monitor.release());
				}
			}
		}

		// Re-queued writebacks must not wait for an unrelated _progressManagement() call.
		_managed->_progressManagement(pendingManagement);
	}

	if(raiseDiscard)
		_managed->_discardEvent.raise();

	ManagedSpace::_raiseMonitors(pendingMonitors);

	while(!pendingManagement.empty()) {
		auto node = pendingManagement.pop_front();
		node->completionEvent.raise();
	}

	return Error::success;
}

Error BackingMemory::markDirtyRange(size_t offset, size_t length) {
	if(_managed->isSwapSpace)
		return Error::illegalObject;
	if (offset & (kPageSize - 1))
		return Error::illegalArgs;
	if (length & (kPageSize - 1))
		return Error::illegalArgs;
	if (offset > backingMemoryLength || length > backingMemoryLength - offset)
		return Error::bufferTooSmall;

	auto limitPage = (offset + length) >> kPageShift;

	bool needsEvent = false;
	bool needsExpedite = false;
	uint64_t cursor = offset >> kPageShift;
	bool exhausted = false;
	while(!exhausted) {
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_managed->mutex);

		// Skip over absent entries and scan in chunks
		// so that the spinlock is not held for an unbounded time.
		auto it = _managed->pages.lower_bound(cursor);
		for(size_t i = 0; i < markDirtyChunkSize; ++i) {
			if(it == _managed->pages.end() || (*it)->cachePage.identity >= limitPage) {
				exhausted = true;
				break;
			}
			auto *pit = *it;
			++it;
			cursor = pit->cachePage.identity + 1;
			_managed->markDirtyPage(pit, needsEvent, needsExpedite);
		}
	}

	if(needsEvent)
		_managed->_dirtyEvent.raise();
	if(needsExpedite)
		_managed->_expediteEvent.raise();

	return Error::success;
}

coroutine<frg::expected<Error>> BackingMemory::writebackFence(uintptr_t offset, size_t size) {
	assert(currentIpl() == ipl::exceptionalWork);
	if(_managed->isSwapSpace)
		co_return Error::illegalObject;
	if (offset & (kPageSize - 1))
		co_return Error::illegalArgs;
	if (size & (kPageSize - 1))
		co_return Error::illegalArgs;
	if (offset > backingMemoryLength || size > backingMemoryLength - offset)
		co_return Error::bufferTooSmall;

	if(!size)
		co_return {};

	// Mapped stores may only have dirtied PTEs, without marking the managed
	// pages dirty yet. Collect them before waiting for the resulting writeback
	// transactions.
	co_await _managed->_evictQueue->cleanRange(offset, size);

	// Note that writebackFence() expedites writeback
	// (otherwise, callers would need to wait for the full writebackDelayNanos).

	auto limitPage = (offset + size) >> kPageShift;

	uint64_t cursor = offset >> kPageShift;
	while(cursor < limitPage) {
		uint64_t index;
		frg::intrusive_shared_ptr<ManagedSpace::TransactionMonitor, Allocator> monitor;
		bool needSecond = false;
		bool raiseExpedite = false;
		bool exhausted = false;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&_managed->mutex);

			// Skip over absent entries and scan in chunks
			// so that the spinlock is not held for an unbounded time.
			auto it = _managed->pages.lower_bound(cursor);
			for(size_t i = 0; i < fenceChunkSize; ++i) {
				if(it == _managed->pages.end() || (*it)->cachePage.identity >= limitPage) {
					exhausted = true;
					break;
				}
				auto *pit = *it;
				++it;
				index = pit->cachePage.identity;
				cursor = index + 1;
				if(!pit->hasUnwrittenData())
					continue;
				monitor = pit->requireMonitor(ManagedSpace::MonitorType::writeback);
				needSecond = pit->transactionState == ManagedSpace::TxState::writeback;
				// Later pages in the range can be in TxState::dirty; expedite regardless.
				if(!_managed->_writebackExpedited) {
					_managed->_writebackExpedited = true;
					raiseExpedite = true;
				}
				break;
			}
		}
		if(raiseExpedite)
			_managed->_expediteEvent.raise();

		if(exhausted)
			break;

		if(!monitor)
			continue;
		co_await monitor->event.wait();

		// If the writeback was already in progress, it is not guaranteed that it did write
		// back the latest state before the writebackFence().
		// In this case, we may need to wait for another writeback.
		if(needSecond) {
			monitor = {};
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&_managed->mutex);

				auto pit = _managed->findPage(index);
				if(pit && (pit->transactionState == ManagedSpace::TxState::wantWriteback
						|| pit->transactionState == ManagedSpace::TxState::writeback))
					monitor = pit->requireMonitor(ManagedSpace::MonitorType::writeback);
			}

			if(monitor)
				co_await monitor->event.wait();
		}
	}

	co_return {};
}

coroutine<frg::expected<Error>> BackingMemory::invalidateRange(uintptr_t offset, size_t size,
		DiscardMode mode) {
	assert(currentIpl() == ipl::exceptionalWork);
	if(_managed->isSwapSpace)
		co_return Error::illegalObject;
	if (offset & (kPageSize - 1))
		co_return Error::illegalArgs;
	if (size & (kPageSize - 1))
		co_return Error::illegalArgs;
	if (offset > backingMemoryLength || size > backingMemoryLength - offset)
		co_return Error::bufferTooSmall;

	auto firstPage = offset >> kPageShift;
	auto limitPage = (offset + size) >> kPageShift;

	// Mark the pages as discarded.
	// Do this in chunks so that the spinlock is not held for an unbounded time.
	uint64_t markCursor = firstPage;
	bool raiseDirty = false;
	bool raiseDiscard = false;
	bool raiseExpedite = false;
	while(true) {
		bool exhausted = false;
		ManagedSpace::MonitorPendingList pendingMonitors;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&_managed->mutex);

			auto it = _managed->pages.lower_bound(markCursor);
			for(size_t i = 0; i < discardChunkSize; ++i) {
				if(it == _managed->pages.end() || (*it)->cachePage.identity >= limitPage) {
					exhausted = true;
					break;
				}
				// discardPage() can erase the entry, so advance first.
				auto *page = *it;
				markCursor = page->cachePage.identity + 1;
				++it;
				_managed->discardPage(page, mode, raiseDirty, raiseDiscard, raiseExpedite,
						pendingMonitors);
			}
		}
		ManagedSpace::_raiseMonitors(pendingMonitors);
		if(exhausted)
			break;
	}

	// Wake the invalidation coroutine only after everything is marked as discarded.
	// This helps the invalidation coroutine to coelesce ranges.
	if(raiseDirty)
		_managed->_dirtyEvent.raise();
	if(raiseDiscard)
		_managed->_discardEvent.raise();
	if(raiseExpedite)
		_managed->_expediteEvent.raise();

	// Wait until every previously discarded page is erased.
	uint64_t waitCursor = firstPage;
	while(true) {
		bool done = false;
		frg::intrusive_shared_ptr<ManagedSpace::TransactionMonitor, Allocator> monitor;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&_managed->mutex);

			auto it = _managed->pages.lower_bound(waitCursor);
			while(it != _managed->pages.end() && (*it)->cachePage.identity < limitPage
					&& !(*it)->discarded)
				++it;

			if(it == _managed->pages.end() || (*it)->cachePage.identity >= limitPage) {
				done = true;
			} else {
				waitCursor = (*it)->cachePage.identity;
				monitor = (*it)->requireMonitor(ManagedSpace::MonitorType::discard);
			}
		}
		if(done)
			break;
		co_await monitor->event.wait();
	}

	co_return {};
}

// --------------------------------------------------------
// FrontalMemory
// --------------------------------------------------------

std::expected<smarter::shared_ptr<FrontalMemory>, Error> FrontalMemory::create(
		smarter::shared_ptr<ManagedSpace> managed) {
	auto ptr = allocate_rcu_shared<FrontalMemory>(HierarchyAllocator{managed->hierarchy.lock()},
			CtorToken{}, std::move(managed));
	ptr->selfPtr = ptr;
	return ptr;
}

Error FrontalMemory::lockRange(uintptr_t offset, size_t size) {
	// We only check once against the ManagedSpace size.
	// A concurrent shrink after this check is equivalent to locking before the shrink.
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_managed->mutex);

		auto limit = _managed->numPages << kPageShift;
		if(offset > limit || size > limit - offset)
			return Error::bufferTooSmall;
	}

	return _managed->lockPages(offset, size);
}

void FrontalMemory::unlockRange(uintptr_t offset, size_t size) {
	// Note that unlockPages() tolerates locks beyond the current size:
	// unlocking a locked range must still be possible even after shrinking the file.
	_managed->unlockPages(offset, size);
}

PhysicalRange FrontalMemory::peekRange(uintptr_t offset, FetchFlags) {
	auto index = offset >> kPageShift;
	auto misalign = offset & (kPageSize - 1);

	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_managed->mutex);

	if(index >= _managed->numPages)
		return PhysicalRange{};
	auto pit = _managed->findPage(index);
	if(!pit)
		return PhysicalRange{};

	auto physical = _managed->peekPage(pit);
	if(physical == PhysicalAddr(-1))
		return PhysicalRange{};

	return PhysicalRange{
		.physical = physical + misalign,
		.size = kPageSize - misalign,
		.cachingMode = CachingMode::null,
		.isMutable = true
	};
}

std::expected<size_t, Error> FrontalMemory::accessRange(uintptr_t offset, size_t size,
		FetchFlags flags, PageAccessFn fn) {
	auto index = offset >> kPageShift;
	auto misalign = offset & (kPageSize - 1);

	return _managed->accessPage([&] () -> std::expected<ManagedSpace::ManagedPage *, Error> {
		if(index >= _managed->numPages)
			return std::unexpected{Error::fault};
		return _managed->findPage(index);
	}, misalign, size, flags & fetchRequireMutable, fn);
}

coroutine<frg::expected<Error, size_t>>
FrontalMemory::touchRange(uintptr_t offset, size_t, FetchFlags flags) {
	assert(currentIpl() == ipl::exceptionalWork);

	auto index = offset >> kPageShift;
	auto misalign = offset & (kPageSize - 1);

	// Frames for initializePage(), allocated and zeroed outside of the mutex.
	PhysicalAddr freshPhysical(-1);
	std::array<PhysicalAddr, ManagedSpace::numReadaheadPages> readaheadPhysicals;
	size_t numReadaheadPhysicals = 0;
	frg::scope_exit freeFreshFrame{[&] {
		if(freshPhysical != PhysicalAddr(-1))
			deallocateAndUncharge(_managed->hierarchy.get(), freshPhysical, kPageSize,
					PhysicalKind::evictable);
		for(size_t i = 0; i < numReadaheadPhysicals; ++i) {
			if(readaheadPhysicals[i] != PhysicalAddr(-1))
				deallocateAndUncharge(_managed->hierarchy.get(), readaheadPhysicals[i], kPageSize,
						PhysicalKind::evictable);
		}
	}};

	bool retriedReadahead = false;
	while(true) {
		ManageList pendingManagement;
		frg::intrusive_shared_ptr<ManagedSpace::TransactionMonitor, Allocator> fetchMonitor;
		ManagedSpace::ReadaheadFrames readaheadFrames{
			.physicals{readaheadPhysicals.data(), numReadaheadPhysicals}
		};
		{
			auto irq_lock = frg::guard(&irqMutex());
			auto lock = frg::guard(&_managed->mutex);

			if(index >= _managed->numPages)
				co_return Error::fault;

			// Try the fast-paths first.
			auto pit = _managed->findOrInsertPage(index);
			assert(pit);
			if(_managed->touchPresentPage(pit))
				co_return kPageSize - misalign;

			// We have to take the slow-path, i.e., perform the fetch asynchronously.
			auto monitorOutcome = _managed->initializePage(pit, flags, freshPhysical,
					&readaheadFrames, pendingManagement);
			if(!monitorOutcome)
				co_return monitorOutcome.error();
			fetchMonitor = std::move(*monitorOutcome);
		}

		if(fetchMonitor) {
			ManagedSpace::_raiseManagement(pendingManagement);
			// Readahead that lacked frames is retried once, after the page's own request went out.
			if(!readaheadFrames.missing || retriedReadahead) {
				co_await fetchMonitor->event.wait();
				co_return kPageSize - misalign;
			}
			retriedReadahead = true;
		}else{
			// initializePage() needs a frame, allocate one without the mutex held and retry.
			assert(freshPhysical == PhysicalAddr(-1));
			auto physicalOutcome = co_await chargeAndAllocate(_managed->hierarchy.get(),
					PhysicalKind::evictable, flags);
			if(!physicalOutcome)
				co_return physicalOutcome.error();
			freshPhysical = *physicalOutcome;
			PageAccessor accessor{freshPhysical};
			memset(accessor.get(), 0, kPageSize);
		}

		// Drop the entries that initializePage() attached, then allocate the missing frames.
		// Readahead is best effort, so its frames do not wait for memory.
		size_t numUnattached = 0;
		for(size_t i = 0; i < numReadaheadPhysicals; ++i) {
			if(readaheadPhysicals[i] != PhysicalAddr(-1))
				readaheadPhysicals[numUnattached++] = readaheadPhysicals[i];
		}
		numReadaheadPhysicals = numUnattached;
		auto wantReadaheadPhysicals = frg::min(numReadaheadPhysicals + readaheadFrames.missing,
				readaheadPhysicals.size());
		while(numReadaheadPhysicals < wantReadaheadPhysicals) {
			// Readahead is speculative, so it does not use the floor of a synchronous caller.
			auto readaheadOutcome = tryChargeAndAllocate(_managed->hierarchy.get(), kPageSize,
					PhysicalKind::evictable, 64, flags & ~fetchSynchronous);
			if(!readaheadOutcome)
				break;
			PageAccessor readaheadAccessor{*readaheadOutcome};
			memset(readaheadAccessor.get(), 0, kPageSize);
			readaheadPhysicals[numReadaheadPhysicals++] = *readaheadOutcome;
		}

		// Without new frames, another pass would not request any more readahead.
		if(fetchMonitor && numReadaheadPhysicals == numUnattached) {
			co_await fetchMonitor->event.wait();
			co_return kPageSize - misalign;
		}
	}
}

size_t FrontalMemory::getLength() {
	// Size is constant so we do not need to lock.
	return _managed->numPages << kPageShift;
}

// --------------------------------------------------------
// SwappableMemory
// --------------------------------------------------------

std::expected<smarter::shared_ptr<SwappableMemory>, Error> SwappableMemory::create(
		smarter::shared_ptr<Hierarchy> hierarchy, smarter::shared_ptr<SwapSpace> space,
		size_t length) {
	smarter::borrowed_ptr<Hierarchy> hierarchyPtr{hierarchy};
	auto ptr = allocate_rcu_shared<SwappableMemory>(HierarchyAllocator{std::move(hierarchy)},
			CtorToken{}, hierarchyPtr, std::move(space), length);
	ptr->selfPtr = ptr;
	return ptr;
}

SwappableMemory::SwappableMemory(CtorToken, smarter::borrowed_ptr<Hierarchy> hierarchy,
		smarter::shared_ptr<SwapSpace> space, size_t length)
: _hierarchy{std::move(hierarchy)}, _space{std::move(space)}, _length{length},
		_table{AccountAllocator{_hierarchy->controller()}} {
	assert(!(length & (kPageSize - 1)));
}

SwappableMemory::~SwappableMemory() {
	for(auto it = _table.begin(); it != _table.end(); ++it) {
		_space->discardPageAndRaise(*it, DiscardMode::dropDirty);
		_hierarchy->unchargeSwap(kPageSize);
	}
}

ManagedSpace::ManagedPage *SwappableMemory::_translate(uint64_t index) {
	auto tit = _table.find(index);
	if(tit)
		return *tit;

	auto pit = _space->allocatePage();
	if(!pit)
		return nullptr;
	_table.insert(index, pit);
	_hierarchy->chargeSwap(kPageSize);
	return pit;
}

size_t SwappableMemory::getLength() {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_space->mutex);

	return _length;
}

coroutine<frg::expected<Error>> SwappableMemory::resize(size_t newSize) {
	assert(currentIpl() == ipl::exceptionalWork);
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_space->mutex);

		if(newSize & (kPageSize - 1))
			co_return Error::illegalArgs;
		// TODO: Support shrinking of SwappableMemory.
		if(newSize < _length)
			co_return Error::illegalArgs;
		_length = newSize;
	}
	co_return {};
}

Error SwappableMemory::lockRange(uintptr_t offset, size_t size) {
	bool raiseDiscard = false;
	Error result = Error::success;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_space->mutex);

		if(offset + size > _length)
			return Error::bufferTooSmall;

		for(size_t pg = 0; pg < size; pg += kPageSize) {
			auto index = (offset + pg) >> kPageShift;
			auto page = _translate(index);
			if(!page) {
				// The page cannot be allocated, unwind the locks we already took.
				_unlockPagesLocked(offset, pg, raiseDiscard);
				result = Error::noMemory;
				break;
			}
			_space->lockPage(page);
		}
	}
	if(raiseDiscard)
		_space->_discardEvent.raise();
	return result;
}

void SwappableMemory::_unlockPagesLocked(uintptr_t offset, size_t size, bool &raiseDiscard) {
	for(size_t pg = 0; pg < size; pg += kPageSize) {
		auto index = (offset + pg) >> kPageShift;
		auto tit = _table.find(index);
		assert(tit);
		_space->unlockPage(*tit, raiseDiscard);
	}
}

void SwappableMemory::unlockRange(uintptr_t offset, size_t size) {
	bool raiseDiscard = false;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_space->mutex);

		assert(offset + size <= _length);
		_unlockPagesLocked(offset, size, raiseDiscard);
	}
	if(raiseDiscard)
		_space->_discardEvent.raise();
}

PhysicalRange SwappableMemory::peekRange(uintptr_t offset, FetchFlags) {
	auto index = offset >> kPageShift;
	auto misalign = offset & (kPageSize - 1);

	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_space->mutex);

	if(offset >= _length)
		return PhysicalRange{};

	auto tit = _table.find(index);
	if(!tit)
		return PhysicalRange{};

	auto physical = _space->peekPage(*tit);
	if(physical == PhysicalAddr(-1))
		return PhysicalRange{};

	return PhysicalRange{
		.physical = physical + misalign,
		.size = kPageSize - misalign,
		.cachingMode = CachingMode::null,
		.isMutable = true
	};
}

std::expected<size_t, Error> SwappableMemory::accessRange(uintptr_t offset, size_t size,
		FetchFlags flags, PageAccessFn fn) {
	auto index = offset >> kPageShift;
	auto misalign = offset & (kPageSize - 1);

	return _space->accessPage([&] () -> std::expected<ManagedSpace::ManagedPage *, Error> {
		if(offset >= _length)
			return std::unexpected{Error::fault};
		auto tit = _table.find(index);
		if(!tit)
			return nullptr;
		return *tit;
	}, misalign, size, flags & fetchRequireMutable, fn);
}

coroutine<frg::expected<Error, size_t>>
SwappableMemory::touchRange(uintptr_t offset, size_t, FetchFlags flags) {
	assert(currentIpl() == ipl::exceptionalWork);

	auto index = offset >> kPageShift;
	auto misalign = offset & (kPageSize - 1);

	// Frame for the zero-fill and swap-in paths. Allocated and zeroed outside of the
	// SwapSpace mutex, then installed under the mutex after re-checking that no
	// other thread allocated the frame meanwhile.
	PhysicalAddr freshPhysical(-1);
	frg::scope_exit freeFreshFrame{[&] {
		if(freshPhysical != PhysicalAddr(-1))
			deallocateAndUncharge(_space->hierarchy.get(), freshPhysical, kPageSize,
					PhysicalKind::evictable);
	}};

	while(true) {
		ManageList pendingManagement;
		frg::intrusive_shared_ptr<ManagedSpace::TransactionMonitor, Allocator> fetchMonitor;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&_space->mutex);

			if(index >= (_length >> kPageShift))
				co_return Error::fault;

			auto pit = _translate(index);
			if(!pit)
				co_return Error::noMemory;

			if(_space->touchPresentPage(pit))
				co_return kPageSize - misalign;

			if(!pit->swapCopyValid) {
				// The page is logically all-zero - zero-fill it synchronously.
				assert(pit->transactionState == ManagedSpace::TxState::none);
				if(freshPhysical != PhysicalAddr(-1)) {
					// A clean install neither wakes the drain nor expedites writeback.
					bool raiseDirty = false;
					bool raiseExpedite = false;
					_space->installPage(pit, freshPhysical, false, 0, raiseDirty, raiseExpedite);
					assert(!raiseDirty && !raiseExpedite);
					freshPhysical = PhysicalAddr(-1);
					co_return kPageSize - misalign;
				}
				// ... no frame at hand, allocate one below without the mutex held and retry.
			}else{
				// The page is swapped out, read it back via the manage protocol.
				// Without a fetch monitor, initializePage() needs a frame; allocate one below.
				auto monitorOutcome = _space->initializePage(pit, flags, freshPhysical, nullptr,
						pendingManagement);
				if(!monitorOutcome)
					co_return monitorOutcome.error();
				fetchMonitor = std::move(*monitorOutcome);
			}
		}

		if(fetchMonitor) {
			ManagedSpace::_raiseManagement(pendingManagement);
			co_await fetchMonitor->event.wait();
			co_return kPageSize - misalign;
		}

		assert(freshPhysical == PhysicalAddr(-1));
		auto physicalOutcome = co_await chargeAndAllocate(_space->hierarchy.get(),
				PhysicalKind::evictable, flags);
		if(!physicalOutcome)
			co_return physicalOutcome.error();
		freshPhysical = *physicalOutcome;
		PageAccessor accessor{freshPhysical};
		memset(accessor.get(), 0, kPageSize);
	}
}

// --------------------------------------------------------
// IndirectMemory
// --------------------------------------------------------

std::expected<smarter::shared_ptr<IndirectMemory>, Error> IndirectMemory::create(size_t numSlots) {
	auto ptr = allocate_rcu_shared<IndirectMemory>(*kernelAlloc, CtorToken{}, numSlots);
	return ptr;
}

IndirectMemory::IndirectMemory(CtorToken, size_t numSlots)
: indirections_{*kernelAlloc} {
	indirections_.resize(numSlots);
}

IndirectMemory::~IndirectMemory() {
	// For now we do nothing when deallocating hardware memory.
}

Error IndirectMemory::lockRange(uintptr_t offset, size_t size) {
	auto slot = offset >> 32;
	auto inSlotOffset = offset & ((uintptr_t(1) << 32) - 1);

	smarter::shared_ptr<IndirectionSlot> indirection;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		if(slot >= indirections_.size())
			return Error::fault;
		if(!indirections_[slot])
			return Error::fault;
		if(inSlotOffset + size > indirections_[slot]->size)
			return Error::fault;
		indirection = indirections_[slot];
	}

	return indirection->memory->lockRange(indirection->offset + inSlotOffset, size);
}

void IndirectMemory::unlockRange(uintptr_t offset, size_t size) {
	auto slot = offset >> 32;
	auto inSlotOffset = offset & ((uintptr_t(1) << 32) - 1);

	smarter::shared_ptr<IndirectionSlot> indirection;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		// Otherwise, lockRange() would have faulted.
		assert(slot < indirections_.size());
		assert(indirections_[slot]);
		assert(inSlotOffset + size <= indirections_[slot]->size);
		indirection = indirections_[slot];
	}

	indirection->memory->unlockRange(indirection->offset + inSlotOffset, size);
}

PhysicalRange IndirectMemory::peekRange(uintptr_t offset, FetchFlags flags) {
	auto slot = offset >> 32;
	auto inSlotOffset = offset & ((uintptr_t(1) << 32) - 1);

	smarter::shared_ptr<IndirectionSlot> indirection;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		if (slot >= indirections_.size())
			return PhysicalRange{};
		if (!indirections_[slot])
			return PhysicalRange{};
		indirection = indirections_[slot];
	}

	if (inSlotOffset >= indirection->size)
		return PhysicalRange{};

	auto physicalRange = indirection->memory->peekRange(indirection->offset + inSlotOffset, flags);
	if (physicalRange.physical == ~PhysicalAddr{0})
		return PhysicalRange{};

	CachingMode cachingOverride = CachingMode::null;
	if(indirection->flags & cacheWriteCombine)
		cachingOverride = CachingMode::writeCombine;

	return PhysicalRange{
		.physical = physicalRange.physical,
		.size = frg::min(physicalRange.size, indirection->size - inSlotOffset),
		.cachingMode = determineCachingMode(physicalRange.cachingMode, cachingOverride),
		.isMutable = physicalRange.isMutable
	};
}

std::expected<size_t, Error> IndirectMemory::accessRange(uintptr_t offset, size_t size,
		FetchFlags flags, PageAccessFn fn) {
	auto slot = offset >> 32;
	auto inSlotOffset = offset & ((uintptr_t(1) << 32) - 1);

	smarter::shared_ptr<IndirectionSlot> indirection;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		if(slot >= indirections_.size())
			return std::unexpected{Error::fault};
		if(!indirections_[slot])
			return std::unexpected{Error::fault};
		indirection = indirections_[slot];
	}

	if(inSlotOffset >= indirection->size)
		return std::unexpected{Error::fault};

	auto chunk = frg::min(size, indirection->size - inSlotOffset);
	return indirection->memory->accessRange(indirection->offset + inSlotOffset, chunk, flags, fn);
}

coroutine<frg::expected<Error, size_t>>
IndirectMemory::touchRange(uintptr_t offset, size_t sizeHint, FetchFlags flags) {
	assert(currentIpl() == ipl::exceptionalWork);

	auto slot = offset >> 32;
	auto inSlotOffset = offset & ((uintptr_t(1) << 32) - 1);

	smarter::shared_ptr<IndirectionSlot> indirection;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		if (slot >= indirections_.size())
			co_return Error::fault;
		if (!indirections_[slot])
			co_return Error::fault;
		indirection = indirections_[slot];
	}

	if (inSlotOffset >= indirection->size)
		co_return Error::fault;

	auto affectedSize = FRG_CO_TRY(
		co_await indirection->memory->touchRange(indirection->offset + inSlotOffset, sizeHint, flags)
	);
	co_return frg::min(affectedSize, indirection->size - inSlotOffset);
}

size_t IndirectMemory::getLength() {
	return indirections_.size() << 32;
}

Error IndirectMemory::setIndirection(size_t slot, smarter::shared_ptr<MemoryView> memory,
		uintptr_t offset, size_t size, CachingFlags flags) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&mutex_);

	if(slot >= indirections_.size())
		return Error::outOfBounds;
	auto indirection = smarter::allocate_shared<IndirectionSlot>(*kernelAlloc,
			this, slot, memory, offset, size, flags);
	// TODO: Forward evictions of the memory to mappings of this view.
	//       Until then, we must not observe it: an unserviced observer stalls all of its evictions.
	indirections_[slot] = std::move(indirection);
	return Error::success;
}

// --------------------------------------------------------
// CopyOnWriteMemory
// --------------------------------------------------------

// Must run outside any locks since discardPageAndRaise take ManagedSpace mutex.
CowPage::~CowPage() {
	if(state == CowState::null)
		return;
	assert(state == CowState::hasCopy);
	if(swapPage) {
		assert(currentIpl() < ipl::interrupt);
		auto space = static_cast<ManagedSpace *>(swapPage->cachePage.bundle);
		space->discardPageAndRaise(swapPage, DiscardMode::dropDirty);
	}else{
		assert(physical != PhysicalAddr(-1));
		globalPfnDb().erase(physical);
		physicalAllocator->free(physical, kPageSize);
	}
}

std::expected<smarter::shared_ptr<CopyOnWriteMemory>, Error> CopyOnWriteMemory::create(
		smarter::shared_ptr<Hierarchy> hierarchy, smarter::shared_ptr<SwapSpace> space,
		smarter::shared_ptr<MemoryView> view, uintptr_t offset, size_t length) {
	auto ptr = allocate_rcu_shared<CopyOnWriteMemory>(*kernelAlloc, CtorToken{},
			std::move(hierarchy), std::move(space), std::move(view), offset, length);
	ptr->selfPtr = ptr;
	return ptr;
}

CopyOnWriteMemory::CopyOnWriteMemory(CtorToken, smarter::shared_ptr<Hierarchy> hierarchy,
		smarter::shared_ptr<SwapSpace> space, smarter::shared_ptr<MemoryView> view,
		uintptr_t offset, size_t length)
: MemoryView{frg::allocate_intrusive_shared<EvictionQueue>(Allocator{})},
		_hierarchy{std::move(hierarchy)}, _view{std::move(view)},
		_viewOffset{offset}, _length{length}, _space{std::move(space)},
		_ownedPages{*kernelAlloc}, _sharedPages{*kernelAlloc} {
	assert(length);
	assert(!(offset & (kPageSize - 1)));
	assert(!(length & (kPageSize - 1)));
}

CopyOnWriteMemory::~CopyOnWriteMemory() {
	unchargePages_(_chargedPages);
}

size_t CopyOnWriteMemory::getLength() {
	return _length;
}

PhysicalAddr CopyOnWriteMemory::_getResident(CowPage *page) {
	assert(page->state == CowState::hasCopy);
	if(!_space) {
		assert(page->physical != PhysicalAddr(-1));
		return page->physical;
	}

	auto spaceLock = frg::guard(&_space->mutex);
	return _space->peekPage(page->swapPage);
}

coroutine<frg::expected<Error>>
CopyOnWriteMemory::_ensureResident(CowPage *page, FetchFlags flags) {
	assert(page->state == CowState::hasCopy);
	if(!_space)
		co_return {};

	// Frame for initializePage(), allocated and zeroed outside of the mutex.
	PhysicalAddr freshPhysical(-1);
	frg::scope_exit freeFreshFrame{[&] {
		if(freshPhysical != PhysicalAddr(-1))
			deallocateAndUncharge(_space->hierarchy.get(), freshPhysical, kPageSize,
					PhysicalKind::evictable);
	}};

	while(true) {
		ManageList pendingManagement;
		frg::intrusive_shared_ptr<ManagedSpace::TransactionMonitor, Allocator> fetchMonitor;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto spaceLock = frg::guard(&_space->mutex);

			if(_space->touchPresentPage(page->swapPage))
				co_return {};
			// Copies are installed dirty, hence swapped-out copies always have a valid disk copy.
			assert(page->swapPage->swapCopyValid);
			auto monitorOutcome = _space->initializePage(page->swapPage, flags, freshPhysical,
					nullptr, pendingManagement);
			if(!monitorOutcome)
				co_return monitorOutcome.error();
			fetchMonitor = std::move(*monitorOutcome);
		}

		if(fetchMonitor) {
			ManagedSpace::_raiseManagement(pendingManagement);
			co_await fetchMonitor->event.wait();
			co_return {};
		}

		// initializePage() needs a frame, allocate one without the mutex held and retry.
		assert(freshPhysical == PhysicalAddr(-1));
		auto physicalOutcome = co_await chargeAndAllocate(_space->hierarchy.get(),
				PhysicalKind::evictable, flags);
		if(!physicalOutcome)
			co_return physicalOutcome.error();
		freshPhysical = *physicalOutcome;
		PageAccessor accessor{freshPhysical};
		memset(accessor.get(), 0, kPageSize);
	}
}

void CopyOnWriteMemory::_lockPage(CowPage *page) {
	page->lockCount++;

	// Locks taken before materialization are transferred by _publishCopy().
	if(_space && page->state == CowState::hasCopy) {
		auto spaceLock = frg::guard(&_space->mutex);
		_space->lockPage(page->swapPage);
	}
}

void CopyOnWriteMemory::_unlockPage(CowPage *page, bool &raiseDiscard) {
	assert(page->lockCount > 0);
	page->lockCount--;

	if(_space && page->state == CowState::hasCopy) {
		auto spaceLock = frg::guard(&_space->mutex);
		_space->unlockPage(page->swapPage, raiseDiscard);
	}
}

coroutine<frg::expected<Error>>
CopyOnWriteMemory::_copyFromCowPage(CowPage *src, PhysicalAddr dst, FetchFlags flags) {
	assert(src->state == CowState::hasCopy);
	PageAccessor dstAccessor{dst};

	if(!_space) {
		assert(src->physical != PhysicalAddr(-1));
		PageAccessor srcAccessor{src->physical};
		memcpy(dstAccessor.get(), srcAccessor.get(), kPageSize);
		co_return {};
	}

	// Pin the source across the copy and page it in.
	{
		auto irqLock = frg::guard(&irqMutex());
		auto spaceLock = frg::guard(&_space->mutex);

		_space->lockPage(src->swapPage);
	}
	auto outcome = co_await _ensureResident(src, flags);
	if(outcome) {
		PhysicalAddr srcPhysical;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto spaceLock = frg::guard(&_space->mutex);

			srcPhysical = _space->peekPage(src->swapPage);
			assert(srcPhysical != PhysicalAddr(-1));
		}
		PageAccessor srcAccessor{srcPhysical};
		memcpy(dstAccessor.get(), srcAccessor.get(), kPageSize);
	}

	bool raiseDiscard = false;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto spaceLock = frg::guard(&_space->mutex);

		_space->unlockPage(src->swapPage, raiseDiscard);
	}
	if(raiseDiscard)
		_space->_discardEvent.raise();
	co_return outcome;
}

frg::expected<Error> CopyOnWriteMemory::_publishCopy(CowPage *page, PhysicalAddr physical,
		bool &raiseDirty, bool &raiseExpedite) {
	assert(page->state != CowState::hasCopy);
	if(!_space) {
		page->physical = physical;
		globalPfnDb().insert(physical, PfnDescriptor::otherPage());
	}else{
		auto irqLock = frg::guard(&irqMutex());
		auto spaceLock = frg::guard(&_space->mutex);

		auto swapPage = _space->allocatePage();
		if(!swapPage)
			return Error::noMemory;
		// Copies are installed dirty - their content doesn't exist anywhere
		// elsewhere, so the frame can only be dropped after a writeback.
		_space->installPage(swapPage, physical, true, page->lockCount, raiseDirty, raiseExpedite);
		page->swapPage = swapPage;
	}
	page->state = CowState::hasCopy;
	return {};
}

coroutine<frg::expected<Error, smarter::shared_ptr<MemoryView>>> CopyOnWriteMemory::fork(
	smarter::shared_ptr<Hierarchy> hierarchy
) {
	assert(currentIpl() == ipl::exceptionalWork);

	// Note that locked pages require special attention during CoW: as we cannot
	// replace them by copies, we have to copy them eagerly.
	// Therefore, they are special-cased below.
	smarter::shared_ptr<CopyOnWriteMemory> forked;
	frg::vector<frg::tuple<size_t, smarter::shared_ptr<CowPage>>, KernelAlloc> lockedCopies{*kernelAlloc};
	size_t numSharedPages{0};

	// Note: We turn owned pages into shared pages while holding the locks below.
	//       Since this happens while the lock is held, peekRange() and touchRange() can never
	//       see intermediate states (e.g., pages that are already removed from _ownedPages
	//       but that are not available in _sharedPages yet).

	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		// Create a new mapping in the forked space.
		forked = allocate_rcu_shared<CopyOnWriteMemory>(*kernelAlloc, CtorToken{},
				std::move(hierarchy), _space, _view, _viewOffset, _length);
		forked->selfPtr = forked;

		// Inspect all copied pages owned by the original mapping.
		// To correctly handle locked pages, we share only non-locked pages
		// between the original and the forked mapping.
		for(size_t pg = 0; pg < _length; pg += kPageSize) {
			auto it = _ownedPages.find(pg >> kPageShift);
			smarter::shared_ptr<CowPage> page;
			if(it)
				page = *it;

			// Pages without a published copy (null or inProgress) still have the content
			// of their source, i.e., the shared page (if any) or the root view.
			if(!page || page->state != CowState::hasCopy) {
				if(auto sharedIt = _sharedPages.find(pg >> kPageShift); sharedIt) {
					auto sharedPage = *sharedIt;
					assert(sharedPage->state == CowState::hasCopy);
					auto newIt = forked->_sharedPages.insert(pg >> kPageShift);
					*newIt = sharedPage;
					++numSharedPages;
				}
				continue;
			}

			if(page->lockCount /*|| disableCow */) {
				// The page is locked. We *need* to keep it in the old address space.
				lockedCopies.push(frg::make_tuple(pg, page));
			}else{
				// Sharing the page shares its content (frame or swap page) with the child.
				_ownedPages.erase(pg >> kPageShift);
				auto sharedIt = _sharedPages.insert(pg >> kPageShift);
				*sharedIt = page;
				auto newIt = forked->_sharedPages.insert(pg >> kPageShift);
				*newIt = page;
				++numSharedPages;
			}
		}
	}

	// Charge the memory to the forked memory view.
	// Shared pages are counted twice: once in the original and once in the forked memory view.
	// This ensures that uncharging behaves correctly.
	{
		auto numPages = numSharedPages + lockedCopies.size();
		if(auto charge = forked->tryChargePages_(numPages); !charge)
			co_return charge.error();

		auto irqLock = frg::guard(&irqMutex());
		auto forkedLock = frg::guard(&forked->_mutex);
		forked->_chargedPages += numPages;
	}

	// Copy all the pages that were locked.
	for(auto [pg, src] : lockedCopies) {
		auto physicalOutcome = co_await _allocateCopy(fetchNone);
		if(!physicalOutcome)
			co_return physicalOutcome.error();
		auto copyPhysical = *physicalOutcome;
		frg::scope_exit freeCopy{[&] {
			_freeCopy(copyPhysical);
		}};

		// The lock observed above may have been dropped concurrently, so the source
		// is pinned again by the copy itself.
		if(auto outcome = co_await _copyFromCowPage(src.get(), copyPhysical, 0); !outcome)
			co_return outcome.error();

		auto copyPage = smarter::allocate_shared<CowPage>(*kernelAlloc);
		bool raiseDirty = false;
		bool raiseExpedite = false;
		if(auto outcome = _publishCopy(copyPage.get(), copyPhysical, raiseDirty, raiseExpedite); !outcome)
			co_return outcome.error();
		freeCopy.release();
		auto copyIt = forked->_ownedPages.insert(pg >> kPageShift);
		*copyIt = copyPage;
		if(raiseDirty)
			_space->_dirtyEvent.raise();
		if(raiseExpedite)
			_space->_expediteEvent.raise();
	}

	co_await evictionQueue()->breakRange(0, _length);
	co_return smarter::shared_ptr<MemoryView>{std::move(forked)};
}

Error CopyOnWriteMemory::lockRange(uintptr_t offset, size_t size) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	for(size_t pg = 0; pg < size; pg += kPageSize) {
		auto it = _ownedPages.find((offset + pg) >> kPageShift);
		if(it) {
			auto page = *it;
			_lockPage(page.get());
		}else{
			auto cowPage = smarter::allocate_shared<CowPage>(*kernelAlloc);
			cowPage->lockCount = 1;
			it = _ownedPages.insert((offset + pg) >> kPageShift);
			*it = cowPage;
		}
	}

	return Error::success;
}

void CopyOnWriteMemory::unlockRange(uintptr_t offset, size_t size) {
	bool raiseDiscard = false;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		for(size_t pg = 0; pg < size; pg += kPageSize) {
			auto it = _ownedPages.find((offset + pg) >> kPageShift);
			assert(it);
			auto page = *it;
			_unlockPage(page.get(), raiseDiscard);
		}
	}
	if(raiseDiscard)
		_space->_discardEvent.raise();
}

PhysicalRange CopyOnWriteMemory::peekRange(uintptr_t offset, FetchFlags flags) {
	auto misalign = offset & (kPageSize - 1);

	// Note: the passthrough cases here have to match touchRange() since
	//       callers expect touchRange() to make the page available to peekRange().
	bool passthrough = false;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		if(offset >= _length)
			return PhysicalRange{};

		if(auto it = _ownedPages.find(offset >> kPageShift); it) {
			auto page = *it;
			if(page->state == CowState::hasCopy) {
				auto physical = _getResident(page.get());
				if(physical == PhysicalAddr(-1))
					return PhysicalRange{};
				return PhysicalRange{
					.physical = physical + misalign,
					.size = kPageSize - misalign,
					.cachingMode = CachingMode::null,
					.isMutable = true
				};
			}
		} else {
			if (!(flags & fetchRequireMutable)) {
				passthrough = true;
			}
		}

		if (passthrough) {
			if(auto it = _sharedPages.find(offset >> kPageShift); it) {
				auto page = *it;
				auto physical = _getResident(page.get());
				if(physical == PhysicalAddr(-1))
					return PhysicalRange{};
				return PhysicalRange{
					.physical = physical + misalign,
					.size = kPageSize - misalign,
					.cachingMode = CachingMode::null,
					.isMutable = false
				};
			}
		}
	}
	// Note: totalOffset is not necessarily page aligned.
	auto totalOffset = _viewOffset + offset;

	if (passthrough) {
		auto range = _view->peekRange(totalOffset, flags);
		// Note: passthrough caching mode etc. but clamp the size to kPageSize.
		if(range.physical != PhysicalAddr(-1)) {
			return PhysicalRange{
				.physical = range.physical,
				.size = frg::min(range.size, kPageSize - misalign),
				.cachingMode = range.cachingMode,
				.isMutable = false
			};
		}
	}

	return PhysicalRange{};
}

std::expected<size_t, Error> CopyOnWriteMemory::accessRange(uintptr_t offset, size_t size,
		FetchFlags flags, PageAccessFn fn) {
	auto misalign = offset & (kPageSize - 1);
	auto chunk = frg::min(size, kPageSize - misalign);

	smarter::shared_ptr<CowPage> ownedPage;
	smarter::shared_ptr<CowPage> sharedPage;
	// Note: the passthrough cases here have to match touchRange() since
	//       callers expect touchRange() to make the page available to accessRange().
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		if(offset >= _length)
			return std::unexpected{Error::fault};

		if(auto it = _ownedPages.find(offset >> kPageShift); it) {
			auto page = *it;
			if(page->state != CowState::hasCopy)
				return 0;
			// The lock keeps fork() from sharing the page during the access.
			// It is not mirrored into the swap page since ManagedSpace::accessRange() locks that.
			page->lockCount++;
			ownedPage = std::move(page);
		} else {
			if(flags & fetchRequireMutable)
				return 0;
			if(auto it = _sharedPages.find(offset >> kPageShift); it) {
				assert((*it)->state == CowState::hasCopy);
				sharedPage = *it;
			}
		}
	}

	if(!ownedPage && !sharedPage) {
		// Note: totalOffset is not necessarily page aligned.
		auto totalOffset = _viewOffset + offset;
		return _view->accessRange(totalOffset, chunk, flags, fn);
	}

	auto page = ownedPage ? ownedPage.get() : sharedPage.get();
	std::expected<size_t, Error> outcome;
	if(_space) {
		outcome = _space->accessPage(
				[&] () -> std::expected<ManagedSpace::ManagedPage *, Error> { return page->swapPage; },
				misalign, chunk, flags & fetchRequireMutable, fn);
	} else {
		assert(page->physical != PhysicalAddr(-1));
		PageAccessor accessor{page->physical};
		fn(reinterpret_cast<uint8_t *>(accessor.get()) + misalign, chunk);
		outcome = chunk;
	}

	if(ownedPage) {
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		assert(ownedPage->lockCount > 0);
		ownedPage->lockCount--;
	}
	return outcome;
}

coroutine<frg::expected<Error, size_t>>
CopyOnWriteMemory::touchRange(uintptr_t offset, size_t sizeHint, FetchFlags flags) {
	assert(currentIpl() == ipl::exceptionalWork);

	auto misalign = offset & (kPageSize - 1);

	while(true) {
		smarter::shared_ptr<CowPage> cowPage;
		smarter::shared_ptr<CowPage> sharedPage;
		// Note: the passthrough cases here have to match peekRange() since
		//       callers expect touchRange() to make the page available to peekRange().
		bool passthrough = false;
		bool waitForCopy = false;
		bool touchOwned = false;
		{
			// If the page is owned by this memory object, we just return it.
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&_mutex);

			if(offset >= _length)
				co_return Error::fault;

			auto cowIt = _ownedPages.find(offset >> kPageShift);
			if(cowIt) {
				cowPage = *cowIt;
				if(cowPage->state == CowState::hasCopy) {
					touchOwned = true;
				}else if(cowPage->state == CowState::inProgress) {
					waitForCopy = true;
				}else{
					assert(cowPage->state == CowState::null);
					cowPage->state = CowState::inProgress;
				}
			}else{
				if (!(flags & fetchRequireMutable)) {
					passthrough = true;
				} else {
					// Otherwise we need to copy from a shared page or from the root view.
					cowPage = smarter::allocate_shared<CowPage>(*kernelAlloc);
					cowPage->state = CowState::inProgress;
					cowIt = _ownedPages.insert(offset >> kPageShift);
					*cowIt = cowPage;
				}
			}

			if(!waitForCopy && !touchOwned) {
				if(auto sharedIt = _sharedPages.find(offset >> kPageShift); sharedIt) {
					sharedPage = *sharedIt;
					assert(sharedPage->state == CowState::hasCopy);
				}
			}
		}

		if(touchOwned) {
			// The copy may be swapped out.
			FRG_CO_TRY(co_await _ensureResident(cowPage.get(), flags));
			co_return kPageSize - misalign;
		}

		// Passthrough and waitForCopy are mutually exclusive:
		// if waitForCopy is set, we may need to wait for eviction to finish
		// and we must not return passed through pages after eviction started.
		assert(!(passthrough && waitForCopy));

		if(passthrough) {
			if(sharedPage) {
				// The shared page may be swapped out.
				FRG_CO_TRY(co_await _ensureResident(sharedPage.get(), flags));
				co_return kPageSize - misalign;
			}

			// Note: totalOffset is not necessarily page aligned.
			auto totalOffset = _viewOffset + offset;
			auto affectedSize = FRG_CO_TRY(co_await _view->touchRange(totalOffset, sizeHint, flags));
			co_return frg::min(affectedSize, kPageSize - misalign);
		}

		if(waitForCopy) {
			bool stillWaiting;
			do {
				stillWaiting = co_await _copyEvent.async_wait_if([&] () -> bool {
					// TODO: this could be faster if cowIt->state was atomic.
					auto irqLock = frg::guard(&irqMutex());
					auto lock = frg::guard(&_mutex);

					return cowPage->state == CowState::inProgress;
				});
			} while(stillWaiting);

			// The copy may have failed (rolling the page back) or may already
			// have been swapped out again; re-inspect the page.
			continue;
		}

		FRG_CO_TRY(co_await _materializePage(offset, cowPage, sharedPage, flags));
		co_return kPageSize - misalign;
	}
}

coroutine<frg::expected<Error>>
CopyOnWriteMemory::_materializePage(uintptr_t offset,
		smarter::shared_ptr<CowPage> cowPage, smarter::shared_ptr<CowPage> sharedPage,
		FetchFlags flags) {
	auto alignedOffset = offset & ~(kPageSize - 1);
	// Note: offset is not necessarily page aligned.
	auto pageOffset = (_viewOffset + offset) & ~(kPageSize - 1);

	// If there is no copy, roll the page back so that waiters do not wait forever.
	frg::scope_exit rollback{[&] {
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&_mutex);

			assert(cowPage->state == CowState::inProgress);
			cowPage->state = CowState::null;
			// Locked pages stay as placeholders, as created by lockRange().
			if(!cowPage->lockCount)
				_ownedPages.erase(offset >> kPageShift);
		}
		_copyEvent.raise();
	}};

	// Replacing a shared page by an owned one does not charge here
	// since we already charged for the shared page at fork() time.
	if(!sharedPage) {
		if(auto charge = tryChargePages_(1); !charge)
			co_return charge.error();
	}
	frg::scope_exit uncharge{[&] {
		if(!sharedPage)
			unchargePages_(1);
	}};

	auto physicalOutcome = co_await _allocateCopy(flags);
	if(!physicalOutcome)
		co_return physicalOutcome.error();
	auto physical = *physicalOutcome;
	frg::scope_exit freeCopy{[&] {
		_freeCopy(physical);
	}};

	// Copy from the shared page (outside of the locks; it remains in hasCopy state)
	// or from the root view.
	if(sharedPage) {
		FRG_CO_TRY(co_await _copyFromCowPage(sharedPage.get(), physical, flags));
	}else{
		PageAccessor accessor{physical};
		FRG_CO_TRY(co_await _view->copyFrom(pageOffset, accessor.get(), kPageSize));
	}

	// To make CoW unobservable, we first need to evict the page here.
	co_await evictionQueue()->breakRange(alignedOffset, kPageSize);

	bool raiseDirty = false;
	bool raiseExpedite = false;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		assert(cowPage->state == CowState::inProgress);
		if(auto outcome = _publishCopy(cowPage.get(), physical, raiseDirty, raiseExpedite); !outcome)
			co_return outcome.error();
		freeCopy.release();
		uncharge.release();
		rollback.release();
		if(!sharedPage)
			_chargedPages++;
		// The owned copy supersedes the shared page, so drop our reference to it.
		if(sharedPage)
			_sharedPages.erase(offset >> kPageShift);
	}
	_copyEvent.raise();
	if(raiseDirty)
		_space->_dirtyEvent.raise();
	if(raiseExpedite)
		_space->_expediteEvent.raise();
	co_return {};
}

std::expected<void, Error> CopyOnWriteMemory::tryChargePages_(size_t n) {
	// Swappable copies hold swap slots; their frames are charged by the swap space.
	if(_space) {
		_hierarchy->chargeSwap(n << kPageShift);
		return {};
	}
	return _hierarchy->tryChargeMemory(n << kPageShift);
}

void CopyOnWriteMemory::unchargePages_(size_t n) {
	if(_space) {
		_hierarchy->unchargeSwap(n << kPageShift);
		return;
	}
	_hierarchy->unchargeMemory(n << kPageShift);
}

coroutine<std::expected<PhysicalAddr, Error>>
CopyOnWriteMemory::_allocateCopy(FetchFlags flags) {
	if(_space)
		co_return co_await chargeAndAllocate(_space->hierarchy.get(), PhysicalKind::evictable,
				flags);
	// Non-swappable CopyOnWriteMemory only needs the charge in tryChargePages_().
	co_return co_await physicalAllocator->allocateAwaitable(
			userAllocationPolicy(_hierarchy.get(), flags), PhysicalKind::pinned,
			async::cancellation_token{});
}

void CopyOnWriteMemory::_freeCopy(PhysicalAddr physical) {
	if(_space) {
		deallocateAndUncharge(_space->hierarchy.get(), physical, kPageSize,
				PhysicalKind::evictable);
		return;
	}
	// Non-swappable CopyOnWriteMemory only needs the uncharge in unchargePages_().
	physicalAllocator->free(physical, kPageSize);
}

// --------------------------------------------------------------------------------------

namespace {
	frg::eternal<FutexRealm> globalFutexRealm;
}

FutexRealm *getGlobalFutexRealm() {
	return &globalFutexRealm.get();
}

} // namespace thor
