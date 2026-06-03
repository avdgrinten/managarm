#include "raw.hpp"
#include <protocols/posix/procdata.hpp>

#include <linux/cdrom.h>

#include <bragi/helpers-std.hpp>
#include <core/dispatch.hpp>
#include "fs.bragi.hpp"

namespace blockfs {
namespace raw {

RawFs::RawFs(BlockDevice *device)
: device{device} { }

async::result<void> RawFs::init() {
	auto device_size = co_await device->getSize();
	auto cache_size = (device_size + 0xFFF) & ~size_t(0xFFF);
	HEL_CHECK(helCreateManagedMemory(posix::getProcessHierarchy(), cache_size, 0,
				&backingMemory, &frontalMemory));

	manageMapping();
	co_return;
}

async::detached RawFs::manageMapping() {
	while(true) {
		helix::ManageMemory manage;
		auto &&submit = helix::submitManageMemory(helix::BorrowedDescriptor{backingMemory},
				&manage, helix::Dispatcher::global());
		co_await submit.async_wait();
		HEL_CHECK(manage.error());

		auto device_size = co_await device->getSize();
		auto cache_size = (device_size + 0xFFF) & ~size_t(0xFFF);
		assert(manage.offset() + manage.length() <= cache_size);

		if(manage.type() == kHelManageInitialize) {
			helix::Mapping file_map{helix::BorrowedDescriptor{backingMemory},
				static_cast<ptrdiff_t>(manage.offset()), manage.length(), kHelMapProtWrite};
			assert(!(manage.offset() & (device->sectorSize - 1)));

			size_t backed_size = std::min(manage.length(), device_size - manage.offset());
			size_t num_blocks = (backed_size + device->sectorSize - 1) / device->sectorSize;

			assert(num_blocks * device->sectorSize <= manage.length());
			co_await device->readSectors(manage.offset() / device->sectorSize, file_map.get(),
					num_blocks);

			HEL_CHECK(helUpdateMemory(backingMemory, kHelManageInitialize,
						manage.offset(), manage.length()));
		} else {
			assert(manage.type() == kHelManageWriteback);

			helix::Mapping file_map{helix::BorrowedDescriptor{backingMemory},
				static_cast<ptrdiff_t>(manage.offset()), manage.length(), kHelMapProtRead};

			assert(!(manage.offset() & (device->sectorSize - 1)));

			size_t backed_size = std::min(manage.length(), device_size - manage.offset());
			size_t num_blocks = (backed_size + device->sectorSize - 1) / device->sectorSize;

			assert(num_blocks * device->sectorSize <= manage.length());
			co_await device->writeSectors(manage.offset() / device->sectorSize, file_map.get(),
					num_blocks);

			HEL_CHECK(helUpdateMemory(backingMemory, kHelManageWriteback,
						manage.offset(), manage.length()));
		}
	}
}

OpenFile::OpenFile(RawFs *rawFs)
: rawFs(rawFs) { }

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
	uint64_t start;
	HEL_CHECK(helGetClock(&start));

	auto self = static_cast<raw::OpenFile *>(object);
	// TODO(geert): pass cancellation token through here
	auto file_size = co_await self->rawFs->device->getSize();

	if(self->offset >= file_size)
		co_return std::unexpected{protocols::fs::Error::endOfFile};

	auto remaining = file_size - self->offset;
	auto chunkSize = std::min(length, remaining);
	if(!chunkSize)
		co_return std::unexpected{protocols::fs::Error::endOfFile};

	auto chunk_offset = self->offset;
	self->offset += chunkSize;

	// TODO(geert): use cancellation token here
	auto readMemory = co_await helix_ng::readMemory(
			helix::BorrowedDescriptor(self->rawFs->frontalMemory),
			chunk_offset, chunkSize, buffer);
	HEL_CHECK(readMemory.error());

	uint64_t end;
	HEL_CHECK(helGetClock(&end));

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

	uint64_t start;
	HEL_CHECK(helGetClock(&start));

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

	uint64_t end;
	HEL_CHECK(helGetClock(&end));

	ostContext.emit(
		ostEvtRawRead,
		ostAttrNumBytes(length),
		ostAttrTime(end - start)
	);

	co_return chunkSize;
}

async::result<protocols::fs::Error> rawFlock(void *object, int flags) {
	auto self = static_cast<raw::OpenFile*>(object);

	auto result = co_await self->rawFs->flockManager.lock(&self->flock, flags);
	co_return result;
}

async::result<protocols::fs::SeekResult> rawSeekAbs(void *object, int64_t offset) {
	auto self = static_cast<raw::OpenFile*>(object);
	self->offset = offset;
	co_return static_cast<ssize_t>(self->offset);
}

async::result<protocols::fs::SeekResult> rawSeekRel(void *object, int64_t offset) {
	auto self = static_cast<raw::OpenFile*>(object);
	self->offset += offset;
	co_return static_cast<ssize_t>(self->offset);
}

async::result<protocols::fs::SeekResult> rawSeekEof(void *object, int64_t offset) {
	auto self = static_cast<raw::OpenFile *>(object);
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

} // namespace anonymous

constinit protocols::fs::FileOperations rawOperations {
	.seekAbs = rawSeekAbs,
	.seekRel = rawSeekRel,
	.seekEof = rawSeekEof,
	.read = rawRead,
	.pread = rawPread,
	.ioctl = rawIoctl,
	.flock = rawFlock,
};

} // namespace raw
} // namespace blockfs
