#pragma once

#include <atomic>
#include <expected>
#include <frg/list.hpp>
#include <frg/spinlock.hpp>
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
	~Hierarchy();

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

	// Hook that links this node into its parent's list of children.
	frg::intrusive_rcu_list_hook<Hierarchy> siblingHook_;

	// Protects children_. Never acquired from IRQ context.
	frg::ticket_spinlock childrenMutex_;
	frg::intrusive_rcu_list<
		Hierarchy,
		frg::locate_member<
			Hierarchy,
			frg::intrusive_rcu_list_hook<Hierarchy>,
			&Hierarchy::siblingHook_
		>
	> children_;

private:
	smarter::shared_ptr<Hierarchy> parent_;
	frg::string<KernelAlloc> tag_;
	std::atomic<size_t> chargedBytes_{0};
};

smarter::shared_ptr<Hierarchy> rootHierarchy();

} // namespace thor
