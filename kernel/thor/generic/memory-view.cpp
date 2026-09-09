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
}

// --------------------------------------------------------
// Reclaim implementation.
// --------------------------------------------------------

struct MemoryReclaimer {
	void registerBundle(CacheBundle *bundle) {
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		bundleList_.push_back(bundle);
	}

	void addPage(CachePage *page) {
		auto *bundle = page->bundle;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&bundle->reclaimMutex_);

			assert(!(page->flags & CachePage::reclaimRegistered));

			page->generation = bundle->newestGen_;
			bundle->genLists_[bundle->newestGen_].push_back(page);
			page->flags |= CachePage::reclaimRegistered;
		}

		rotationTurnaround_.fetch_add(1, std::memory_order_relaxed);
		if (shouldRotate_())
			rotationEvent_.raise();
	}

	void removePage(CachePage *page) {
		auto *bundle = page->bundle;
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&bundle->reclaimMutex_);

		assert(page->flags & CachePage::reclaimRegistered);

		if(page->flags & CachePage::reclaimPosted) {
			if(!(page->flags & CachePage::reclaimInflight)) {
				auto it = bundle->_reclaimList.iterator_to(page);
				bundle->_reclaimList.erase(it);
			}

			page->flags &= ~(CachePage::reclaimPosted | CachePage::reclaimInflight);
		}else{
			auto it = bundle->genLists_[page->generation].iterator_to(page);
			bundle->genLists_[page->generation].erase(it);
		}
		page->flags &= ~CachePage::reclaimRegistered;
	}

	void bumpPage(CachePage *page) {
		auto *bundle = page->bundle;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&bundle->reclaimMutex_);

			assert(page->flags & CachePage::reclaimRegistered);

			if(page->flags & CachePage::reclaimPosted) {
				if(!(page->flags & CachePage::reclaimInflight)) {
					auto it = bundle->_reclaimList.iterator_to(page);
					bundle->_reclaimList.erase(it);
				}

				page->flags &= ~(CachePage::reclaimPosted | CachePage::reclaimInflight);
				page->generation = bundle->newestGen_;
				bundle->genLists_[bundle->newestGen_].push_back(page);
			}else if(page->generation != bundle->newestGen_) {
				auto it = bundle->genLists_[page->generation].iterator_to(page);
				bundle->genLists_[page->generation].erase(it);
				page->generation = bundle->newestGen_;
				bundle->genLists_[bundle->newestGen_].push_back(page);
			}
		}

		rotationTurnaround_.fetch_add(1, std::memory_order_relaxed);
		if (shouldRotate_())
			rotationEvent_.raise();
	}

	bool checkPressure() {
		auto watermark = physicalAllocator->numTotalPages() * 3 / 4;
		return tortureUncaching || physicalAllocator->numUsedPages() >= watermark;
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

	void reclaimPages(CacheBundle *bundle, CachePagesList &out) {
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&bundle->reclaimMutex_);

		out.splice(out.end(), bundle->_reclaimList);

		for(auto page : out) {
			assert(page->flags & CachePage::reclaimRegistered);
			assert(page->flags & CachePage::reclaimPosted);
			assert(!(page->flags & CachePage::reclaimInflight));

			page->flags |= CachePage::reclaimInflight;
		}
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

				// On memory pressure: rotate generations until pressure drops.
				if (checkPressure()) {
					for(unsigned int i = 1; i <= CacheBundle::numGenerations; i++) {
						if(!checkPressure())
							break;

						auto result = rotateGenerations_();
						if(logReclaim) {
							infoLogger() << frg::fmt(
								"thor: Reclamation under pressure (iteration {}) reclaims 0x{:x} bytes",
								i,
								result.sizeReclaimed
							) << frg::endlog;
						}
					}
				}

				// Otherwise: rotate generations when number of page bumps crosses threshold.
				if(shouldRotate_()) {
					auto result = rotateGenerations_();
					if(logReclaim) {
						infoLogger() << frg::fmt(
							"thor: Generation rotation reclaims 0x{:x} bytes",
							result.sizeReclaimed
						) << frg::endlog;
					}
				}

				auto sleepNs = tortureUncaching ? 10'000'000 : 1'000'000'000;
				KernelFiber::asyncBlockCurrent(
					async::race_and_cancel(
						[&] (async::cancellation_token ct) {
							return async::transform(
								rotationEvent_.async_wait_if([&] -> bool {
									return !shouldRotate_();
								}, ct),
								[] (auto) {}
							);
						},
						[&] (async::cancellation_token ct) {
							// TODO: It would be nicer to also handle the pressure case by an event
							//       but that requires integration with the physical allocator.
							return async::transform(
								generalTimerEngine()->sleepFor(sleepNs, ct),
								[] (auto) {}
							);
						}
					)
				);
			}
		});
	}

private:
	struct RotateResult {
		size_t sizeReclaimed{0};
	};

	RotateResult rotateGenerations_() {
		rotationTurnaround_.store(0, std::memory_order_relaxed);

		size_t sizeReclaimed = 0;
		for(auto it = bundleList_.begin(); it != bundleList_.end(); ++it) {
			auto *bundle = *it;

			bool anyReclaimed = false;
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&bundle->reclaimMutex_);

				// The slot after newest is the oldest.
				// This becomes the newest generation after rotation.
				auto g = (bundle->newestGen_ + 1) % CacheBundle::numGenerations;

				// Drain the oldest generation into _reclaimList.
				auto &genList = bundle->genLists_[g];
				while(!genList.empty()) {
					auto page = genList.pop_front();
					assert(page->flags & CachePage::reclaimRegistered);
					assert(!(page->flags & CachePage::reclaimPosted));
					page->flags |= CachePage::reclaimPosted;
					bundle->_reclaimList.push_back(page);
					anyReclaimed = true;
					sizeReclaimed += kPageSize;
				}
				bundle->newestGen_ = g;
			}

			if(anyReclaimed)
				bundle->_reclaimEvent.raise();
		}

		return {
			.sizeReclaimed = sizeReclaimed
		};
	}

	bool shouldRotate_() {
		// TODO: We assume that half of total memory is available for CachePages.
		//       Instead, we should track how many non-swappable pages are allocated
		//       and subtract that from the total page count.
		auto totalCachePages = physicalAllocator->numTotalPages() / 2;
		auto threshold = totalCachePages / CacheBundle::numGenerations;
		return rotationTurnaround_.load(std::memory_order_relaxed) >= threshold;
	}

	frg::ticket_spinlock mutex_;

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

	async::recurring_event rotationEvent_;
};

static frg::manual_box<MemoryReclaimer> globalReclaimer;

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
		globalReclaimer->runReclaimFiber();
	}
};

// --------------------------------------------------------
// MemoryView.
// --------------------------------------------------------

coroutine<frg::expected<Error>> MemoryView::resize(size_t newSize) {
	assert(currentIpl() == ipl::exceptionalWork);
	(void)newSize;
	co_return Error::illegalObject;
}

coroutine<frg::expected<Error, smarter::shared_ptr<MemoryView>>> MemoryView::fork() {
	assert(currentIpl() == ipl::exceptionalWork);
	co_return Error::illegalObject;
}

