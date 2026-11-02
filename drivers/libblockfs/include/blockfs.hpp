#pragma once

#include <async/result.hpp>
#include <arch/dma_pool.hpp>
#include <protocols/fs/common.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/ostrace/ostrace.hpp>
#include <expected>
#include <stdint.h>

#include <memory>
#include <optional>

namespace blockfs {

enum class Error {
	// The device failed to complete the request or returned unusable data.
	ioError,
	// The device does not implement the request.
	notSupported,
	// The swap target is busy.
	busy,
};

inline protocols::fs::Error operator|(Error e, protocols::fs::ToFsProtoError) {
	switch(e) {
		case Error::ioError: return protocols::fs::Error::ioError;
		case Error::notSupported: return protocols::fs::Error::notSupported;
		case Error::busy: return protocols::fs::Error::busy;
	}
}

struct BlockDevice;
struct IoScheduler;

async::detached runDevice(BlockDevice *device);

struct BlockDevice {
	// Constraints of the issue{Read,Write}() hooks that the I/O scheduler enforces.
	struct Limits {
		// Largest request handed to a hook, in sectors. Absent if unlimited.
		std::optional<size_t> maxSectorsPerRequest;
		// Hardware queues that the hooks submit to.
		size_t numQueues = 1;
		// Capacity of each hardware queue, which the requests in flight in the hooks share.
		// Every request occupies issueCost() units of it, i.e., one by default.
		// Absent if unlimited, i.e., if the driver queues excess requests itself.
		std::optional<size_t> queueCapacity;
	};

	BlockDevice(size_t sector_size, int64_t parent_id, arch::contiguous_pool *pool,
			Limits limits);
	BlockDevice(size_t sector_size, int64_t parent_id, arch::contiguous_pool *pool);

	virtual ~BlockDevice();

	// Sector I/O passes through the device's I/O scheduler, which splits requests
	// according to limits and calls issue{Read,Write}() to perform them.
	// Only available after runDevice() attached the scheduler.
	virtual async::result<std::expected<void, Error>> readSectors(uint64_t sector, arch::dma_buffer_view view);
	virtual async::result<std::expected<void, Error>> writeSectors(uint64_t sector, arch::dma_buffer_view view);

	// Flushes the device's write cache. Bypasses the I/O scheduler.
	// flush() affects all write requests that have completed before flush() starts.
	// When flush() returns, all these write requests will have made it to durable storage.
	virtual async::result<std::expected<void, Error>> flush() {
		co_return {};
	}

	virtual async::result<size_t> getSize() = 0;

	virtual async::result<void> handleIoctl(managarm::fs::GenericIoctlRequest &req, helix::BorrowedDescriptor conversation) {
		std::cout << "\e[31m" "libblockfs: Unknown ioctl() message with ID "
				<< req.command() << "\e[39m" << std::endl;

		auto [dismiss] = co_await helix_ng::exchangeMsgs(
			conversation, helix_ng::dismiss());
		HEL_CHECK(dismiss.error());
	}

	size_t size;
	const size_t sectorSize;
	const size_t sectorShift;
	int64_t parentId = -1;

	std::string diskNamePrefix = "sd";
	std::string diskNameSuffix = "";
	std::string partNameSuffix = "";

	arch::contiguous_pool *pagePool;

	// Drivers that learn their limits after construction set them before calling runDevice().
	Limits limits;

protected:
	// Driver hooks. The I/O scheduler guarantees the constraints stated in Limits.
	// The first argument is the hardware queue (below limits.numQueues) that the request
	// was bound to when it was submitted.
	virtual async::result<std::expected<void, Error>> issueRead(unsigned int, uint64_t, arch::dma_buffer_view) {
		co_return std::unexpected{Error::notSupported};
	}

	virtual async::result<std::expected<void, Error>> issueWrite(unsigned int, uint64_t, arch::dma_buffer_view) {
		co_return std::unexpected{Error::notSupported};
	}

	// Runs once per submission, before the request is split and queued,
	// for work that issueCost() depends on.
	virtual async::result<void> prepareIo(unsigned int, arch::dma_buffer_view) {
		co_return;
	}

	// Returns the units of Limits::queueCapacity that a request occupies while it is in flight.
	// Must not be less than what the hook consumes, and an overestimate holds back
	// requests that the device could take, so this should be exact.
	virtual size_t issueCost(unsigned int, arch::dma_buffer_view) {
		return 1;
	}

	// Returns the hardware queue that requests of the calling thread are bound to.
	unsigned int pickQueue() const;

private:
	friend struct IoScheduler;
	friend async::detached runDevice(BlockDevice *device);

	std::unique_ptr<IoScheduler> scheduler_;
};

extern protocols::ostrace::Event ostEvtGetLink;
extern protocols::ostrace::Event ostEvtTraverseLinks;
extern protocols::ostrace::Event ostEvtRead;
extern protocols::ostrace::Event ostEvtRawRead;
extern protocols::ostrace::Event ostEvtRawWrite;
extern protocols::ostrace::Event ostEvtExt2AssignDataBlocks;
extern protocols::ostrace::Event ostEvtExt2AllocateBlocks;
extern protocols::ostrace::Event ostEvtExt2AllocateInode;
extern protocols::ostrace::Event ostEvtVirtioBlkReadSectors;
extern protocols::ostrace::Event ostEvtVirtioBlkWriteSectors;
extern protocols::ostrace::Event ostEvtVirtioBlkRequest;
extern protocols::ostrace::Event ostEvtVirtioBlkFlush;
extern protocols::ostrace::UintAttribute ostAttrTime;
extern protocols::ostrace::UintAttribute ostAttrNumBytes;
extern protocols::ostrace::UintAttribute ostAttrIsWrite;
extern protocols::ostrace::UintAttribute ostAttrQueueIndex;
extern protocols::ostrace::UintAttribute ostAttrTimeSetup;
extern protocols::ostrace::UintAttribute ostAttrTimeObtain;
extern protocols::ostrace::UintAttribute ostAttrTimeDevice;

extern protocols::ostrace::Context ostContext;

} // namespace blockfs
