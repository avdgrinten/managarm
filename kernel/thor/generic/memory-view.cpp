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
	constexpr bool tortureUncaching = false;
	constexpr bool disableUncaching = false;
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
				if (checkPressure_()) {
					for(unsigned int i = 1; i <= CacheBundle::numGenerations; i++) {
						if(!checkPressure_())
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

	bool checkPressure_() {
		auto watermark = physicalAllocator->numTotalPages() * 3 / 4;
		return tortureUncaching || physicalAllocator->numUsedPages() >= watermark;
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

coroutine<frg::expected<Error>> MemoryView::invalidateRange(uintptr_t, size_t) {
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
	ZeroMemory() {
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
		auto memory = smarter::allocate_shared<ZeroMemory>(*kernelAlloc);
		memory->selfPtr = memory;
		return memory;
	}();
	return singleton.get();
}

// --------------------------------------------------------
// ImmediateMemory
// --------------------------------------------------------

std::expected<smarter::shared_ptr<ImmediateMemory>, Error>
ImmediateMemory::create(size_t length) {
	auto ptr = smarter::allocate_shared<ImmediateMemory>(*kernelAlloc);
	if(!ptr)
		return std::unexpected{Error::noMemory};
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

ImmediateMemory::ImmediateMemory()
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

HardwareMemory::HardwareMemory(PhysicalAddr base, size_t length, CachingMode cache_mode)
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

AllocatedMemory::AllocatedMemory(size_t desiredLngth,
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

ManagedSpace::ManagedSpace(size_t length, bool readahead)
: pages{*kernelAlloc}, numPages{length >> kPageShift}, readahead{readahead} {
	assert(!(length & (kPageSize - 1)));

	globalReclaimer->registerBundle(this);

	[] (ManagedSpace *self, enable_detached_coroutine) -> void {
		while(true) {
			// TODO: Cancel awaitReclaim() when the ManagedSpace is destructed.
			co_await globalReclaimer->awaitReclaim(self);

			CachePagesList batch;
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&self->mutex);

				globalReclaimer->reclaimPages(self, batch);

				for(auto cachePage : batch) {
					auto *page = frg::container_of(cachePage, &ManagedPage::cachePage);
					assert(page);
					assert(page->loadState == LoadState::present);
					assert(page->transactionState == TxState::inReclaimer);
					assert(!page->lockCount);
					page->transactionState = TxState::performReclaim;
					globalReclaimer->removePage(cachePage);
				}
			}

			if(batch.empty())
				continue;

			co_await self->_evictQueue.fenceEphemeral();

			bool anyDirty = false;
			size_t sizeFreed = 0;
			while(!batch.empty()) {
				PhysicalAddr physical;
				frg::intrusive_shared_ptr<TransactionMonitor, Allocator> invalidateMonitor;
				{
					auto irqLock = frg::guard(&irqMutex());
					auto lock = frg::guard(&self->mutex);

					auto cachePage = batch.pop_front();
					auto *page = frg::container_of(cachePage, &ManagedPage::cachePage);

					if(page->transactionState == TxState::avertReclaim) {
						if(page->stillDirty) {
							page->stillDirty = false;
							page->transactionState = TxState::dirty;
							self->_dirtyList.push_back(&page->cachePage);
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

					if(page->forceInvalidation) {
						invalidateMonitor = std::move(page->monitor);
						page->forceInvalidation = false;
					}
				}

				globalPfnDb().erase(physical);
				physicalAllocator->free(physical, kPageSize);
				if(invalidateMonitor)
					invalidateMonitor->event.raise();
				sizeFreed += kPageSize;
			}

			if(anyDirty)
				self->_dirtyEvent.raise();

			if(logUncaching)
				infoLogger() << frg::fmt(
					"thor: Reclamation freed 0x{:x} bytes",
					sizeFreed
				)<< frg::endlog;
		}
	}(this, enable_detached_coroutine{WorkQueue::generalQueue().lock()});

	[] (ManagedSpace *self, enable_detached_coroutine) -> void {
		while(true) {
			co_await self->_dirtyEvent.async_wait_if([self] () -> bool {
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&self->mutex);
				return self->_dirtyList.empty();
			});

			frg::intrusive_list<
				CachePage,
				frg::locate_member<CachePage, frg::default_list_hook<CachePage>, &CachePage::listHook>
			> pending;
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&self->mutex);
				pending.splice(pending.end(), self->_dirtyList);
			}
			if (pending.empty())
				continue;

			co_await self->_evictQueue.fenceDirty();

			ManageList mgmtPending;
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&self->mutex);

				while(!pending.empty()) {
					auto *cp = pending.pop_front();
					auto *page = frg::container_of(cp, &ManagedPage::cachePage);
					// A concurrent resize() may already have evicted a truncated page
					// that was still in our local pending list; skip anything that is
					// no longer dirty, and discard (rather than write back) pages that
					// have since fallen beyond numPages.
					if(page->transactionState != TxState::dirty)
						continue;
					if(cp->identity >= self->numPages) {
						page->transactionState = TxState::none;
						continue;
					}
					page->transactionState = TxState::wantWriteback;
					page->monitor = frg::allocate_intrusive_shared<TransactionMonitor>(Allocator{});
					self->_writebackList.push_back(cp);
				}

				self->_progressManagement(mgmtPending);
			}

			while(!mgmtPending.empty()) {
				auto node = mgmtPending.pop_front();
				node->completionEvent.raise();
			}
		}
	}(this, enable_detached_coroutine{WorkQueue::generalQueue().lock()});
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
	if((offset + size) / kPageSize > numPages)
		return Error::bufferTooSmall;

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
			}
		}
	}
	return Error::success;
}