coroutine<frg::expected<Error>> MemoryView::copyTo(uintptr_t offset,
		const void *pointer, size_t size,
		FetchFlags flags) {
	// fetchRequireMutable is managed by this function.
	assert(!(flags & fetchRequireMutable));
	assert(currentIpl() == ipl::exceptionalWork);

	if (auto err = lockRange(offset, size); err != Error::success)
		co_return err;
	frg::scope_exit unlockOnExit{[&] {
		unlockRange(offset, size);
	}};

	size_t progress = 0;
	while(progress < size) {
		auto fetchOffset = (offset + progress) & ~(kPageSize - 1);
		FRG_CO_TRY(co_await touchRange(fetchOffset, kPageSize, flags | fetchRequireMutable));
		auto range = peekRange(fetchOffset, flags | fetchRequireMutable);
		assert(range.physical != PhysicalAddr(-1));
		assert(range.isMutable);

		auto misalign = (offset + progress) & (kPageSize - 1);
		size_t chunk = frg::min(kPageSize - misalign, size - progress);

		PageAccessor accessor{range.physical};
		memcpy(
			reinterpret_cast<uint8_t *>(accessor.get()) + misalign,
			reinterpret_cast<const uint8_t *>(pointer) + progress,
			chunk
		);
		if(auto descriptor = globalPfnDb().find(range.physical))
			markDirty(*descriptor);
		progress += chunk;
	}

	co_return {};
}

coroutine<frg::expected<Error>> MemoryView::copyFrom(uintptr_t offset,
		void *pointer, size_t size,
		FetchFlags flags) {
	// fetchRequireMutable is managed by this function.
	assert(!(flags & fetchRequireMutable));
	assert(currentIpl() == ipl::exceptionalWork);

	if (auto err = lockRange(offset, size); err != Error::success)
		co_return err;
	frg::scope_exit unlockOnExit{[&] {
		unlockRange(offset, size);
	}};

	size_t progress = 0;
	while(progress < size) {
		auto fetchOffset = (offset + progress) & ~(kPageSize - 1);
		FRG_CO_TRY(co_await touchRange(fetchOffset, kPageSize, flags));
		auto range = peekRange(fetchOffset, flags);
		assert(range.physical != PhysicalAddr(-1));

		auto misalign = (offset + progress) & (kPageSize - 1);
		size_t chunk = frg::min(kPageSize - misalign, size - progress);

		PageAccessor accessor{range.physical};
		memcpy(
			reinterpret_cast<uint8_t *>(pointer) + progress,
			reinterpret_cast<uint8_t *>(accessor.get()) + misalign,
			chunk
		);
		progress += chunk;
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
	if (auto err = destView->lockRange(destOffset, size); err != Error::success)
		co_return err;
	frg::scope_exit unlockDestOnExit{[&] {
		destView->unlockRange(destOffset, size);
	}};

	if (auto err = srcView->lockRange(srcOffset, size); err != Error::success)
		co_return err;
	frg::scope_exit unlockSrcOnExit{[&] {
		srcView->unlockRange(srcOffset, size);
	}};

	size_t progress = 0;
	while(progress < size) {
		auto destFetchOffset = (destOffset + progress) & ~(kPageSize - 1);
		auto srcFetchOffset = (srcOffset + progress) & ~(kPageSize - 1);

		FRG_CO_TRY(co_await destView->touchRange(destFetchOffset, kPageSize, fetchRequireMutable));
		auto destRange = destView->peekRange(destFetchOffset, fetchRequireMutable);
		assert(destRange.physical != PhysicalAddr(-1));
		assert(destRange.isMutable);

		FRG_CO_TRY(co_await srcView->touchRange(srcFetchOffset, kPageSize, fetchNone));
		auto srcRange = srcView->peekRange(srcFetchOffset, fetchNone);
		assert(srcRange.physical != PhysicalAddr(-1));

		auto destMisalign = (destOffset + progress) & (kPageSize - 1);
		auto srcMisalign = (srcOffset + progress) & (kPageSize - 1);
		size_t chunk = frg::min(
			frg::min(kPageSize - destMisalign,
			kPageSize - srcMisalign), size - progress
		);

		PageAccessor destAccessor{destRange.physical};
		PageAccessor srcAccessor{srcRange.physical};
		memcpy(
			(uint8_t *)destAccessor.get() + destMisalign,
			(uint8_t *)srcAccessor.get() + srcMisalign,
			chunk
		);
		if(auto descriptor = globalPfnDb().find(destRange.physical))
			markDirty(*descriptor);
		progress += chunk;
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
		auto ptr = smarter::allocate_shared<ZeroMemory>(*kernelAlloc, CtorToken{});
		ptr->selfPtr = ptr;
		return ptr;
	}

	ZeroMemory(CtorToken) {
		_zeroPage = physicalAllocator->allocate(kPageSize);
		if (_zeroPage == PhysicalAddr(-1))
			panicLogger() << "thor: OOM when trying to allocate zero page" << frg::endlog;
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
	auto ptr = smarter::allocate_shared<ImmediateMemory>(*kernelAlloc, CtorToken{});
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
		WorkQueue::generalQueue().lock(),
		[](smarter::shared_ptr<ImmediateMemory> memory, void *base, size_t size) -> coroutine<void> {
			// The pages are still accessible until shootdown completes, so keep the shared_ptr around.
			(void)memory;
			co_await shootdown(
				&KernelPageSpace::global(),
				reinterpret_cast<VirtualAddr>(base),
				size,
				WorkQueue::generalQueue().get()
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
	auto ptr = smarter::allocate_shared<HardwareMemory>(*kernelAlloc, CtorToken{},
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
		size_t length, int addressBits, size_t chunkSize, size_t chunkAlign) {
	auto ptr = smarter::allocate_shared<AllocatedMemory>(*kernelAlloc, CtorToken{},
			length, addressBits, chunkSize, chunkAlign);
	ptr->selfPtr = ptr;
	return ptr;
}

AllocatedMemory::AllocatedMemory(CtorToken, size_t desiredLngth,
		int addressBits, size_t desiredChunkSize, size_t chunkAlign)
: _physicalChunks{*kernelAlloc},
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
			physicalAllocator->free(_physicalChunks[i], _chunkSize);
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

coroutine<frg::expected<Error, size_t>>
AllocatedMemory::touchRange(uintptr_t offset, size_t, FetchFlags) {
	assert(currentIpl() == ipl::exceptionalWork);

	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	auto index = offset / _chunkSize;
	auto misalign = offset & (_chunkSize - 1);

	if(index >= _physicalChunks.size())
		co_return Error::fault;

	if(_physicalChunks[index] == PhysicalAddr(-1)) {
		auto physical = physicalAllocator->allocate(_chunkSize, _addressBits);
		assert(physical != PhysicalAddr(-1) && "OOM");
		assert(!(physical & (_chunkAlign - 1)));

		for(size_t pg_progress = 0; pg_progress < _chunkSize; pg_progress += kPageSize) {
			PageAccessor accessor{physical + pg_progress};
			memset(accessor.get(), 0, kPageSize);

			globalPfnDb().insert(physical + pg_progress, PfnDescriptor::otherPage());
		}
		_physicalChunks[index] = physical;
	}

	assert(_physicalChunks[index] != PhysicalAddr(-1));
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
			|| transactionState == TxState::pendingWriteback
			|| transactionState == TxState::wantWriteback
			|| transactionState == TxState::writeback
			|| stillDirty;
}

std::expected<smarter::shared_ptr<ManagedSpace>, Error> ManagedSpace::create(
		size_t length, bool readahead) {
	if(length > backingMemoryLength)
		return std::unexpected{Error::illegalArgs};
	auto self = smarter::allocate_shared<ManagedSpace>(*kernelAlloc, length, readahead);
	self->selfPtr = self;
	spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(), self->_runReclaimLoop());
	spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(), self->_runDrainLoop());
	spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(), self->_runInvalidationLoop());
	return self;
}

ManagedSpace::ManagedSpace(size_t length, bool readahead)
: pages{*kernelAlloc}, numPages{length >> kPageShift}, readahead{readahead} {
	assert(!(length & (kPageSize - 1)));

	globalReclaimer->registerBundle(this);
}

coroutine<void> ManagedSpace::_runReclaimLoop() {
	while(true) {
		// TODO: Cancel these waits when the ManagedSpace is destructed.
		co_await async::race_and_cancel(
			[&] (async::cancellation_token ct) {
				return globalReclaimer->awaitReclaim(this, ct);
			},
			[&] (async::cancellation_token ct) {
				return async::transform(
					_discardEvent.async_wait_if([this] () -> bool {
						auto irqLock = frg::guard(&irqMutex());
						auto lock = frg::guard(&mutex);
						return _discardList.empty();
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

			globalReclaimer->reclaimPages(this, batch);

			for(auto cachePage : batch) {
				auto *page = frg::container_of(cachePage, &ManagedPage::cachePage);
				assert(page);
				assert(page->loadState == LoadState::present);
				assert(page->transactionState == TxState::inReclaimer);
				assert(!page->lockCount);
				page->transactionState = TxState::performReclaim;
				globalReclaimer->removePage(cachePage);
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

		co_await _evictQueue.fenceEphemeral();

		bool anyDirty = false;
		bool anyExpedite = false;
		bool anyDiscardQueued = false;
		bool anyDiscardErased = false;
		size_t sizeFreed = 0;
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
						anyDiscardQueued |= _disposeDiscarded(page);
					}
					continue;
				}

				if(page->transactionState == TxState::avertReclaim) {
					if(page->stillDirty) {
						page->stillDirty = false;
						_enqueueDirty(page, anyExpedite);
						if(page->swapBudgetClaimed)
							_drainBlocked = false;
						anyDirty = true;
					} else if(page->lockCount
							|| page->cachePage.useCount.load(std::memory_order_relaxed)) {
						page->transactionState = TxState::none;
					} else {
						globalReclaimer->addPage(&page->cachePage);
						page->transactionState = TxState::inReclaimer;
					}
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
			physicalAllocator->free(physical, kPageSize);
			sizeFreed += kPageSize;
		}

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
						anyDiscardQueued |= _disposeDiscarded(page);
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
				_pageDiscarded(page);
				pages.erase(cachePage->identity);
			}

			if(physical != PhysicalAddr(-1)) {
				physicalAllocator->free(physical, kPageSize);
				sizeFreed += kPageSize;
			}
			anyDiscardErased = true;
		}

		_raiseMonitors(pendingMonitors);
		if(anyDirty || anyDiscardErased)
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
}

coroutine<void> ManagedSpace::_runDrainLoop() {
	while(true) {
		co_await _dirtyEvent.async_wait_if([this] () -> bool {
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&mutex);
			return _dirtyList.empty() || _drainBlocked;
		});

		// Delay the writeback such that further dirty pages can accumulate
		// and _progressManagement() can fuse larger requests.
		while(true) {
			uint64_t deadline;
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&mutex);
				if(_writebackExpedited)
					break;
				deadline = _writebackDeadline;
			}
			// Skip the deadline under memory pressure such that dirty pages become reclaimable immediately.
			// TODO: Pressure is only sampled before we sleep.
			//       Waking the sleep would require the physical allocator to signal pressure.
			if(globalReclaimer->checkPressure())
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
							return !_writebackExpedited;
						}, ct),
						[] (auto) { }
					);
				}
			);
		}

		CachePagesList pending;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&mutex);

			// However we left the delay, this pass serves the pending expedite request.
			_writebackExpedited = false;

			auto it = _dirtyList.begin();
			while(it != _dirtyList.end()) {
				auto *cp = *it++;
				auto *page = frg::container_of(cp, &ManagedPage::cachePage);
				assert(page->transactionState == TxState::dirty);
				assert(!page->discarded || page->discardMode == DiscardMode::keepDirty);
				if(!claimSwapBudget(page))
					continue;
				_dequeueDirty(page);
				page->transactionState = TxState::pendingWriteback;
				pending.push_back(cp);
			}

			if(pending.empty() && !_dirtyList.empty())
				_drainBlocked = true;
		}
		if (pending.empty())
			continue;

		co_await _evictQueue.fenceDirty();

		ManageList mgmtPending;
		MonitorPendingList pendingMonitors;
		bool anyDiscardQueued = false;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&mutex);

			while(!pending.empty()) {
				auto *cp = pending.pop_front();
				auto *page = frg::container_of(cp, &ManagedPage::cachePage);
				assert(page->transactionState == TxState::pendingWriteback);
				if(page->discarded) {
					if(page->discardMode == DiscardMode::dropDirty) {
						// The page was discarded while we were waiting for the fence.
						// Note that claimSwapBudget() already ran for this page - _pageDiscarded() undoes the claim.
						auto writebackMonitor = page->detachMonitor(MonitorType::writeback);
						if(writebackMonitor)
							pendingMonitors.push_back(writebackMonitor.release());
						page->transactionState = TxState::none;
						if(_disposeDiscarded(page))
							anyDiscardQueued = true;
						continue;
					}
					assert(page->discardMode == DiscardMode::keepDirty);
				}
				page->transactionState = TxState::wantWriteback;
				_writebackList.push_back(cp);
			}

			_progressManagement(mgmtPending);
		}
		_raiseMonitors(pendingMonitors);
		if(anyDiscardQueued)
			_discardEvent.raise();

		while(!mgmtPending.empty()) {
			auto node = mgmtPending.pop_front();
			node->completionEvent.raise();
		}
	}
}

