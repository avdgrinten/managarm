#pragma once

#include <async/result.hpp>
#include <arch/dma_pool.hpp>
#include <protocols/fs/common.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/ostrace/ostrace.hpp>
#include <expected>
#include <stdint.h>

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

struct BlockDevice {
	BlockDevice(size_t sector_size, int64_t parent_id, arch::contiguous_pool *pool);

	virtual ~BlockDevice() = default;

	virtual async::result<std::expected<void, Error>> readSectors(uint64_t, arch::dma_buffer_view) = 0;

	virtual async::result<std::expected<void, Error>> writeSectors(uint64_t, arch::dma_buffer_view) {
		co_return std::unexpected{Error::notSupported};
	}

	// Flushes the device's write cache.
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
protected:
};

async::detached runDevice(BlockDevice *device);

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
