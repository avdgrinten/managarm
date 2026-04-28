#pragma once

#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/virtualization.hpp>

namespace thor::svm {

struct NptPageSpace : PageSpace {
	NptPageSpace(PhysicalAddr root);

	NptPageSpace(const NptPageSpace &) = delete;

	~NptPageSpace();

	NptPageSpace &operator= (const NptPageSpace &) = delete;
};

struct NptOperations final : VirtualOperations {
	NptOperations(NptPageSpace *pageSpace);

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
	NptPageSpace *pageSpace_;
};

} // namespace thor::svm

namespace thor::svm {
	struct NptSpace final : VirtualizedPageSpace {
		friend struct ShootNode;
		friend struct Vcpu;

		NptSpace(PhysicalAddr root, smarter::shared_ptr<Hierarchy> hierarchy);

		NptSpace(const NptSpace &) = delete;

		~NptSpace();

		NptSpace& operator=(const NptSpace &) = delete;

		static smarter::shared_ptr<NptSpace> create(PhysicalAddr root,
				smarter::shared_ptr<Hierarchy> hierarchy) {
			auto ptr = smarter::allocate_shared<NptSpace>(Allocator{}, root, std::move(hierarchy));
			ptr->selfPtr = ptr;
			ptr->setupInitialHole(0, 0x7ffffff00000);
			return ptr;
		}

		PhysicalAddr rootTable() {
			return pageSpace_.rootTable();
		}

	private:
		NptOperations nptOps_;
		NptPageSpace pageSpace_;
		frg::ticket_spinlock _mutex;
	};
} // namespace thor::svm