coroutine<void> ManagedSpace::_runInvalidationLoop() {
	assert(!isSwapSpace);
	while(true) {
		co_await _discardEvent.async_wait_if([this] () -> bool {
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&mutex);
			return _invalidationList.empty();
		});

		CachePagesList batch;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&mutex);

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
			co_await _evictQueue.breakRange(index << kPageShift, count << kPageShift);
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
					raiseDiscard |= _disposeDiscarded(page);
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
}

bool ManagedSpace::claimSwapBudget(ManagedPage *) {
	// File caches write back to their backing store, so no budget applies.
	return true;
}

void ManagedSpace::discardPage(ManagedPage *pit, DiscardMode mode, bool &raiseDiscard,
		bool &raiseExpedite, MonitorPendingList &pendingMonitors) {
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
	case TxState::pendingWriteback:
		// The drain coroutine completes the discard, the page is on
		// its local pending list.
		break;
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

	// Erases entries without a frame right away instead of paying for a fenceEphemeral().
	if(dispose) {
		assert(pit->transactionState == TxState::none);
		assert(!pit->monitors);
		if(pit->physical == PhysicalAddr(-1)
				&& !pit->lockCount
				&& !pit->cachePage.useCount.load(std::memory_order_relaxed)) {
			auto index = pit->cachePage.identity;
			_pageDiscarded(pit);
			pages.erase(index);
		} else {
			raiseDiscard |= _disposeDiscarded(pit);
		}
	}
}

