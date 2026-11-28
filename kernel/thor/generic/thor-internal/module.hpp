#pragma once

#include <atomic>
#include <frg/string.hpp>
#include <frg/vector.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/kernel-heap.hpp>

namespace thor {

enum class MfsType {
	null,
	directory,
	regular
};

struct MfsNode {
	MfsNode(MfsType type)
	: type{type}, inodeNumber{nextInodeNumber_.fetch_add(1, std::memory_order_relaxed)} { }

	const MfsType type;
	// Lets clients such as ld.so tell files apart by st_ino.
	const uint64_t inodeNumber;

private:
	static inline std::atomic<uint64_t> nextInodeNumber_{1};
};

struct MfsDirectory : MfsNode {
	struct Link {
		frg::string<KernelAlloc> name;
		MfsNode *node;
	};

	MfsDirectory()
	: MfsNode{MfsType::directory}, _entries{*kernelAlloc} { }

	void link(frg::string<KernelAlloc> name, MfsNode *node) {
		assert(!getTarget(name));
		_entries.push(Link{std::move(name), node});
	}

	size_t numEntries() {
		return _entries.size();
	}

	Link getEntry(size_t i) {
		return _entries[i];
	}

	MfsNode *getTarget(frg::string_view name) {
		for(size_t i = 0; i < _entries.size(); i++) {
			if(_entries[i].name == name)
				return _entries[i].node;
		}
		return nullptr;
	}

private:
	frg::vector<Link, KernelAlloc> _entries;
};

struct MfsRegular : MfsNode {
	MfsRegular(smarter::shared_ptr<MemoryView> memory, size_t size)
	: MfsNode{MfsType::regular}, _memory{std::move(memory)}, _size{size} {
		assert(_size <= _memory->getLength());
	}

	smarter::shared_ptr<MemoryView> getMemory() {
		return _memory;
	}

	size_t size() {
		return _size;
	}

private:
	smarter::shared_ptr<MemoryView> _memory;
	size_t _size;
};

extern MfsDirectory *mfsRoot;

MfsNode *resolveModule(frg::string_view path);

} // namespace thor
