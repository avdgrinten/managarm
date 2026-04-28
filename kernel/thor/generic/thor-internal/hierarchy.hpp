#pragma once

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

private:
	smarter::shared_ptr<Hierarchy> parent_;
	frg::string<KernelAlloc> tag_;
};

smarter::shared_ptr<Hierarchy> rootHierarchy();

} // namespace thor