void ManagedSpace::_enqueueDirty(ManagedPage *page, bool &raiseExpedite) {
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

bool ManagedSpace::_disposeDiscarded(ManagedPage *page) {
	assert(page->discarded);
	assert(page->transactionState == TxState::none);
	// Only the discard monitor may outlive the page's last transaction.
	assert(!(page->attachedMonitors
			& ~(uint8_t{1} << static_cast<unsigned int>(MonitorType::discard))));
	if(page->lockCount)
		return false;
	if(page->cachePage.useCount.load(std::memory_order_relaxed)) {
		// Swap slot identities cannot be translated back to view offsets;
		// hence, swap spaces cannot use TxState::invalidation.
		// Instead, discarded pages will be re-routed to _disposeDiscarded() when their useCount drops to zero.
		if(isSwapSpace)
			return false;
		page->transactionState = TxState::invalidation;
		_invalidationList.push_back(&page->cachePage);
		return true;
	}
	page->transactionState = TxState::discardQueued;
	_discardList.push_back(&page->cachePage);
	return true;
}

void ManagedSpace::_raiseMonitors(MonitorPendingList &pendingMonitors) {
	while(!pendingMonitors.empty()) {
		frg::intrusive_shared_ptr<TransactionMonitor, Allocator> monitor{
			frg::adopt_rc, pendingMonitors.pop_front()
		};
		monitor->event.raise();
	}
}

void ManagedSpace::_pageDiscarded(ManagedPage *) {}

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

std::expected<smarter::shared_ptr<SwapSpace>, Error> SwapSpace::create() {
	auto self = smarter::allocate_shared<SwapSpace>(*kernelAlloc);
	self->selfPtr = self;
	spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(), self->_runReclaimLoop());
	spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(), self->_runDrainLoop());
	// TODO: Don't leak the swap spaces.
	self.policy().increment();
	return self;
}

SwapSpace::SwapSpace()
: ManagedSpace{UINT64_C(1) << 32, false}, _buddyMetadata{*kernelAlloc} {
	isSwapSpace = true;

	assert(numPages);
	auto tableOrder = BuddyAccessor::suitableOrder(numPages);
	auto numRoots = numPages >> tableOrder;
	_buddyMetadata.resize(BuddyAccessor::determineSize(numRoots, tableOrder));
	BuddyAccessor::initialize(_buddyMetadata.data(), numRoots, tableOrder);
	_buddyAccessor = BuddyAccessor{0, 0, _buddyMetadata.data(), numRoots, tableOrder};
}

bool SwapSpace::claimSwapBudget(ManagedPage *page) {
	if(page->swapBudgetClaimed)
		return true;
	if(_budgetClaimed >= _budget)
		return false;
	_budgetClaimed++;
	page->swapBudgetClaimed = true;
	return true;
}

void SwapSpace::_pageDiscarded(ManagedPage *page) {
	if(page->swapBudgetClaimed) {
		assert(_budgetClaimed);
		_budgetClaimed--;
		page->swapBudgetClaimed = false;
		_drainBlocked = false;
	}
	_freeOffset(page->cachePage.identity);
}

void SwapSpace::setBudget(size_t numSlots) {
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex);
		_budget = numSlots;
	}
	_wakeDrain();
}

frg::optional<uint64_t> SwapSpace::_allocateOffset() {
	auto offset = _buddyAccessor.allocate(0, 64);
	if(offset == BuddyAccessor::illegalAddress)
		return frg::null_opt;
	return offset;
}

void SwapSpace::_freeOffset(uint64_t offset) {
	assert(offset < numPages);
	_buddyAccessor.free(offset, 0);
}

ManagedSpace::~ManagedSpace() {
	// TODO: Free all physical memory.
	// TODO: We also have to remove all Loaded/Evicting pages from the reclaimer.
	assert(!"Implement this");
}

// Note: Neither offset nor size are necessarily multiples of the page size.
Error ManagedSpace::lockPages(uintptr_t offset, size_t size) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&mutex);

	for(size_t pg = 0; pg < size; pg += kPageSize) {
		size_t index = (offset + pg) / kPageSize;
		auto [pit, wasInserted] = pages.find_or_insert(index, this, index);
		assert(pit);
		pit->lockCount++;
		if(pit->lockCount == 1) {
			if(pit->loadState == LoadState::present && pit->transactionState == TxState::inReclaimer) {
				globalReclaimer->removePage(&pit->cachePage);
				pit->transactionState = TxState::none;
			}else if(pit->transactionState == TxState::performReclaim) {
				pit->transactionState = TxState::avertReclaim;
			}else if(pit->transactionState == TxState::discardQueued
					|| pit->transactionState == TxState::performDiscard) {
				// Handle discardQueued pages by moving them into avertDiscard.
				// This avoids raising monitors on this code path.
				pit->transactionState = TxState::avertDiscard;
			}
		}
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
			auto pit = pages.find(index);
			assert(pit);
			assert(pit->lockCount > 0);
			pit->lockCount--;
			if(!pit->lockCount) {
				if(pit->discarded) {
					// If a transaction is still in flight, the page stays owned by it;
					// the transaction's completion path disposes of the page instead.
					if(pit->transactionState == TxState::none)
						raiseDiscard |= _disposeDiscarded(pit);
				} else if(pit->loadState == LoadState::present
						&& pit->transactionState == TxState::none
						&& !pit->cachePage.useCount.load(std::memory_order_relaxed)) {
					globalReclaimer->addPage(&pit->cachePage);
					pit->transactionState = TxState::inReclaimer;
				}
			}
		}
	}
	if(raiseDiscard)
		_discardEvent.raise();
}

void ManagedSpace::submitManagement(ManageNode *node) {
	ManageList pending;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex);

		_managementQueue.push_back(node);
		_progressManagement(pending);
	}

	while(!pending.empty()) {
		auto node = pending.pop_front();
		node->completionEvent.raise();
	}
}

void ManagedSpace::_progressManagement(ManageList &pending) {
	// For now, we prefer writeback to initialization.
	// "Proper" priorization should probably be done in the userspace driver
	// (we do not want to store per-page priorities here).

	while(!_writebackList.empty() && !_managementQueue.empty()) {
		auto page = _writebackList.front();
		auto index = page->identity;

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
					raiseDiscard = _disposeDiscarded(page);
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
				globalReclaimer->removePage(cachePage);
			_enqueueDirty(page, needsExpedite);
			// When the drain coroutine is blocked on swap budget, only pages that
			// already claimed swap budget can enter writeback.
			if(!_drainBlocked || page->swapBudgetClaimed) {
				_drainBlocked = false;
				needsEvent = true;
			}
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
		} else {
			assert(page->transactionState == TxState::dirty
					|| page->transactionState == TxState::pendingWriteback
					|| page->transactionState == TxState::wantWriteback);
		}
	}

	if(needsEvent)
		_dirtyEvent.raise();
	if(needsExpedite)
		_expediteEvent.raise();
}

// --------------------------------------------------------
// BackingMemory
// --------------------------------------------------------

