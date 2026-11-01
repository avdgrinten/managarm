#include "raw.hpp"

#include <fcntl.h>
#include <iostream>
#include <linux/cdrom.h>
#include <print>

#include <bragi/helpers-std.hpp>
#include <core/dispatch.hpp>
#include <core/logging.hpp>
#include <helix/clock.hpp>
#include <helix/dispatcher-pool.hpp>
#include "fs.bragi.hpp"
#include "fs.hpp"

namespace blockfs {
namespace raw {

RawFs::RawFs(BlockDevice *device, RawFs *disk)
: device{device}, disk{disk} { }

async::result<void> RawFs::init() {
	auto device_size = co_await device->getSize();
	auto cache_size = (device_size + 0xFFF) & ~size_t(0xFFF);
	HEL_CHECK(helCreateManagedMemory(fileDataHierarchy(), cache_size, 0,
				&backingMemory, &frontalMemory));

	helix::DispatcherPool::global().detach(manageMapping());
	co_return;
}

async::result<void> RawFs::manageMapping() {
	while(true) {
		helix::ManageMemory manage;
		auto &&submit = helix::submitManageMemory(helix::BorrowedDescriptor{backingMemory},
				&manage, helix::Dispatcher::global());
		co_await submit.async_wait();
		HEL_CHECK(manage.error());

		auto device_size = co_await device->getSize();
		auto cache_size = (device_size + 0xFFF) & ~size_t(0xFFF);
		assert(manage.offset() + manage.length() <= cache_size);

		// The manager's own writes into the backing view must not schedule a writeback.
		auto view = device->pagePool->realm()->importMemory(
		    helix::BorrowedDescriptor{backingMemory}, manage.offset(), manage.length(),
		    kHelMapNoDirtyTracking
		);

		if(manage.type() == kHelManageInitialize) {
			assert(!(manage.offset() & (device->sectorSize - 1)));

			size_t backed_size = std::min(manage.length(), device_size - manage.offset());
			size_t num_blocks = (backed_size + device->sectorSize - 1) / device->sectorSize;

			assert(num_blocks * device->sectorSize <= manage.length());
			auto readRes = co_await device->readSectors(manage.offset() / device->sectorSize,
					view.view().subview(0, num_blocks * device->sectorSize));
			if(!readRes)
				logPanic("libblockfs: Failed to read raw device at offset {:#x}", manage.offset());

			HEL_CHECK(helUpdateMemory(backingMemory, kHelManageInitialize,
						manage.offset(), manage.length()));
		} else {
			assert(manage.type() == kHelManageWriteback);
			assert(!(manage.offset() & (device->sectorSize - 1)));

			size_t backed_size = std::min(manage.length(), device_size - manage.offset());
			size_t num_blocks = (backed_size + device->sectorSize - 1) / device->sectorSize;

			assert(num_blocks * device->sectorSize <= manage.length());
			auto writeRes = co_await device->writeSectors(manage.offset() / device->sectorSize,
					view.view().subview(0, num_blocks * device->sectorSize));
			if(!writeRes)
				logPanic("libblockfs: Failed to write raw device at offset {:#x}", manage.offset());

			HEL_CHECK(helUpdateMemory(backingMemory, kHelManageWriteback,
						manage.offset(), manage.length()));
		}
	}
}

async::result<std::expected<void, Error>> RawFs::synchronize() {
	auto device_size = co_await device->getSize();
	auto cache_size = (device_size + 0xFFF) & ~size_t(0xFFF);
	auto writeback = co_await helix_ng::writebackFence(
			helix::BorrowedDescriptor{backingMemory}, 0, cache_size);
	HEL_CHECK(writeback.error());

	co_return co_await device->flush();
}

async::result<void> RawFs::invalidate(uint64_t offset, uint64_t size) {
	auto device_size = co_await device->getSize();
	auto cache_size = (device_size + 0xFFF) & ~size_t(0xFFF);

	// A corrupt partition table can place a partition beyond the end of the disk.
	auto first = std::min(offset & ~uint64_t(0xFFF), uint64_t{cache_size});
	auto limit = std::min((offset + size + 0xFFF) & ~uint64_t(0xFFF), uint64_t{cache_size});

	auto invalidate = co_await helix_ng::invalidateMemory(
			helix::BorrowedDescriptor{backingMemory}, first, limit - first);
	HEL_CHECK(invalidate.error());
}

OpenFile::OpenFile(RawFs *rawFs, bool nonBlock)
: rawFs(rawFs), nonBlock(nonBlock) { }

struct OpenFile::HandleIoctl {
	async::result<std::expected<void, DispatchError>> operator() (managarm::fs::GenericIoctlRequest &&req, helix::BorrowedDescriptor conversation, bragi::preamble, raw::OpenFile *self) {
		if (req.command() == CDROM_GET_CAPABILITY) {
			managarm::fs::GenericIoctlReply rsp;
			rsp.set_error(managarm::fs::Errors::NOT_A_TERMINAL);

			auto ser = rsp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		} else {
			co_await self->rawFs->device->handleIoctl(req, conversation);
		}

		co_return {};
	}
};

namespace {

async::result<protocols::fs::ReadResult> rawRead(void *object, helix_ng::CredentialsView,
		void *buffer, size_t length, async::cancellation_token) {
	auto start = helix::getClock();

	auto self = static_cast<raw::OpenFile *>(object);

	co_await self->offsetMutex.async_lock();
	frg::unique_lock offsetLock{frg::adopt_lock, self->offsetMutex};

	// TODO(geert): pass cancellation token through here
	auto file_size = co_await self->rawFs->device->getSize();

	if(self->offset >= file_size)
		co_return std::unexpected{protocols::fs::Error::endOfFile};

	auto remaining = file_size - self->offset;
	auto chunkSize = std::min(length, remaining);
	if(!chunkSize)
		co_return std::unexpected{protocols::fs::Error::endOfFile};

	// TODO(geert): use cancellation token here
	auto readMemory = co_await helix_ng::readMemory(
			helix::BorrowedDescriptor(self->rawFs->frontalMemory),
			self->offset, chunkSize, buffer);
	HEL_CHECK(readMemory.error());

	self->offset += chunkSize;

	auto end = helix::getClock();

	ostContext.emit(
		ostEvtRawRead,
		ostAttrNumBytes(length),
		ostAttrTime(end - start)
	);

	co_return chunkSize;
}

async::result<protocols::fs::ReadResult>
rawPread(void *object, int64_t offset, helix_ng::CredentialsView, void *buffer, size_t length) {
	size_t unsignedOffset = offset;

	auto start = helix::getClock();

	auto self = static_cast<raw::OpenFile *>(object);
	// TODO(geert): pass cancellation token through here
	auto file_size = co_await self->rawFs->device->getSize();

	if(unsignedOffset >= file_size)
		co_return std::unexpected{protocols::fs::Error::endOfFile};

	auto remaining = file_size - unsignedOffset;
	auto chunkSize = std::min(length, remaining);
	if(!chunkSize)
		co_return std::unexpected{protocols::fs::Error::endOfFile};

	// TODO(geert): use cancellation token here
	auto readMemory = co_await helix_ng::readMemory(
			helix::BorrowedDescriptor(self->rawFs->frontalMemory),
			unsignedOffset, chunkSize, buffer);
	HEL_CHECK(readMemory.error());

	auto end = helix::getClock();

	ostContext.emit(
		ostEvtRawRead,
		ostAttrNumBytes(length),
		ostAttrTime(end - start)
	);

	co_return chunkSize;
}

async::result<frg::expected<protocols::fs::Error, size_t>>
writeAt(raw::OpenFile *self, uint64_t offset, const void *buffer, size_t length) {
	if(!length)
		co_return size_t{0};

	auto start = helix::getClock();

	auto file_size = co_await self->rawFs->device->getSize();
	if(offset >= file_size)
		co_return protocols::fs::Error::noSpaceLeft;

	auto chunkSize = std::min(length, file_size - offset);
	auto writeMemory = co_await helix_ng::writeMemory(
			helix::BorrowedDescriptor(self->rawFs->frontalMemory),
			offset, chunkSize, buffer);
	HEL_CHECK(writeMemory.error());

	auto end = helix::getClock();

	ostContext.emit(
		ostEvtRawWrite,
		ostAttrNumBytes(length),
		ostAttrTime(end - start)
	);

	co_return chunkSize;
}

async::result<frg::expected<protocols::fs::Error, size_t>> rawWrite(void *object, helix_ng::CredentialsView,
		const void *buffer, size_t length) {
	auto self = static_cast<raw::OpenFile *>(object);

	co_await self->offsetMutex.async_lock();
	frg::unique_lock offsetLock{frg::adopt_lock, self->offsetMutex};

	auto res = co_await writeAt(self, self->offset, buffer, length);
	if(res)
		self->offset += res.value();
	co_return res;
}

async::result<frg::expected<protocols::fs::Error, size_t>> rawPwrite(void *object, int64_t offset,
		helix_ng::CredentialsView, const void *buffer, size_t length) {
	if(offset < 0)
		co_return protocols::fs::Error::illegalArguments;

	auto self = static_cast<raw::OpenFile *>(object);
	co_return co_await writeAt(self, offset, buffer, length);
}

async::result<protocols::fs::Error> rawFlock(void *object, int flags) {
	auto self = static_cast<raw::OpenFile*>(object);

	auto result = co_await self->rawFs->flockManager.lock(&self->flock, flags);
	co_return result;
}

async::result<protocols::fs::Error> rawSynchronize(void *object, protocols::fs::SynchronizeFlags) {
	auto self = static_cast<raw::OpenFile*>(object);

	auto result = co_await self->rawFs->synchronize();
	if(!result)
		co_return result.error() | protocols::fs::toFsProtoError;
	co_return protocols::fs::Error::none;
}

async::result<protocols::fs::SeekResult> rawSeekAbs(void *object, int64_t offset) {
	auto self = static_cast<raw::OpenFile*>(object);

	co_await self->offsetMutex.async_lock();
	frg::unique_lock offsetLock{frg::adopt_lock, self->offsetMutex};

	self->offset = offset;
	co_return static_cast<ssize_t>(self->offset);
}

async::result<protocols::fs::SeekResult> rawSeekRel(void *object, int64_t offset) {
	auto self = static_cast<raw::OpenFile*>(object);

	co_await self->offsetMutex.async_lock();
	frg::unique_lock offsetLock{frg::adopt_lock, self->offsetMutex};

	self->offset += offset;
	co_return static_cast<ssize_t>(self->offset);
}

async::result<protocols::fs::SeekResult> rawSeekEof(void *object, int64_t offset) {
	auto self = static_cast<raw::OpenFile *>(object);

	co_await self->offsetMutex.async_lock();
	frg::unique_lock offsetLock{frg::adopt_lock, self->offsetMutex};

	auto size = co_await self->rawFs->device->getSize();
	self->offset = offset + size;
	co_return static_cast<ssize_t>(self->offset);
}

async::result<void> rawIoctl(void *object, uint32_t, helix_ng::RecvInlineResult msg,
		helix::UniqueLane conversation) {
	auto self = static_cast<raw::OpenFile *>(object);

	auto res = co_await dispatchRequest<
		managarm::fs::GenericIoctlRequest
	>(conversation, std::move(msg), OpenFile::HandleIoctl{}, self);

	if (!res) {
		auto [dismiss] = co_await helix_ng::exchangeMsgs(
			conversation, helix_ng::dismiss());
		HEL_CHECK(dismiss.error());
	}
}

async::result<int> rawGetFileFlags(void *object) {
	auto self = static_cast<raw::OpenFile *>(object);

	// TODO: Report the real access mode once DEV_OPEN forwards it.
	int flags = O_RDWR;
	if(self->nonBlock.load(std::memory_order_relaxed))
		flags |= O_NONBLOCK;
	co_return flags;
}

async::result<void> rawSetFileFlags(void *object, int flags) {
	auto self = static_cast<raw::OpenFile *>(object);

	// Callers pass back the result of F_GETFL, which includes the access mode.
	if(auto unknown = flags & ~(O_ACCMODE | O_NONBLOCK); unknown)
		std::println(std::cout, "libblockfs: setFileFlags on raw device called with unknown flags {:#x}",
				unknown);

	self->nonBlock.store(flags & O_NONBLOCK, std::memory_order_relaxed);
	co_return;
}

} // namespace anonymous

constinit protocols::fs::FileOperations rawOperations {
	.seekAbs = rawSeekAbs,
	.seekRel = rawSeekRel,
	.seekEof = rawSeekEof,
	.read = rawRead,
	.pread = rawPread,
	.write = rawWrite,
	.pwrite = rawPwrite,
	.synchronize = rawSynchronize,
	.ioctl = rawIoctl,
	.flock = rawFlock,
	.getFileFlags = rawGetFileFlags,
	.setFileFlags = rawSetFileFlags,
};

} // namespace raw
} // namespace blockfs
