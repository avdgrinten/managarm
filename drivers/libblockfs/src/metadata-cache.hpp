#pragma once

#include <async/result.hpp>
#include <hel.h>
#include <helix/ipc.hpp>
#include <helix/memory.hpp>

#include <blockfs.hpp>

namespace blockfs {

// Mount-wide page cache for filesystem metadata blocks, indexed by disk block number.
// The cache offset of a block is a fixed function of its block number alone;
// hence, servicing a page reads no mutable filesystem state (such as block maps).
// Each block occupies its own page-aligned frame, i.e., blocks smaller than a page
// are never packed into the same page (to keep writeback of a page confined to a
// single block).
struct MetadataCache {
	// A locked and mapped view of a single metadata block.
	// The block stays present in the cache for the lifetime of this object.
	struct BlockWindow {
		BlockWindow() = default;

		void *get() {
			return mapping_.get();
		}

	private:
		friend struct MetadataCache;

		BlockWindow(helix::UniqueDescriptor lock, helix::Mapping mapping)
		: lock_{std::move(lock)}, mapping_{std::move(mapping)} { }

		helix::UniqueDescriptor lock_;
		helix::Mapping mapping_;
	};

	MetadataCache(BlockDevice *device, uint64_t numBlocks, size_t blockSize);

	// The servicer coroutine started by the constructor refers back to this object.
	MetadataCache(const MetadataCache &) = delete;
	MetadataCache &operator=(const MetadataCache &) = delete;

	// Locks and maps the given block.
	// Writes through a writable window are eventually written back to the block.
	async::result<BlockWindow> access(uint64_t block, bool writable);

	// Reads bytes from the given block through the cache without mapping it.
	async::result<void> read(uint64_t block, size_t offset, size_t length, void *buffer);

private:
	async::detached manage_(helix::UniqueDescriptor backing);
	async::detached serviceRequest_(helix::BorrowedDescriptor backing,
			int type, uintptr_t offset, size_t length);

	BlockDevice *device_;
	uint64_t numBlocks_;
	size_t blockSize_;
	uint32_t blockPagesShift_;
	size_t sectorsPerBlock_;
	helix::UniqueDescriptor frontal_;
};

} // namespace blockfs