std::expected<smarter::shared_ptr<BackingMemory>, Error> BackingMemory::create(
		smarter::shared_ptr<ManagedSpace> managed) {
	auto ptr = smarter::allocate_shared<BackingMemory>(*kernelAlloc, CtorToken{}, std::move(managed));
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

	auto pit = _managed->pages.find(index);
	if(!pit)
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

coroutine<frg::expected<Error, size_t>>
BackingMemory::touchRange(uintptr_t offset, size_t, FetchFlags) {
	assert(currentIpl() == ipl::exceptionalWork);

	auto index = offset >> kPageShift;
	auto misalign = offset & (kPageSize - 1);

	if(offset >= backingMemoryLength)
		co_return Error::fault;

	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_managed->mutex);

	auto [pit, wasInserted] = _managed->pages.find_or_insert(index, _managed.get(), index);
	assert(pit);

	if(pit->transactionState == ManagedSpace::TxState::performReclaim) {
		pit->transactionState = ManagedSpace::TxState::avertReclaim;
	} else if(pit->transactionState == ManagedSpace::TxState::performDiscard) {
		pit->transactionState = ManagedSpace::TxState::avertDiscard;
	}

	if(pit->physical == PhysicalAddr(-1)) {
		PhysicalAddr physical = physicalAllocator->allocate(kPageSize);
		assert(physical != PhysicalAddr(-1) && "OOM");

		PageAccessor accessor{physical};
		memset(accessor.get(), 0, kPageSize);

		globalPfnDb().insert(physical, PfnDescriptor::cachePage(&pit->cachePage));
		pit->physical = physical;
	}

	co_return kPageSize - misalign;
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
			auto pit = _managed->pages.find(index);
			if(!pit || pit->transactionState != expectedState)
				return Error::illegalArgs;
		}

		if(type == ManageRequest::initialize) {
			for(size_t pg = 0; pg < length; pg += kPageSize) {
				size_t index = (offset + pg) / kPageSize;
				auto pit = _managed->pages.find(index);
				pit->loadState = ManagedSpace::LoadState::present;
				auto monitor = pit->detachMonitor(ManagedSpace::MonitorType::initialization);
				if(monitor)
					pendingMonitors.push_back(monitor.release());
				if(pit->discarded) {
					// The page completed initialization normally but will now be discarded.
					pit->transactionState = ManagedSpace::TxState::none;
					raiseDiscard |= _managed->_disposeDiscarded(pit);
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
				auto pit = _managed->pages.find(index);

				if(pit->discarded) {
					if(pit->discardMode == DiscardMode::dropDirty) {
						// Raise the monitor as usual - writebackFence() waiters hold references to it.
						auto monitor = pit->detachMonitor(ManagedSpace::MonitorType::writeback);
						if(monitor)
							pendingMonitors.push_back(monitor.release());
						// The frame is being discarded so it doesn't need to be written back.
						pit->stillDirty = false;
						pit->transactionState = ManagedSpace::TxState::none;
						raiseDiscard |= _managed->_disposeDiscarded(pit);
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
						raiseDiscard |= _managed->_disposeDiscarded(pit);
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

coroutine<frg::expected<Error>> BackingMemory::writebackFence(uintptr_t offset, size_t size) {
	assert(currentIpl() == ipl::exceptionalWork);
	if (offset & (kPageSize - 1))
		co_return Error::illegalArgs;
	if (size & (kPageSize - 1))
		co_return Error::illegalArgs;
	if (offset > backingMemoryLength || size > backingMemoryLength - offset)
		co_return Error::bufferTooSmall;

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
				if(it == _managed->pages.end() || it->cachePage.identity >= limitPage) {
					exhausted = true;
					break;
				}
				auto *pit = &*it;
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

				auto pit = _managed->pages.find(index);
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
				if(it == _managed->pages.end() || it->cachePage.identity >= limitPage) {
					exhausted = true;
					break;
				}
				// discardPage() can erase the entry, so advance first.
				auto *page = &*it;
				markCursor = page->cachePage.identity + 1;
				++it;
				_managed->discardPage(page, mode, raiseDiscard, raiseExpedite, pendingMonitors);
			}
		}
		ManagedSpace::_raiseMonitors(pendingMonitors);
		if(exhausted)
			break;
	}

	// Wake the invalidation coroutine only after everything is marked as discarded.
	// This helps the invalidation coroutine to coelesce ranges.
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
			while(it != _managed->pages.end() && it->cachePage.identity < limitPage
					&& !it->discarded)
				++it;

			if(it == _managed->pages.end() || it->cachePage.identity >= limitPage) {
				done = true;
			} else {
				waitCursor = it->cachePage.identity;
				monitor = it->requireMonitor(ManagedSpace::MonitorType::discard);
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
	auto ptr = smarter::allocate_shared<FrontalMemory>(*kernelAlloc, CtorToken{}, std::move(managed));
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
	auto pit = _managed->pages.find(index);
	if(!pit)
		return PhysicalRange{};

	if(pit->loadState == ManagedSpace::LoadState::present) {
		auto physical = pit->physical;
		assert(physical != PhysicalAddr(-1));

		if(pit->transactionState == ManagedSpace::TxState::performReclaim) {
			pit->transactionState = ManagedSpace::TxState::avertReclaim;
		} else if(pit->transactionState == ManagedSpace::TxState::performDiscard) {
			pit->transactionState = ManagedSpace::TxState::avertDiscard;
		}

		return PhysicalRange{
			.physical = physical + misalign,
			.size = kPageSize - misalign,
			.cachingMode = CachingMode::null,
			.isMutable = true
		};
	}else{
		assert(pit->loadState == ManagedSpace::LoadState::missing);
		return PhysicalRange{};
	}
}

coroutine<frg::expected<Error, size_t>>
FrontalMemory::touchRange(uintptr_t offset, size_t, FetchFlags flags) {
	assert(currentIpl() == ipl::exceptionalWork);

	auto index = offset >> kPageShift;
	auto misalign = offset & (kPageSize - 1);

	ManageList pendingManagement;
	frg::intrusive_shared_ptr<ManagedSpace::TransactionMonitor, Allocator> fetchMonitor;
	{
		auto irq_lock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_managed->mutex);

		if(index >= _managed->numPages)
			co_return Error::fault;

		// Try the fast-paths first.
		auto [pit, wasInserted] = _managed->pages.find_or_insert(index, _managed.get(), index);
		assert(pit);
		if(pit->loadState == ManagedSpace::LoadState::present) {
			assert(pit->physical != PhysicalAddr(-1));

			if(pit->transactionState == ManagedSpace::TxState::inReclaimer) {
				globalReclaimer->bumpPage(&pit->cachePage);
			}else if(pit->transactionState == ManagedSpace::TxState::performReclaim) {
				pit->transactionState = ManagedSpace::TxState::avertReclaim;
			}else if(pit->transactionState == ManagedSpace::TxState::performDiscard) {
				pit->transactionState = ManagedSpace::TxState::avertDiscard;
			}

			co_return kPageSize - misalign;
		}else{
			assert(pit->loadState == ManagedSpace::LoadState::missing);
		}

		if(flags & fetchDisallowBacking) {
			urgentLogger() << "thor: Backing of page is disallowed" << frg::endlog;
			co_return Error::fault;
		}

		// We have to take the slow-path, i.e., perform the fetch asynchronously.
		if(pit->loadState == ManagedSpace::LoadState::missing
				&& pit->transactionState == ManagedSpace::TxState::none) {
			pit->transactionState = ManagedSpace::TxState::wantInitialization;
			_managed->_initializationList.push_back(&pit->cachePage);
		}

		// Perform readahead.
		if(_managed->readahead)
			for(size_t i = 1; i < 4; ++i) {
				if(!(index + i < _managed->numPages))
					break;
				auto [pit, wasInserted] = _managed->pages.find_or_insert(
						index + i, _managed.get(), index + i);
				assert(pit);
				if(pit->loadState == ManagedSpace::LoadState::missing
						&& pit->transactionState == ManagedSpace::TxState::none) {
					pit->transactionState = ManagedSpace::TxState::wantInitialization;
					_managed->_initializationList.push_back(&pit->cachePage);
				}
			}

		_managed->_progressManagement(pendingManagement);

		assert(pit->transactionState == ManagedSpace::TxState::wantInitialization
				|| pit->transactionState == ManagedSpace::TxState::initialization);
		fetchMonitor = pit->requireMonitor(ManagedSpace::MonitorType::initialization);
	}

	while(!pendingManagement.empty()) {
		auto node = pendingManagement.pop_front();
		node->completionEvent.raise();
	}

	co_await fetchMonitor->event.wait();

	co_return kPageSize - misalign;
}

size_t FrontalMemory::getLength() {
	// Size is constant so we do not need to lock.
	return _managed->numPages << kPageShift;
}

// --------------------------------------------------------
// SwappableMemory
// --------------------------------------------------------

std::expected<smarter::shared_ptr<SwappableMemory>, Error> SwappableMemory::create(
		smarter::shared_ptr<SwapSpace> space, size_t length) {
	auto ptr = smarter::allocate_shared<SwappableMemory>(*kernelAlloc, CtorToken{},
			std::move(space), length);
	ptr->selfPtr = ptr;
	return ptr;
}

SwappableMemory::SwappableMemory(CtorToken, smarter::shared_ptr<SwapSpace> space, size_t length)
: MemoryView{&space->_evictQueue}, _space{std::move(space)}, _length{length}, _table{*kernelAlloc} {
	assert(!(length & (kPageSize - 1)));
}

SwappableMemory::~SwappableMemory() {
	for(auto it = _table.begin(); it != _table.end(); ++it) {
		bool raiseDiscard = false;
		bool raiseExpedite = false;
		ManagedSpace::MonitorPendingList pendingMonitors;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&_space->mutex);

			auto pit = _space->pages.find(*it);
			assert(pit);
			_space->discardPage(pit, DiscardMode::dropDirty,
					raiseDiscard, raiseExpedite, pendingMonitors);
		}
		ManagedSpace::_raiseMonitors(pendingMonitors);
		if(raiseDiscard)
			_space->_discardEvent.raise();
		if(raiseExpedite)
			_space->_expediteEvent.raise();
	}
	// Discarding may have released swap budget.
	_space->_wakeDrain();
}

