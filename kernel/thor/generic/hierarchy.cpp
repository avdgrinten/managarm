#include <frg/manual_box.hpp>

#include <thor-internal/debug.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/hierarchy.hpp>
#include <thor-internal/ipl.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/timer.hpp>

namespace thor {

Hierarchy::Hierarchy(smarter::shared_ptr<Hierarchy> parent,
		frg::string<KernelAlloc> tag)
: parent_{std::move(parent)}, tag_{std::move(tag)} {
	if(parent_) {
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&parent_->childrenMutex_);
		parent_->children_.push_back(this);
	}
}

Hierarchy::~Hierarchy() {
	if(parent_) {
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&parent_->childrenMutex_);
		parent_->children_.erase(this);
	}
}

std::expected<smarter::shared_ptr<Hierarchy>, Error> Hierarchy::createRoot() {
	auto ptr = smarter::allocate_shared<Hierarchy>(*kernelAlloc,
			smarter::shared_ptr<Hierarchy>{},
			frg::string<KernelAlloc>{*kernelAlloc, "root"});
	if(!ptr)
		return std::unexpected{Error::noMemory};
	return ptr;
}

std::expected<smarter::shared_ptr<Hierarchy>, Error> Hierarchy::extend(
		smarter::shared_ptr<Hierarchy> parent,
		frg::string<KernelAlloc> tag) {
	if(!parent)
		return std::unexpected{Error::illegalArgs};
	auto ptr = smarter::allocate_shared<Hierarchy>(*kernelAlloc,
			std::move(parent), std::move(tag));
	if(!ptr)
		return std::unexpected{Error::noMemory};
	return ptr;
}

namespace {
	frg::manual_box<smarter::shared_ptr<Hierarchy>> rootHierarchy_;
}

smarter::shared_ptr<Hierarchy> rootHierarchy() {
	if(!rootHierarchy_) {
		auto created = Hierarchy::createRoot();
		if(!created)
			panicLogger() << "thor: Failed to allocate root hierarchy"
					<< frg::endlog;
		rootHierarchy_.initialize(std::move(*created));
	}
	return *rootHierarchy_;
}

namespace {

// Interval between successive memory-usage reports.
constexpr uint64_t hierarchyLogInterval = 1'000'000'000; // 1 s in ns.
// Number of top consumers to report.
constexpr int hierarchyLogTopK = 16;

struct TopConsumers {
	struct Entry {
		// Holds the full tag (userspace may pass up to 128 bytes) plus a null terminator.
		char tag[129];
		size_t bytes;
	};

	// Inserts a node into the descending-by-bytes top list, dropping the smallest
	// once the list is full.
	void consider(frg::string_view tag, size_t bytes) {
		if(count == hierarchyLogTopK && bytes <= entries[hierarchyLogTopK - 1].bytes)
			return;
		if(count < hierarchyLogTopK)
			++count;
		int i = count - 1;
		while(i > 0 && entries[i - 1].bytes < bytes) {
			entries[i] = entries[i - 1];
			--i;
		}
		entries[i].bytes = bytes;
		size_t n = tag.size();
		if(n > sizeof(entries[i].tag) - 1)
			n = sizeof(entries[i].tag) - 1;
		memcpy(entries[i].tag, tag.data(), n);
		entries[i].tag[n] = 0;
	}

	Entry entries[hierarchyLogTopK];
	int count = 0;
};

// Walks the subtree rooted at node, accounting each node's charged memory.
// Holds childrenMutex_ along the current path so that children cannot be
// unlinked while they are inspected.
void collectConsumers(Hierarchy *node, TopConsumers &top) {
	top.consider(node->tag(), node->chargedBytes());

	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&node->childrenMutex_);
	for(auto child : node->children_)
		collectConsumers(child, top);
}

} // anonymous namespace

void dumpHierarchyMemoryUsage() {
	// Avoid reentrancy (this may be called from the allocation-failure path) and
	// concurrent dumps from other CPUs.
	static std::atomic<bool> inProgress{false};
	if(inProgress.exchange(true, std::memory_order_acq_rel))
		return;

	// Do not force the root hierarchy into existence from here.
	if(rootHierarchy_) {
		auto root = *rootHierarchy_;
		TopConsumers top;
		collectConsumers(root.get(), top);

		infoLogger() << "thor: Hierarchy memory usage (top consumers):" << frg::endlog;
		for(int i = 0; i < top.count; ++i) {
			if(!top.entries[i].bytes)
				break;
			infoLogger() << "thor:     " << top.entries[i].tag << ": "
					<< (top.entries[i].bytes / 1024) << " KiB" << frg::endlog;
		}
	}

	inProgress.store(false, std::memory_order_release);
}

namespace {

void runHierarchyMonitor() {
	KernelFiber::run([] {
		while(true) {
			KernelFiber::asyncBlockCurrent(
					generalTimerEngine()->sleepFor(hierarchyLogInterval));
			dumpHierarchyMemoryUsage();
		}
	});
}

} // anonymous namespace

static initgraph::Task initHierarchyMonitor{&globalInitEngine, "generic.init-hierarchy-monitor",
	initgraph::Requires{getFibersAvailableStage()},
	[] {
		runHierarchyMonitor();
	}
};

} // namespace thor
