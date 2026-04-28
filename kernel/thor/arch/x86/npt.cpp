#include <thor-internal/arch/npt.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/physical.hpp>

namespace thor::svm {

constexpr uint64_t nptPresent = UINT64_C(1) << 0;
constexpr uint64_t nptWrite = UINT64_C(1) << 1;
constexpr uint64_t nptUser = UINT64_C(1) << 2;
constexpr uint64_t nptDirty = UINT64_C(1) << 6;
constexpr uint64_t nptXd = UINT64_C(1) << 6;
constexpr uint64_t nptAddress = 0x000F'FFFF'FFFF'F000;

struct NptCursorPolicy {
	static inline constexpr size_t maxLevels = 4;
	static inline constexpr size_t bitsPerLevel = 9;

	static constexpr size_t numLevels() { return 4; }

	static constexpr bool ptePagePresent(uint64_t pte) {
		return pte & nptPresent;
	}

	static constexpr bool ptePageCanAccess(uint64_t pte, PageFlags flags) {
		if (!(pte & nptPresent))
			return false;

		if (flags & page_access::write && !(pte & nptWrite))
			return false;

		if (flags & page_access::execute && (pte & nptXd))
			return false;

		return true;
	}

	static constexpr PhysicalAddr ptePageAddress(uint64_t pte) {
		return pte & nptAddress;
	}

	static constexpr PageStatus ptePageStatus(uint64_t pte) {
		if(!ptePagePresent(pte))
			return 0;
		PageStatus status = page_status::present;
		if(pte & nptDirty)
			status |= page_status::dirty;
		return status;
	}

	static uint64_t pteClean(uint64_t *ptePtr) {
		return __atomic_fetch_and(ptePtr, ~nptDirty, __ATOMIC_RELAXED);
	}

	static constexpr uint64_t pteBuild(PhysicalAddr physical, PageFlags flags, CachingMode cachingMode) {
		auto pte = physical | nptPresent | nptUser;

		if(flags & page_access::write)
			pte |= nptWrite;
		if(!(flags & page_access::execute))
			pte |= nptXd;
		assert(cachingMode == CachingMode::null || cachingMode == CachingMode::writeBack);

		return pte;
	}

	static std::pair<uint64_t, bool> pteAge(uint64_t *ptePtr, bool) {
		return {__atomic_load_n(ptePtr, __ATOMIC_RELAXED), false};
	}

	static constexpr void pteWriteBarrier() { }
	static constexpr void pteSyncICache(uintptr_t) { }


	static constexpr bool pteTablePresent(uint64_t pte) {
		return pte & nptPresent;
	}

	static constexpr PhysicalAddr pteTableAddress(uint64_t pte) {
		return pte & nptAddress;
	}

	static uint64_t pteNewTable() {
		auto newPtAddr = physicalAllocator->allocate(kPageSize);
		assert(newPtAddr != PhysicalAddr(-1) && "OOM");

		PageAccessor accessor{newPtAddr};
		memset(accessor.get(), 0, kPageSize);

		return newPtAddr | nptPresent | nptUser | nptWrite;
	}
};

static_assert(CursorPolicy<NptCursorPolicy>);

using NptCursor = thor::PageCursor<NptCursorPolicy>;

NptPageSpace::NptPageSpace(PhysicalAddr root)
: PageSpace{root} { }

NptPageSpace::~NptPageSpace() {
	freePt<NptCursorPolicy, 3>(rootTable());
}

NptOperations::NptOperations(NptPageSpace *pageSpace)
: pageSpace_{pageSpace} { }

void NptOperations::retire(RetireNode *node) {
	// TODO: Shootdown needs to be implemented for NPT.
	(void)node;
}

bool NptOperations::submitShootdown(ShootNode *node) {
	// TODO: Shootdown needs to be implemented for NPT.
	(void)node;
	return false;
}

frg::expected<Error, PagesAffected> NptOperations::mapPresentPages(VirtualAddr va, MemoryView *view,
		uintptr_t offset, size_t size, PageFlags flags, CachingMode mode) {
	return mapPresentPagesByCursor<NptCursor>(pageSpace_,
			va, view, offset, size, flags, mode);
}

frg::expected<Error, PagesAffected> NptOperations::restrictPages(VirtualAddr va,
		size_t size, PageFlags flags, CachingMode mode) {
	return restrictPagesByCursor<NptCursor>(pageSpace_, va, size, flags, mode);
}

frg::expected<Error, PagesAffected> NptOperations::faultPage(VirtualAddr va, MemoryView *view,
		uintptr_t offset, FetchFlags fetchFlags, PageFlags flags, CachingMode mode) {
	return faultPageByCursor<NptCursor>(pageSpace_,
			va, view, offset, fetchFlags, flags, mode);
}

frg::expected<Error, PagesAffected> NptOperations::cleanPages(VirtualAddr va, size_t size) {
	return cleanPagesByCursor<NptCursor>(pageSpace_, va, size);
}

frg::expected<Error, PagesAffected> NptOperations::unmapPages(VirtualAddr va, size_t size) {
	return unmapPagesByCursor<NptCursor>(pageSpace_, va, size);
}

frg::expected<Error, PagesAffected> NptOperations::agePages(VirtualAddr, size_t, bool) {
	return PagesAffected{};
}

NptSpace::NptSpace(PhysicalAddr root, smarter::shared_ptr<Hierarchy> hierarchy)
: VirtualizedPageSpace{&nptOps_, std::move(hierarchy)}, nptOps_{&pageSpace_}, pageSpace_{root} { }

NptSpace::~NptSpace() { }

} // namespace thor::svm