frg::optional<uint64_t> SwappableMemory::_translate(uint64_t index) {
	auto tit = _table.find(index);
	if(tit)
		return *tit;

	auto allocated = _space->_allocateOffset();
	if(!allocated)
		return frg::null_opt;
	_table.insert(index, *allocated);

	auto [pit, wasInserted] = _space->pages.find_or_insert(*allocated, _space.get(), *allocated);
	assert(pit);
	assert(wasInserted);
	return *allocated;
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
			auto swapOffset = _translate(index);
			if(!swapOffset) {
				// The swap space is exhausted, unwind the locks we already took.
				_unlockPagesLocked(offset, pg, raiseDiscard);
				result = Error::noMemory;
				break;
			}
			auto pit = _space->pages.find(*swapOffset);
			assert(pit);
			if(++pit->lockCount == 1) {
				if(pit->loadState == ManagedSpace::LoadState::present
						&& pit->transactionState == ManagedSpace::TxState::inReclaimer) {
					globalReclaimer->removePage(&pit->cachePage);
					pit->transactionState = ManagedSpace::TxState::none;
				}else if(pit->transactionState == ManagedSpace::TxState::performReclaim) {
					pit->transactionState = ManagedSpace::TxState::avertReclaim;
				}else if(pit->transactionState == ManagedSpace::TxState::discardQueued
						|| pit->transactionState == ManagedSpace::TxState::performDiscard) {
					// Handle discardQueued pages by moving them into avertDiscard.
					// This avoids raising monitors on this code path.
					pit->transactionState = ManagedSpace::TxState::avertDiscard;
				}
			}
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
		auto pit = _space->pages.find(*tit);
		assert(pit);
		assert(pit->lockCount);
		pit->lockCount--;
		if(!pit->lockCount) {
			if(pit->discarded) {
				if(pit->transactionState == ManagedSpace::TxState::none)
					raiseDiscard |= _space->_disposeDiscarded(pit);
			} else if(pit->loadState == ManagedSpace::LoadState::present
					&& pit->transactionState == ManagedSpace::TxState::none
					&& !pit->cachePage.useCount.load(std::memory_order_relaxed)) {
				globalReclaimer->addPage(&pit->cachePage);
				pit->transactionState = ManagedSpace::TxState::inReclaimer;
			}
		}
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
	auto pit = _space->pages.find(*tit);
	assert(pit);

	if(pit->loadState == ManagedSpace::LoadState::present) {
		auto physical = pit->physical;
		assert(physical != PhysicalAddr(-1));

		if(pit->transactionState == ManagedSpace::TxState::performReclaim) {
			pit->transactionState = ManagedSpace::TxState::avertReclaim;
		} else if(pit->transactionState == ManagedSpace::TxState::performDiscard) {
			pit->transactionState = ManagedSpace::TxState::avertDiscard;
		}

		return PhysicalRange{
			.physical = physical + misalign,
			.size = kPageSize - misalign,
			.cachingMode = CachingMode::null,
			.isMutable = true
		};
	}

	assert(pit->loadState == ManagedSpace::LoadState::missing);
	return PhysicalRange{};
}

coroutine<frg::expected<Error, size_t>>
SwappableMemory::touchRange(uintptr_t offset, size_t, FetchFlags flags) {
	assert(currentIpl() == ipl::exceptionalWork);

	auto index = offset >> kPageShift;
	auto misalign = offset & (kPageSize - 1);

	// Frame for the zero-fill path. Allocated and zeroed outside of the
	// SwapSpace mutex, then installed under the mutex after re-checking that no
	// other thread allocated the frame meanwhile.
	PhysicalAddr freshPhysical(-1);
	frg::scope_exit freeFreshFrame{[&] {
		if(freshPhysical != PhysicalAddr(-1))
			physicalAllocator->free(freshPhysical, kPageSize);
	}};

	while(true) {
		ManageList pendingManagement;
		frg::intrusive_shared_ptr<ManagedSpace::TransactionMonitor, Allocator> fetchMonitor;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&_space->mutex);

			if(index >= (_length >> kPageShift))
				co_return Error::fault;

			auto swapOffset = _translate(index);
			if(!swapOffset)
				co_return Error::noMemory;
			auto pit = _space->pages.find(*swapOffset);
			assert(pit);

			if(pit->loadState == ManagedSpace::LoadState::present) {
				assert(pit->physical != PhysicalAddr(-1));

				if(pit->transactionState == ManagedSpace::TxState::inReclaimer) {
					globalReclaimer->bumpPage(&pit->cachePage);
				}else if(pit->transactionState == ManagedSpace::TxState::performReclaim) {
					pit->transactionState = ManagedSpace::TxState::avertReclaim;
				}else if(pit->transactionState == ManagedSpace::TxState::performDiscard) {
					pit->transactionState = ManagedSpace::TxState::avertDiscard;
				}

				co_return kPageSize - misalign;
			}
			assert(pit->loadState == ManagedSpace::LoadState::missing);

			if(!pit->swapCopyValid) {
				// The page is logically all-zero - zero-fill it synchronously.
				assert(pit->transactionState == ManagedSpace::TxState::none);
				if(freshPhysical != PhysicalAddr(-1)) {
					globalPfnDb().insert(freshPhysical,
							PfnDescriptor::cachePage(&pit->cachePage));
					pit->physical = freshPhysical;
					pit->loadState = ManagedSpace::LoadState::present;
					freshPhysical = PhysicalAddr(-1);
					if(!pit->lockCount
							&& !pit->cachePage.useCount.load(std::memory_order_relaxed)) {
						globalReclaimer->addPage(&pit->cachePage);
						pit->transactionState = ManagedSpace::TxState::inReclaimer;
					}
					co_return kPageSize - misalign;
				}
				// ... no frame at hand, allocate one below without the mutex held and retry.
			}else{
				// The page is swapped out, read it back via the manage protocol.
				if(flags & fetchDisallowBacking) {
					urgentLogger() << "thor: Backing of swapped-out page is disallowed"
							<< frg::endlog;
					co_return Error::fault;
				}

				if(pit->transactionState == ManagedSpace::TxState::none) {
					pit->transactionState = ManagedSpace::TxState::wantInitialization;
					_space->_initializationList.push_back(&pit->cachePage);
				}

				_space->_progressManagement(pendingManagement);
				assert(pit->transactionState == ManagedSpace::TxState::wantInitialization
						|| pit->transactionState == ManagedSpace::TxState::initialization);
				fetchMonitor = pit->requireMonitor(ManagedSpace::MonitorType::initialization);
			}
		}

		if(fetchMonitor) {
			while(!pendingManagement.empty()) {
				auto node = pendingManagement.pop_front();
				node->completionEvent.raise();
			}

			co_await fetchMonitor->event.wait();
			co_return kPageSize - misalign;
		}

		assert(freshPhysical == PhysicalAddr(-1));
		freshPhysical = physicalAllocator->allocate(kPageSize);
		assert(freshPhysical != PhysicalAddr(-1) && "OOM");
		PageAccessor accessor{freshPhysical};
		memset(accessor.get(), 0, kPageSize);
	}
}

