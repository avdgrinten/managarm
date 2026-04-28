#include <thor-internal/arch/ept.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/physical.hpp>

namespace thor::vmx {

constexpr uint64_t eptRead = UINT64_C(1) << 0;
constexpr uint64_t eptWrite = UINT64_C(1) << 1;
constexpr uint64_t eptExecute = UINT64_C(1) << 2;
constexpr uint64_t eptCacheWb = UINT64_C(6) << 3;
constexpr uint64_t eptIgnorePat = UINT64_C(1) << 6;
constexpr uint64_t eptDirty = UINT64_C(1) << 9;
// TODO: Support user-executable permissions (needs VM-x bit).
// constexpr uint64_t eptUserExecute = UINT64_C(1) << 10;
constexpr uint64_t eptAddress = 0x000F'FFFF'FFFF'F000;

struct EptCursorPolicy {
	static inline constexpr size_t maxLevels = 4;
	static inline constexpr size_t bitsPerLevel = 9;

	static constexpr size_t numLevels() { return 4; }

	static constexpr bool ptePagePresent(uint64_t pte) {
		return pte & eptRead;
	}

	static constexpr bool ptePageCanAccess(uint64_t pte, PageFlags flags) {
		if(flags & page_access::read && !(pte & eptRead))
			return false;

		if (flags & page_access::write && !(pte & eptWrite))
			return false;

		if (flags & page_access::execute && !(pte & eptExecute))
			return false;

		return true;
	}

	static constexpr PhysicalAddr ptePageAddress(uint64_t pte) {
		return pte & eptAddress;
	}

	static constexpr PageStatus ptePageStatus(uint64_t pte) {
		if(!ptePagePresent(pte))
			return 0;
		PageStatus status = page_status::present;
		if(pte & eptDirty)
			status |= page_status::dirty;
		return status;
	}

	static uint64_t pteClean(uint64_t *ptePtr) {
		return __atomic_fetch_and(ptePtr, ~eptDirty, __ATOMIC_RELAXED);
	}

	static constexpr uint64_t pteBuild(PhysicalAddr physical, PageFlags flags, CachingMode cachingMode) {
		auto pte = physical | eptIgnorePat | eptRead; // TODO: Do not always set eptRead.

		if(flags & page_access::write)
			pte |= eptWrite;
		if(flags & page_access::execute)
			pte |= eptExecute;
		assert(cachingMode == CachingMode::null || cachingMode == CachingMode::writeBack);
		pte |= eptCacheWb;

		return pte;
	}

	static std::pair<uint64_t, bool> pteAge(uint64_t *ptePtr, bool) {
		return {__atomic_load_n(ptePtr, __ATOMIC_RELAXED), false};
	}

	static constexpr void pteWriteBarrier() { }
	static constexpr void pteSyncICache(uintptr_t) { }


	static constexpr bool pteTablePresent(uint64_t pte) {
		return pte & eptRead;
	}

	static constexpr PhysicalAddr pteTableAddress(uint64_t pte) {
		return pte & eptAddress;
	}

	static uint64_t pteNewTable() {
		auto newPtAddr = physicalAllocator->allocate(kPageSize);
		assert(newPtAddr != PhysicalAddr(-1) && "OOM");

		PageAccessor accessor{newPtAddr};
		memset(accessor.get(), 0, kPageSize);

		return newPtAddr | eptRead | eptWrite | eptExecute;
	}
};

static_assert(CursorPolicy<EptCursorPolicy>);

using EptCursor = thor::PageCursor<EptCursorPolicy>;

EptPageSpace::EptPageSpace(PhysicalAddr root)
: PageSpace{root} { }

EptPageSpace::~EptPageSpace() {
	freePt<EptCursorPolicy, 3>(rootTable());
}

EptOperations::EptOperations(EptPageSpace *pageSpace)
: pageSpace_{pageSpace} { }

void EptOperations::retire(RetireNode *node) {
	EptPtr ptr = {pageSpace_->rootTable(), 0};
	asm volatile (
		"invept (%0), %1;"
		: : "r"(&ptr), "r"((uint64_t)1)
	);
	node->complete();
}

bool EptOperations::submitShootdown(ShootNode *node) {
	EptPtr ptr = {pageSpace_->rootTable(), 0};
	asm volatile (
		"invept (%0), %1;"
		: : "r"(&ptr), "r"((uint64_t)1)
	);
	node->complete();
	return false;
}

frg::expected<Error, PagesAffected> EptOperations::mapPresentPages(VirtualAddr va, MemoryView *view,
		uintptr_t offset, size_t size, PageFlags flags, CachingMode mode) {
	return mapPresentPagesByCursor<EptCursor>(pageSpace_,
			va, view, offset, size, flags, mode);
}

frg::expected<Error, PagesAffected> EptOperations::restrictPages(VirtualAddr va,
		size_t size, PageFlags flags, CachingMode mode) {
	return restrictPagesByCursor<EptCursor>(pageSpace_, va, size, flags, mode);
}

frg::expected<Error, PagesAffected> EptOperations::faultPage(VirtualAddr va, MemoryView *view,
		uintptr_t offset, FetchFlags fetchFlags, PageFlags flags, CachingMode mode) {
	return faultPageByCursor<EptCursor>(pageSpace_,
			va, view, offset, fetchFlags, flags, mode);
}

frg::expected<Error, PagesAffected> EptOperations::cleanPages(VirtualAddr va, size_t size) {
	return cleanPagesByCursor<EptCursor>(pageSpace_, va, size);
}

frg::expected<Error, PagesAffected> EptOperations::unmapPages(VirtualAddr va, size_t size) {
	return unmapPagesByCursor<EptCursor>(pageSpace_, va, size);
}

frg::expected<Error, PagesAffected> EptOperations::agePages(VirtualAddr, size_t, bool) {
	return PagesAffected{};
}

EptSpace::EptSpace(PhysicalAddr root, smarter::shared_ptr<Hierarchy> hierarchy)
: VirtualizedPageSpace{&eptOps_, std::move(hierarchy)}, eptOps_{&pageSpace_}, pageSpace_{root} { }

EptSpace::~EptSpace() { }

} // namespace thor