// Note: Neither offset nor size are necessarily multiples of the page size.
// The range may extend beyond numPages: a concurrent resize() can shrink the
// object while an in-flight DMA import still holds a lock on a truncated page.
void ManagedSpace::unlockPages(uintptr_t offset, size_t size) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&mutex);

	for(size_t pg = 0; pg < size; pg += kPageSize) {
		size_t index = (offset + pg) / kPageSize;
		auto pit = pages.find(index);
		assert(pit);
		assert(pit->lockCount > 0);
		pit->lockCount--;
		if(!pit->lockCount) {
			if(pit->loadState == LoadState::present && pit->transactionState == TxState::none
					&& !pit->cachePage.useCount.load(std::memory_order_relaxed)) {
				globalReclaimer->addPage(&pit->cachePage);
				pit->transactionState = TxState::inReclaimer;
			}
		}
	}
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
		}
	}
}

void ManagedSpace::decrementUses(CachePage *cachePage) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&mutex);

	auto page = frg::container_of(cachePage, &ManagedPage::cachePage);

	auto cnt = cachePage->useCount.fetch_sub(1, std::memory_order_release);
	assert(cnt > 0);
	if(cnt == 1) {
		if(page->loadState == LoadState::present
				&& page->transactionState == TxState::none
				&& !page->lockCount) {
			globalReclaimer->addPage(cachePage);
			page->transactionState = TxState::inReclaimer;
		}
	}
}

void ManagedSpace::markDirty(CachePage *cachePage) {
	bool needsEvent = false;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex);

		auto page = frg::container_of(cachePage, &ManagedPage::cachePage);

		if(page->loadState == LoadState::missing)
			return;

		// A page beyond the current size is being discarded by a concurrent
		// resize(); do not enter it into the writeback pipeline.
		if(cachePage->identity >= numPages)
			return;

		if(page->loadState == LoadState::present
				&& (page->transactionState == TxState::none
					|| page->transactionState == TxState::inReclaimer)) {
			if(page->transactionState == TxState::inReclaimer)
				globalReclaimer->removePage(cachePage);
			page->transactionState = TxState::dirty;
			_dirtyList.push_back(cachePage);
			needsEvent = true;
		} else if(page->transactionState == TxState::performReclaim
				|| page->transactionState == TxState::avertReclaim) {
			page->transactionState = TxState::avertReclaim;
			page->stillDirty = true;
		} else if(page->transactionState == TxState::writeback) {
			page->stillDirty = true;
		} else {
			assert(page->transactionState == TxState::dirty
					|| page->transactionState == TxState::wantWriteback);
		}
	}

	if(needsEvent)
		_dirtyEvent.raise();
}