// --------------------------------------------------------
// IndirectMemory
// --------------------------------------------------------

std::expected<smarter::shared_ptr<IndirectMemory>, Error> IndirectMemory::create(size_t numSlots) {
	auto ptr = smarter::allocate_shared<IndirectMemory>(*kernelAlloc, CtorToken{}, numSlots);
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
	// TODO: start a coroutine to observe evictions.
	memory->addObserver(&indirection->observer);
	indirections_[slot] = std::move(indirection);
	return Error::success;
}

// --------------------------------------------------------
// CopyOnWriteMemory
// --------------------------------------------------------

CowPage::~CowPage() {
	if(state == CowState::null)
		return;
	assert(state == CowState::hasCopy);
	assert(physical != PhysicalAddr(-1));
	globalPfnDb().erase(physical);
	physicalAllocator->free(physical, kPageSize);
}

std::expected<smarter::shared_ptr<CopyOnWriteMemory>, Error> CopyOnWriteMemory::create(
		smarter::shared_ptr<MemoryView> view, uintptr_t offset, size_t length) {
	auto ptr = smarter::allocate_shared<CopyOnWriteMemory>(*kernelAlloc, CtorToken{},
			std::move(view), offset, length, nullptr);
	ptr->selfPtr = ptr;
	return ptr;
}

CopyOnWriteMemory::CopyOnWriteMemory(CtorToken, smarter::shared_ptr<MemoryView> view,
		uintptr_t offset, size_t length,
		smarter::shared_ptr<CowChain> chain)
: MemoryView{&_evictQueue}, _view{std::move(view)},
		_viewOffset{offset}, _length{length}, _copyChain{std::move(chain)},
		_ownedPages{*kernelAlloc} {
	assert(length);
	assert(!(offset & (kPageSize - 1)));
	assert(!(length & (kPageSize - 1)));
}

CopyOnWriteMemory::~CopyOnWriteMemory() {
}

size_t CopyOnWriteMemory::getLength() {
	return _length;
}

coroutine<frg::expected<Error, smarter::shared_ptr<MemoryView>>> CopyOnWriteMemory::fork() {
	assert(currentIpl() == ipl::exceptionalWork);

	// Note that locked pages require special attention during CoW: as we cannot
	// replace them by copies, we have to copy them eagerly.
	// Therefore, they are special-cased below.
	smarter::shared_ptr<CopyOnWriteMemory> forked;
	smarter::shared_ptr<CowChain> newChain;
	frg::vector<frg::tuple<size_t, smarter::shared_ptr<CowPage>>, KernelAlloc> inProgressPages{*kernelAlloc};
	frg::vector<frg::tuple<size_t, smarter::shared_ptr<CowPage>>, KernelAlloc> lockedCopies{*kernelAlloc};

	// Note: We turn owned pages into shared pages while holding the locks below.
	//       Since this happens while the lock is held, peekRange() and touchRange() can never
	//       see intermediate states (e.g., pages that are already removed from _ownedPages
	//       but that are not available in _copyChain yet).

	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		// Create a new CowChain for both the original and the forked mapping.
		// To correct handle locks pages, we move only non-locked pages from
		// the original mapping to the new chain.
		auto curChain = _copyChain;
		newChain = smarter::allocate_shared<CowChain>(*kernelAlloc);

		// Update the original mapping
		_copyChain = newChain;

		// Create a new mapping in the forked space.
		forked = smarter::allocate_shared<CopyOnWriteMemory>(*kernelAlloc, CtorToken{},
				_view, _viewOffset, _length, newChain);
		forked->selfPtr = forked;

		// Inspect all copied pages owned by the original mapping.
		for(size_t pg = 0; pg < _length; pg += kPageSize) {
			auto it = _ownedPages.find(pg >> kPageShift);
			smarter::shared_ptr<CowPage> page;
			if(it)
				page = *it;

			// Pages in state null only hold a lock count, so they do not shadow the chain.
			if(!page || page->state == CowState::null) {
				// If the page is missing in this memory object, look at the CowChain.
				auto pageOffset = _viewOffset + pg;
				if (curChain) {
					auto chainLock = frg::guard(&curChain->_mutex);

					if(auto it = curChain->_pages.find(pageOffset >> kPageShift); it) {
						auto chainPage = *it;
						assert(chainPage->state == CowState::hasCopy);
						auto newIt = newChain->_pages.insert(pageOffset >> kPageShift);
						*newIt = chainPage;
					}
				}
				continue;
			}

			if(page->state == CowState::inProgress) {
				// We wait for the in progress pages later, as we
				// need to drop the locks we're holding before
				// suspending, but they are ensuring consistency
				// of the object we're working on.
				inProgressPages.push(frg::make_tuple(pg, page));
				continue;
			}else
				assert(page->state == CowState::hasCopy);

			if(page->lockCount /*|| disableCow */) {
				// The page is locked. We *need* to keep it in the old address space.
				lockedCopies.push(frg::make_tuple(pg, page));
			}else{
				assert(page->physical != PhysicalAddr(-1));

				auto pageOffset = _viewOffset + pg;
				auto newIt = newChain->_pages.insert(pageOffset >> kPageShift);
				*newIt = page;
				_ownedPages.erase(pg >> kPageShift);
			}
		}
	}

	// Wait for the in progress pages to complete copying.
	bool stillWaiting = inProgressPages.size() > 0;
	while (stillWaiting) {
		stillWaiting = co_await _copyEvent.async_wait_if([&] {
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&_mutex);

			for (auto [_, inProgressPage] : inProgressPages) {
				if (inProgressPage->state == CowState::inProgress)
					return true;
			}

			return false;
		});
	}

	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		// Copy all the previously in progress pages now that they're done copying.
		for (auto [pg, page] : inProgressPages) {
			assert(page->state == CowState::hasCopy);

			if(page->lockCount /*|| disableCow */) {
				// The page is locked. We *need* to keep it in the old address space.
				lockedCopies.push(frg::make_tuple(pg, page));
			}else{
				assert(page->physical != PhysicalAddr(-1));

				auto pageOffset = _viewOffset + pg;
				auto newIt = newChain->_pages.insert(pageOffset >> kPageShift);
				*newIt = page;
				_ownedPages.erase(pg >> kPageShift);
			}
		}
	}

	// Copy all the pages that were locked.
	for(auto [pg, src] : lockedCopies) {
		auto copyPhysical = physicalAllocator->allocate(kPageSize);
		assert(copyPhysical != PhysicalAddr(-1) && "OOM");

		PageAccessor lockedAccessor{src->physical};
		PageAccessor copyAccessor{copyPhysical};
		memcpy(copyAccessor.get(), lockedAccessor.get(), kPageSize);

		auto copyPage = smarter::allocate_shared<CowPage>(*kernelAlloc);
		copyPage->state = CowState::hasCopy;
		copyPage->physical = copyPhysical;
		globalPfnDb().insert(copyPhysical, PfnDescriptor::otherPage());
		auto copyIt = forked->_ownedPages.insert(pg >> kPageShift);
		*copyIt = copyPage;
	}

	co_await _evictQueue.breakRange(0, _length);
	co_return smarter::shared_ptr<MemoryView>{std::move(forked)};
}

