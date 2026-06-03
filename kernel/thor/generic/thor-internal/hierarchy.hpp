#pragma once

#include <atomic>
#include <expected>
#include <frg/string.hpp>
#include <smarter.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/kernel-heap.hpp>

namespace thor {

struct Hierarchy {
	static std::expected<smarter::shared_ptr<Hierarchy>, Error> createRoot();
	static std::expected<smarter::shared_ptr<Hierarchy>, Error> extend(
			smarter::shared_ptr<Hierarchy> parent,
			frg::string<KernelAlloc> tag);

	Hierarchy(smarter::shared_ptr<Hierarchy> parent,
			frg::string<KernelAlloc> tag);

	smarter::borrowed_ptr<Hierarchy> parent() const { return parent_; }
	frg::string_view tag() const { return tag_; }

	// Tracks the physical memory (in bytes) currently charged to this node.
	void chargeMemory(size_t bytes) {
		chargedBytes_.fetch_add(bytes, std::memory_order_relaxed);
	}
	void unchargeMemory(size_t bytes) {
		chargedBytes_.fetch_sub(bytes, std::memory_order_relaxed);
	}
	size_t chargedBytes() const {
		return chargedBytes_.load(std::memory_order_relaxed);
	}

private:
	smarter::shared_ptr<Hierarchy> parent_;
	frg::string<KernelAlloc> tag_;
	std::atomic<size_t> chargedBytes_{0};
};

smarter::shared_ptr<Hierarchy> rootHierarchy();

} // namespace thor