// --------------------------------------------------------
// BackingMemory
// --------------------------------------------------------

coroutine<frg::expected<Error>> BackingMemory::resize(size_t newSize) {
	assert(currentIpl() == ipl::exceptionalWork);
	assert(!(newSize & (kPageSize - 1)));
	auto newPages = newSize >> kPageShift;

	size_t oldPages;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_managed->mutex);

		oldPages = _managed->numPages;
		_managed->numPages = newPages;
	}

	if(newPages > oldPages) {
		// Do nothing for now.
	}else if(newPages < oldPages) {
		// TODO: also free the affected pages!
		co_await _managed->_evictQueue.breakRange(newPages << kPageShift,
				(oldPages - newPages) << kPageShift);
	}

	co_return {};
}

Error BackingMemory::lockRange(uintptr_t offset, size_t size) {
	return _managed->lockPages(offset, size);
}

void BackingMemory::unlockRange(uintptr_t offset, size_t size) {
	_managed->unlockPages(offset, size);
}

PhysicalRange BackingMemory::peekRange(uintptr_t offset, FetchFlags) {
	auto index = offset >> kPageShift;
	auto misalign = offset & (kPageSize - 1);

	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_managed->mutex);

	if(index >= _managed->numPages)
		return PhysicalRange{};
	auto pit = _managed->pages.find(index);
	if(!pit)
		return PhysicalRange{};

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

	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_managed->mutex);

	if(index >= _managed->numPages)
		co_return Error::fault;
	auto [pit, wasInserted] = _managed->pages.find_or_insert(index, _managed.get(), index);
	assert(pit);

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
	// Size is constant so we do not need to lock.
	return _managed->numPages << kPageShift;
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
	assert((offset % kPageSize) == 0);
	assert((length % kPageSize) == 0);

	ManagedSpace::MonitorPendingList pendingMonitors;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_managed->mutex);

		// Note: the range is validated against each page's transaction state, not
		// against numPages. A page dispatched before a concurrent resize() shrank
		// the object is a legitimate in-flight transaction and must be retired
		// here even though its index is now beyond numPages; resize() waits for
		// exactly this retirement (and the subsequent unlock) before freeing it.

	/*	assert(length == kPageSize);
		auto inspect = (unsigned char *)physicalToVirtual(_managed->physicalPages[offset / kPageSize]);
		auto log = infoLogger() << "dump";
		for(size_t b = 0; b < kPageSize; b += 16) {
			log << frg::hex_fmt(offset + b) << "   ";
			for(size_t i = 0; i < 16; i++)
				log << " " << frg::hex_fmt(inspect[b + i]);
			log << "\n";
		}
		log << frg::endlog;*/

		if(type == ManageRequest::initialize) {
			for(size_t pg = 0; pg < length; pg += kPageSize) {
				size_t index = (offset + pg) / kPageSize;
				auto pit = _managed->pages.find(index);
				assert(pit);
				assert(pit->transactionState == ManagedSpace::TxState::initialization);
				pit->loadState = ManagedSpace::LoadState::present;
				if (pit->lockCount || pit->cachePage.useCount.load(std::memory_order_relaxed)) {
					pit->transactionState = ManagedSpace::TxState::none;
				} else {
					globalReclaimer->addPage(&pit->cachePage);
					pit->transactionState = ManagedSpace::TxState::inReclaimer;
				}
				ref_rc(pit->monitor.get());
				pendingMonitors.push_back(pit->monitor.get());
				pit->monitor = {};
			}
		}else{
			for(size_t pg = 0; pg < length; pg += kPageSize) {
				size_t index = (offset + pg) / kPageSize;
				auto pit = _managed->pages.find(index);
				assert(pit);

				assert(pit->transactionState == ManagedSpace::TxState::writeback);
				// A page re-dirtied while its writeback was in flight is normally
				// re-queued, but not if a concurrent resize() has truncated it: its
				// contents are being discarded, so retire it clean instead.
				if(!pit->stillDirty || index >= _managed->numPages) {
					pit->stillDirty = false;
					if (pit->lockCount || pit->cachePage.useCount.load(std::memory_order_relaxed)) {
						pit->transactionState = ManagedSpace::TxState::none;
					} else {
						globalReclaimer->addPage(&pit->cachePage);
						pit->transactionState = ManagedSpace::TxState::inReclaimer;
					}
					ref_rc(pit->monitor.get());
					pendingMonitors.push_back(pit->monitor.get());
					pit->monitor = {};
				}else{
					pit->stillDirty = false;
					pit->transactionState = ManagedSpace::TxState::wantWriteback;
					_managed->_writebackList.push_back(&pit->cachePage);
					ref_rc(pit->monitor.get());
					pendingMonitors.push_back(pit->monitor.get());
					// Note that the monitor is destroyed and re-created here.
					pit->monitor = frg::allocate_intrusive_shared<ManagedSpace::TransactionMonitor>(Allocator{});
				}
			}
		}
	}

	while(!pendingMonitors.empty()) {
		frg::intrusive_shared_ptr<ManagedSpace::TransactionMonitor, Allocator> monitor{
			frg::adopt_rc, pendingMonitors.pop_front()
		};
		monitor->event.raise();
	}

	return Error::success;
}