Error CopyOnWriteMemory::lockRange(uintptr_t offset, size_t size) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	for(size_t pg = 0; pg < size; pg += kPageSize) {
		auto it = _ownedPages.find((offset + pg) >> kPageShift);
		if(it) {
			auto page = *it;
			page->lockCount++;
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
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	for(size_t pg = 0; pg < size; pg += kPageSize) {
		auto it = _ownedPages.find((offset + pg) >> kPageShift);
		assert(it);
		auto page = *it;
		assert(page->lockCount > 0);
		page->lockCount--;
	}
}

PhysicalRange CopyOnWriteMemory::peekRange(uintptr_t offset, FetchFlags flags) {
	auto misalign = offset & (kPageSize - 1);

	smarter::shared_ptr<CowChain> chain;
	smarter::shared_ptr<MemoryView> view;
	uintptr_t viewOffset;
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
				assert(page->physical != PhysicalAddr(-1));
				return PhysicalRange{
					.physical = page->physical + misalign,
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

		chain = _copyChain;
		view = _view;
		viewOffset = _viewOffset;
	}
	// Note: totalOffset is not necessarily page aligned.
	auto totalOffset = viewOffset + offset;

	if (passthrough) {
		if (chain) {
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&chain->_mutex);

			if(auto it = chain->_pages.find(totalOffset >> kPageShift); it) {
				auto page = *it;
				return PhysicalRange{
					.physical = page->physical + misalign,
					.size = kPageSize - misalign,
					.cachingMode = CachingMode::null,
					.isMutable = false
				};
			}
		}

		auto range = view->peekRange(totalOffset, flags);
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

coroutine<frg::expected<Error, size_t>>
CopyOnWriteMemory::touchRange(uintptr_t offset, size_t sizeHint, FetchFlags flags) {
	assert(currentIpl() == ipl::exceptionalWork);

	auto misalign = offset & (kPageSize - 1);
	auto alignedOffset = offset & ~(kPageSize - 1);

	smarter::shared_ptr<CowChain> chain;
	smarter::shared_ptr<MemoryView> view;
	uintptr_t viewOffset;
	smarter::shared_ptr<CowPage> cowPage;
	// Note: the passthrough cases here have to match peekRange() since
	//       callers expect touchRange() to make the page available to peekRange().
	bool passthrough = false;
	bool waitForCopy = false;
	{
		// If the page is present in our private chain, we just return it.
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		if(offset >= _length)
			co_return Error::fault;

		auto cowIt = _ownedPages.find(offset >> kPageShift);
		if(cowIt) {
			cowPage = *cowIt;
			if(cowPage->state == CowState::hasCopy) {
				assert(cowPage->physical != PhysicalAddr(-1));
				co_return kPageSize - misalign;
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
				// Otherwise we need to copy from the chain or from the root view.
				cowPage = smarter::allocate_shared<CowPage>(*kernelAlloc);
				cowPage->state = CowState::inProgress;
				cowIt = _ownedPages.insert(offset >> kPageShift);
				*cowIt = cowPage;
			}
		}

		chain = _copyChain;
		view = _view;
		viewOffset = _viewOffset;
	}
	// Note: totalOffset is not necessarily page aligned.
	auto totalOffset = viewOffset + offset;
	auto pageOffset = totalOffset & ~(kPageSize - 1);

	// Passthrough and waitForCopy are mutually exclusive:
	// if waitForCopy is set, we may need to wait for eviction to finish
	// and we must not return passed through pages after eviction started.
	assert(!(passthrough && waitForCopy));

	if(passthrough) {
		if (chain) {
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&chain->_mutex);

			if(chain->_pages.find(totalOffset >> kPageShift))
				co_return kPageSize - misalign;
		}

		auto affectedSize = FRG_CO_TRY(co_await view->touchRange(totalOffset, sizeHint, flags));
		co_return frg::min(affectedSize, kPageSize - misalign);
	}

	if(waitForCopy) {
		bool stillWaiting;
		do {
			stillWaiting = co_await _copyEvent.async_wait_if([&] () -> bool {
				// TODO: this could be faster if cowIt->state was atomic.
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&_mutex);

				if(cowPage->state == CowState::inProgress)
					return true;
				assert(cowPage->state == CowState::hasCopy);
				return false;
			});
		} while(stillWaiting);

		co_return kPageSize - misalign;
	}

	PhysicalAddr physical = physicalAllocator->allocate(kPageSize);
	assert(physical != PhysicalAddr(-1) && "OOM");
	PageAccessor accessor{physical};

	// Try to copy from a descendant CoW chain.
	bool chainHasCopy = false;
	if(chain) {
		smarter::shared_ptr<CowPage> srcPage;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&chain->_mutex);

			if(auto it = chain->_pages.find(pageOffset >> kPageShift); it) {
				srcPage = *it;
				assert(srcPage->state == CowState::hasCopy);
				assert(srcPage->physical != PhysicalAddr(-1));
			}
		}

		// Copy outside of the locks (srcPage remains in hasCopy state).
		if (srcPage) {
			auto srcAccessor = PageAccessor{srcPage->physical};
			memcpy(accessor.get(), srcAccessor.get(), kPageSize);
			chainHasCopy = true;
		}
	}

	// Copy from the root view.
	if(!chainHasCopy) {
		FRG_CO_TRY(co_await view->copyFrom(pageOffset, accessor.get(), kPageSize));
	}

	// To make CoW unobservable, we first need to evict the page here.
	co_await _evictQueue.breakRange(alignedOffset, kPageSize);

	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		assert(cowPage->state == CowState::inProgress);
		cowPage->state = CowState::hasCopy;
		cowPage->physical = physical;
		globalPfnDb().insert(physical, PfnDescriptor::otherPage());
	}
	_copyEvent.raise();
	co_return kPageSize - misalign;
}

// --------------------------------------------------------------------------------------

namespace {
	frg::eternal<FutexRealm> globalFutexRealm;
}

FutexRealm *getGlobalFutexRealm() {
	return &globalFutexRealm.get();
}

} // namespace thor
