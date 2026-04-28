#pragma once

#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/virtualization.hpp>

constexpr uint64_t EPT_READ = (0);
constexpr uint64_t EPT_WRITE = (1);
constexpr uint64_t EPT_EXEC = (2);
constexpr uint64_t EPT_USEREXEC = (10);
constexpr uint64_t EPT_PHYSADDR = (12);
constexpr uint64_t EPT_IGNORE_PAT = (6);
constexpr uint64_t EPT_MEMORY_TYPE = (3);

namespace thor::vmx {

struct EptPtr {
	uint64_t eptp;
	uint64_t gpa;
};

struct EptPageSpace : PageSpace {
	EptPageSpace(PhysicalAddr root);

	EptPageSpace(const EptPageSpace &) = delete;

	~EptPageSpace();

	EptPageSpace &operator= (const EptPageSpace &) = delete;
};

struct EptOperations final : VirtualOperations {
	EptOperations(EptPageSpace *pageSpace);

	void retire(RetireNode *node) override;

	bool submitShootdown(ShootNode *node) override;

	frg::expected<Error, PagesAffected> mapPresentPages(VirtualAddr va, MemoryView *view,
			uintptr_t offset, size_t size, PageFlags flags, CachingMode mode) override;

	frg::expected<Error, PagesAffected> restrictPages(VirtualAddr va,
			size_t size, PageFlags flags, CachingMode mode) override;

	frg::expected<Error, PagesAffected> faultPage(VirtualAddr va, MemoryView *view,
			uintptr_t offset, FetchFlags fetchFlags, PageFlags flags, CachingMode mode) override;

	frg::expected<Error, PagesAffected> cleanPages(VirtualAddr va, size_t size) override;

	frg::expected<Error, PagesAffected> unmapPages(VirtualAddr va, size_t size) override;

	frg::expected<Error, PagesAffected> agePages(VirtualAddr va, size_t size, bool vacate) override;

private:
	EptPageSpace *pageSpace_;
};

struct EptSpace final : VirtualizedPageSpace {
	friend struct Vmcs;
	friend struct ShootNode;

	EptSpace(PhysicalAddr root, smarter::shared_ptr<Hierarchy> hierarchy);

	EptSpace(const EptSpace &) = delete;

	~EptSpace();

	EptSpace& operator=(const EptSpace &) = delete;

	static smarter::shared_ptr<EptSpace> create(PhysicalAddr root,
			smarter::shared_ptr<Hierarchy> hierarchy) {
		auto ptr = smarter::allocate_shared<EptSpace>(Allocator{}, root, std::move(hierarchy));
		ptr->selfPtr = ptr;
		ptr->setupInitialHole(0, 0x7ffffff00000);
		return ptr;
	}

	PhysicalAddr rootTable() {
		return pageSpace_.rootTable();
	}

private:
	EptOperations eptOps_;
	EptPageSpace pageSpace_;
	frg::ticket_spinlock _mutex;
};

} // namespace thor