coroutine<frg::expected<Error>> BackingMemory::writebackFence(uintptr_t offset, size_t size) {
	if (offset & (kPageSize - 1))
		co_return Error::illegalArgs;
	if (size & (kPageSize - 1))
		co_return Error::illegalArgs;

	size_t pg = 0;
	while(pg < size) {
		frg::intrusive_shared_ptr<ManagedSpace::TransactionMonitor, Allocator> monitor;
		bool needSecond = false;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&_managed->mutex);

			while(pg < size) {
				size_t index = (offset + pg) >> kPageShift;
				auto pit = _managed->pages.find(index);
				if(pit) {
					if(pit->transactionState == ManagedSpace::TxState::wantWriteback) {
						monitor = pit->monitor;
						break;
					} else if(pit->transactionState == ManagedSpace::TxState::writeback) {
						monitor = pit->monitor;
						needSecond = true;
						break;
					}
				}
				pg += kPageSize;
			}
		}

		if(!monitor)
			break;

		co_await monitor->event.wait();

		// If the writeback was already in progress, it is not guaranteed that it did write
		// back the latest state before the writebackFence().
		// In this case, we may need to wait for another writeback.
		if(needSecond) {
			monitor = {};
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&_managed->mutex);

				size_t index = (offset + pg) >> kPageShift;
				auto pit = _managed->pages.find(index);
				if(pit && pit->monitor)
					monitor = pit->monitor;
			}

			if(monitor)
				co_await monitor->event.wait();
		}

		pg += kPageSize;
	}

	co_return {};
}

