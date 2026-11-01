
#include <atomic>
#include <hel.h>
#include <async/mutex.hpp>
#include <frg/mutex.hpp>
#include <helix/ipc.hpp>
#include <helix/memory.hpp>
#include <protocols/fs/file-locks.hpp>
#include <blockfs.hpp>

namespace blockfs {
namespace raw {

using FlockManager = protocols::fs::FlockManager;
using Flock = protocols::fs::Flock;

struct RawFs {
	RawFs(BlockDevice *device, RawFs *disk);

	async::result<void> init();

	async::result<void> manageMapping();

	// Writes back the page cache and flushes the device's write cache.
	async::result<std::expected<void, Error>> synchronize();

	// Writes back and drops all cached pages that overlap the given byte range.
	async::result<void> invalidate(uint64_t offset, uint64_t size);

	BlockDevice *device;
	// The RawFs of the whole disk for partitions, nullptr for the whole disk itself.
	RawFs *disk;
	HelHandle backingMemory;
	HelHandle frontalMemory;
	helix::Mapping fileMapping;
	FlockManager flockManager;
};

struct OpenFile {
	OpenFile(RawFs *rawFs, bool nonBlock);

	struct HandleIoctl;

	RawFs *rawFs;
	async::mutex offsetMutex;
	// Protected by offsetMutex.
	uint64_t offset = 0;
	Flock flock;
	// Block devices never block, but the flag must survive F_GETFL/F_SETFL.
	std::atomic<bool> nonBlock;
};

extern protocols::fs::FileOperations rawOperations;

} // namespace raw
} // namespace blockfs
