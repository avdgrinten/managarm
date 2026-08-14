#pragma once

#include <async/result.hpp>
#include <arch/dma_pool.hpp>
#include <protocols/fs/common.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/ostrace/ostrace.hpp>
#include <stdint.h>

namespace blockfs {

struct BlockDevice {
	BlockDevice(size_t sector_size, int64_t parent_id, arch::contiguous_pool *pool);

	virtual ~BlockDevice() = default;

	virtual async::result<void> readSectors(uint64_t, arch::dma_buffer_view) = 0;

	virtual async::result<void> writeSectors(uint64_t, arch::dma_buffer_view) {
		throw std::runtime_error("BlockDevice does not support writeSectors()");
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
extern protocols::ostrace::Event ostEvtExt2AssignDataBlocks;
extern protocols::ostrace::Event ostEvtExt2InitiateInode;
extern protocols::ostrace::Event ostEvtExt2ManageInode;
extern protocols::ostrace::Event ostEvtExt2ManageInodeBitmap;
extern protocols::ostrace::Event ostEvtExt2InitializeFile;
extern protocols::ostrace::Event ostEvtExt2WritebackFile;
extern protocols::ostrace::Event ostEvtExt2WriteDataBlocks;
extern protocols::ostrace::Event ostEvtExt2ManageBlockBitmap;
extern protocols::ostrace::Event ostEvtExt2AllocateBlocks;
extern protocols::ostrace::Event ostEvtExt2AllocateInode;
extern protocols::ostrace::Event ostEvtVirtioBlkReadSectors;
extern protocols::ostrace::Event ostEvtVirtioBlkWriteSectors;
extern protocols::ostrace::Event ostEvtVirtioBlkRequest;
extern protocols::ostrace::UintAttribute ostAttrTime;
extern protocols::ostrace::UintAttribute ostAttrNumBytes;
extern protocols::ostrace::UintAttribute ostAttrIsWrite;
extern protocols::ostrace::UintAttribute ostAttrTimeImport;
extern protocols::ostrace::UintAttribute ostAttrTimeMapCheck;
extern protocols::ostrace::UintAttribute ostAttrTimeAssign;
extern protocols::ostrace::UintAttribute ostAttrTimeWrite;
extern protocols::ostrace::UintAttribute ostAttrTimeQueue;
extern protocols::ostrace::UintAttribute ostAttrTimeSetup;
extern protocols::ostrace::UintAttribute ostAttrTimeObtain;
extern protocols::ostrace::UintAttribute ostAttrTimeDevice;

extern protocols::ostrace::Context ostContext;

} // namespace blockfs