coroutine<frg::expected<Error>> BackingMemory::invalidateRange(uintptr_t offset, size_t size) {
	if (offset & (kPageSize - 1))
		co_return Error::illegalArgs;
	if (size & (kPageSize - 1))
		co_return Error::illegalArgs;

	size_t pg = 0;
	while(pg < size) {
		size_t index = (offset + pg) >> kPageShift;
		frg::intrusive_shared_ptr<ManagedSpace::TransactionMonitor, Allocator> monitor;
		bool shouldEvict = false;
		ManagedSpace::ManagedPage *pit = nullptr;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&_managed->mutex);

			pit = _managed->pages.find(index);
			if(!pit || pit->loadState == ManagedSpace::LoadState::missing) {
				pg += kPageSize;
				continue;
			}

			if(pit->loadState == ManagedSpace::LoadState::present) {
				if(pit->transactionState == ManagedSpace::TxState::performReclaim
						|| pit->transactionState == ManagedSpace::TxState::avertReclaim) {
					pit->forceInvalidation = true;
					if(!pit->monitor)
						pit->monitor = frg::allocate_intrusive_shared<ManagedSpace::TransactionMonitor>(Allocator{});
					monitor = pit->monitor;
				} else if(pit->monitor) {
					monitor = pit->monitor;
				} else if(!pit->lockCount) {
					if(pit->transactionState == ManagedSpace::TxState::inReclaimer)
						globalReclaimer->removePage(&pit->cachePage);
					pit->transactionState = ManagedSpace::TxState::performReclaim;
					shouldEvict = true;
				} else {
					co_return Error::illegalArgs;
				}
			}
		}

		if(shouldEvict) {
			co_await _managed->_evictQueue.breakRange(index << kPageShift, kPageSize);
			PhysicalAddr physical;
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&_managed->mutex);

				if(pit->transactionState == ManagedSpace::TxState::performReclaim) {
					physical = pit->physical;
					pit->loadState = ManagedSpace::LoadState::missing;
					pit->transactionState = ManagedSpace::TxState::none;
					pit->physical = PhysicalAddr(-1);
				} else {
					physical = PhysicalAddr(-1);
				}
			}
			if(physical != PhysicalAddr(-1)) {
				globalPfnDb().erase(physical);
				physicalAllocator->free(physical, kPageSize);
			}
			if(physical != PhysicalAddr(-1))
				pg += kPageSize;
			continue;
		}

		if(monitor)
			co_await monitor->event.wait();
		else
			pg += kPageSize;
	}

	co_return {};
}

// --------------------------------------------------------
// FrontalMemory
// --------------------------------------------------------

Error FrontalMemory::lockRange(uintptr_t offset, size_t size) {
	return _managed->lockPages(offset, size);
}

void FrontalMemory::unlockRange(uintptr_t offset, size_t size) {
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
			pit->monitor = frg::allocate_intrusive_shared<ManagedSpace::TransactionMonitor>(Allocator{});
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
					pit->monitor = frg::allocate_intrusive_shared<ManagedSpace::TransactionMonitor>(Allocator{});
				}
			}

		_managed->_progressManagement(pendingManagement);

		fetchMonitor = pit->monitor;
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
// IndirectMemory
// --------------------------------------------------------

IndirectMemory::IndirectMemory(size_t numSlots)
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

CopyOnWriteMemory::CopyOnWriteMemory(smarter::shared_ptr<MemoryView> view,
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
		forked = smarter::allocate_shared<CopyOnWriteMemory>(*kernelAlloc,
						_view, _viewOffset, _length, newChain);
		forked->selfPtr = forked;

		// Inspect all copied pages owned by the original mapping.
		for(size_t pg = 0; pg < _length; pg += kPageSize) {
			auto it = _ownedPages.find(pg >> kPageShift);

			if(!it) {
				// If the page is missing in this memory object, look at the CowChain.
				auto pageOffset = _viewOffset + pg;
				if (curChain) {
					auto chainLock = frg::guard(&curChain->_mutex);

					if(auto it = curChain->_pages.find(pageOffset >> kPageShift); it) {
						auto page = *it;
						assert(page->state == CowState::hasCopy);
						auto newIt = newChain->_pages.insert(pageOffset >> kPageShift);
						*newIt = page;
					}
				}
				continue;
			}

			auto page = *it;
			if(page->state == CowState::null) {
				continue;
			}else if(page->state == CowState::inProgress) {
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
