
#include <assert.h>
#include <stdio.h>
#include <deque>
#include <optional>
#include <optional>
#include <print>
#include <memory>
#include <sys/epoll.h>

#include <async/result.hpp>
#include <core/dispatch.hpp>
#include <helix/clock.hpp>
#include <helix/ipc.hpp>
#include <protocols/fs/defs.hpp>
#include <protocols/fs/server.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/mbus/client.hpp>
#include <libdrm/drm_fourcc.h>

#include "fs.bragi.hpp"

#include "core/drm/core.hpp"
#include "core/drm/debug.hpp"

// ----------------------------------------------------------------
// File
// ----------------------------------------------------------------

drm_core::File::File(std::shared_ptr<Device> device)
: _device(device), _eventSequence{1} {
	HelHandle handle;
	HEL_CHECK(helCreateIndirectMemory(size_t{1024} << 32, &handle));
	_memory = helix::UniqueDescriptor{handle};

	_statusPage.update(_eventSequence, 0);
};

void drm_core::File::setBlocking(bool blocking) {
	_isBlocking = blocking;
}

void drm_core::File::attachFrameBuffer(std::shared_ptr<drm_core::FrameBuffer> frame_buffer) {
	_frameBuffers.push_back(frame_buffer);
}

void drm_core::File::detachFrameBuffer(drm_core::FrameBuffer *frame_buffer) {
	auto it = std::find_if(_frameBuffers.begin(), _frameBuffers.end(),
			([&](std::shared_ptr<drm_core::FrameBuffer> fb) {
				return fb.get() == frame_buffer;
			}));
	assert(it != _frameBuffers.end());
	_frameBuffers.erase(it);
}

const std::vector<std::shared_ptr<drm_core::FrameBuffer>> &drm_core::File::getFrameBuffers() {
	return _frameBuffers;
}

async::result<uint32_t> drm_core::File::createHandle(std::shared_ptr<BufferObject> bo) {
	// Indirections stay installed after the handle is closed since existing mmap()s remain valid.
	// Hence, re-importing a BufferObject finds its indirection already installed.
	// Indirections are page-granular; the memory of a BufferObject extends to the next page.
	auto [boMemory, boOffset] = bo->getMemory();
	auto result = co_await helix_ng::installMemoryIndirection(_memory,
			bo->getMapping(), boMemory, boOffset, (bo->getSize() + 0xFFF) & ~size_t{0xFFF});
	if(result.error() != kHelErrAlreadyExists)
		HEL_CHECK(result.error());

	// Another request may have imported the BufferObject while we were installing it.
	if(auto existing = getHandle(bo))
		co_return *existing;

	auto handle = _allocator.allocate();
	auto ret = _buffers.insert({handle, bo});
	assert(ret.second);

	if(logDrmRequests)
		std::println("core/drm: createHandle for BufferObject {} -> handle {}", static_cast<void *>(bo.get()), handle);

	co_return handle;
}

drm_core::BufferObject *drm_core::File::resolveHandle(uint32_t handle) {
	auto it = _buffers.find(handle);
	if(it == _buffers.end())
		return nullptr;
	return it->second.get();
};

std::optional<uint32_t> drm_core::File::getHandle(std::shared_ptr<drm_core::BufferObject> bo) {
	for(auto &it : _buffers) {
		if(it.second == bo)
			return it.first;
	}

	return {};
};

/**
 * For the currently opened File, this exports a BufferObject references by the handle with
 * the credentials `creds` to the device. It also creates the mapping between credentials and the
 * DRM handle in this file.
 */
bool drm_core::File::exportBufferObject(uint32_t handle, helix_ng::Credentials creds) {
	auto bo = resolveHandle(handle);
	if(!bo)
		return false;
	auto buffer = bo->sharedBufferObject();

	_device->registerBufferObject(buffer, creds);
	return true;
}

/**
 * For the currently opened File, this imports the BufferObject from the device if necessary and
 * returns a pair of (BufferObject, DRM handle) for the `File`.
 */
async::result<std::pair<std::shared_ptr<drm_core::BufferObject>, uint32_t>>
drm_core::File::importBufferObject(helix_ng::Credentials creds) {
	auto bo = _device->findBufferObject(creds);
	if(!bo)
		co_return {};

	auto handle = getHandle(bo);

	if(!handle) {
		handle = co_await createHandle(bo);
	}

	co_return {bo, handle.value_or(-1)};
}

void drm_core::File::postEvent(drm_core::Event event) {
	event.timestamp = helix::getClock();

	if(_pendingEvents.empty()) {
		++_eventSequence;
		_statusPage.update(_eventSequence, EPOLLIN);
	}
	_pendingEvents.push_back(event);
	_eventBell.raise();
}

async::result<protocols::fs::ReadResult>
drm_core::File::read(void *object, helix_ng::CredentialsView,
		void *buffer, size_t length, async::cancellation_token ce) {
	auto self = static_cast<drm_core::File *>(object);

	if(!self->_isBlocking && self->_pendingEvents.empty())
		co_return std::unexpected{protocols::fs::Error::wouldBlock};
	while (self->_pendingEvents.empty()) {
		if (!co_await self->_eventBell.async_wait(ce))
			co_return std::unexpected{protocols::fs::Error::interrupted};
	}

	auto ev = &self->_pendingEvents.front();

	// TODO: Support sequence number and CRTC id.
	drm_event_vblank out;
	memset(&out, 0, sizeof(drm_event_vblank));
	out.base.type = DRM_EVENT_FLIP_COMPLETE;
	out.base.length = sizeof(drm_event_vblank);
	out.user_data = ev->cookie;
	out.crtc_id = ev->crtcId;
	out.tv_sec = ev->timestamp / 1000000000;
	out.tv_usec = (ev->timestamp % 1000000000) / 1000;

	assert(length >= sizeof(drm_event_vblank));
	memcpy(buffer, &out, sizeof(drm_event_vblank));

	self->_pendingEvents.pop_front();
	if(self->_pendingEvents.empty())
		self->_statusPage.update(self->_eventSequence, 0);

	co_return sizeof(drm_event_vblank);
}

async::result<helix::BorrowedDescriptor>
drm_core::File::accessMemory(void *object) {
	auto self = static_cast<drm_core::File *>(object);
	co_return self->_memory;
}

async::result<frg::expected<protocols::fs::Error, protocols::fs::PollWaitResult>>
drm_core::File::pollWait(void *object, uint64_t sequence, int mask,
		async::cancellation_token c) {
	auto self = static_cast<drm_core::File *>(object);

	if(sequence > self->_eventSequence)
		co_return protocols::fs::Error::illegalArguments;

	int edges = 0;

	// Wait until we surpass the input sequence.
	while (true) {
		edges = 0;
		if (self->_eventSequence > sequence)
			edges |= EPOLLIN;

		if (edges & mask)
			break;

		if (!co_await self->_eventBell.async_wait(c))
			break;
	}

	co_return protocols::fs::PollWaitResult{self->_eventSequence, edges & mask};
}

async::result<frg::expected<protocols::fs::Error, protocols::fs::PollStatusResult>>
drm_core::File::pollStatus(void *object) {
	auto self = static_cast<drm_core::File *>(object);

	int s = 0;
	if(!self->_pendingEvents.empty())
		s |= EPOLLIN;

	co_return protocols::fs::PollStatusResult{self->_eventSequence, s};
}

void drm_core::File::_retirePageFlip(uint64_t cookie, uint32_t crtc_id) {
	Event event;
	event.cookie = cookie;
	event.crtcId = crtc_id;
	postEvent(event);
}

drm_core::PrimeFile::PrimeFile(helix::BorrowedDescriptor handle, size_t size)
: size(size) {
	_memory = std::move(handle);
};

async::result<helix::BorrowedDescriptor>
drm_core::PrimeFile::accessMemory(void *object) {
	auto self = static_cast<drm_core::PrimeFile *>(object);
	co_return self->_memory;
}

async::result<protocols::fs::SeekResult>
drm_core::PrimeFile::seekAbs(void *object, int64_t offset) {
	auto self = static_cast<drm_core::PrimeFile *>(object);
	self->offset = offset;
	co_return static_cast<ssize_t>(self->offset);
}

async::result<protocols::fs::SeekResult>
drm_core::PrimeFile::seekRel(void *object, int64_t offset) {
	auto self = static_cast<drm_core::PrimeFile *>(object);
	self->offset += offset;
	co_return static_cast<ssize_t>(self->offset);
}

async::result<protocols::fs::SeekResult>
drm_core::PrimeFile::seekEof(void *object, int64_t offset) {
	auto self = static_cast<drm_core::PrimeFile  *>(object);
	self->offset = offset + self->size;
	co_return static_cast<ssize_t>(self->offset);
}

async::result<frg::expected<protocols::fs::Error, protocols::fs::PollStatusResult>>
drm_core::PrimeFile::pollStatus(void *) {
	// TODO: properly support this once we implement fences
	co_return protocols::fs::PollStatusResult{1, EPOLLIN | EPOLLOUT};
}

async::result<frg::expected<protocols::fs::Error, protocols::fs::PollWaitResult>>
drm_core::PrimeFile::pollWait(
    void *, uint64_t sequence, int, async::cancellation_token cancellation
) {
	// TODO: properly support this once we implement fences
	if (sequence == 0)
		co_return protocols::fs::PollWaitResult{1, EPOLLIN | EPOLLOUT};
	co_await async::suspend_indefinitely(cancellation);
	co_return protocols::fs::Error::interrupted;
}

namespace drm_core {

static constexpr auto defaultFileOperations = protocols::fs::FileOperations{
	.read = &File::read,
	.accessMemory = &File::accessMemory,
	.ioctl = &File::ioctl,
	.pollWait = &File::pollWait,
	.pollStatus = &File::pollStatus
};

namespace {

struct ServeDeviceVisitor {
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::CntRequest &&req,
			helix::BorrowedDescriptor conversation, bragi::preamble,
			std::shared_ptr<drm_core::Device> device) {
		if(req.req_type() == managarm::fs::CntReqType::DEV_OPEN) {
			if(req.flags() & ~(managarm::fs::OpenFlags::OF_NONBLOCK)) {
				std::println("\e[31mcore/drm: Illegal flags {} for DEV_OPEN\e[39m", req.flags());

				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);

				auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
				HEL_CHECK(send_resp.error());
			}

			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			auto file = smarter::make_shared<drm_core::File>(device);

			if(req.flags() & managarm::fs::OpenFlags::OF_NONBLOCK)
				file->setBlocking(false);

			async::detach(protocols::fs::servePassthrough(
					std::move(local_lane), file, &defaultFileOperations));

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_caps(managarm::fs::FileCaps::FC_STATUS_PAGE | managarm::fs::FileCaps::FC_POSIX_LANE);

			auto [send_resp, push_pt, push_page] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
				helix_ng::pushDescriptor(remote_lane, kHelRightInvoke | kHelRightManage),
				helix_ng::pushDescriptor(file->statusPageMemory(), kHelRightRead | kHelRightAssign)
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_pt.error());
			HEL_CHECK(push_page.error());
		}else if(req.req_type() == managarm::fs::CntReqType::OPEN_FD_LANE) {
			auto [fd_lane] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::pullDescriptor(kHelRightInvoke | kHelRightManage)
			);
			HEL_CHECK(fd_lane.error());

			device->_posixLane = fd_lane.descriptor();
		}else{
			throw std::runtime_error("Invalid request in serveDevice()");
		}
		co_return {};
	}
};

} // namespace

async::detached serveDrmDevice(std::shared_ptr<drm_core::Device> device,
		helix::UniqueLane lane) {
	while(true) {
		auto res = co_await dispatchRequest<
			managarm::fs::CntRequest
		>(lane, ServeDeviceVisitor{}, device);
		if(!res) {
			if(res.error() == DispatchError::shutdown)
				co_return;
			std::cout << "core/drm: dispatch error" << std::endl;
			continue;
		}
	}
}

} // namespace core_drm

// ----------------------------------------------------------------
// Functions
// ----------------------------------------------------------------

uint32_t drm_core::convertLegacyFormat(uint32_t bpp, uint32_t depth) {
	switch(bpp) {
		case 8:
			assert(depth == 8);
			return DRM_FORMAT_C8;

		case 16:
			assert(depth == 15 || depth == 16);
			if(depth == 15) {
				return DRM_FORMAT_XRGB1555;
			}else {
				return DRM_FORMAT_RGB565;
			}

		case 24:
			assert(depth == 24);
			return DRM_FORMAT_RGB888;

		case 32:
			assert(depth == 24 || depth == 30 || depth == 32);
			if(depth == 24) {
				return DRM_FORMAT_XRGB8888;
			}else if(depth == 30) {
				return DRM_FORMAT_XRGB2101010;
			}else {
				return DRM_FORMAT_ARGB8888;
			}

		default:
			throw std::runtime_error("Bad BPP");
	}
}

drm_mode_modeinfo drm_core::makeModeInfo(const char *name, uint32_t type,
		uint32_t clock, unsigned int hdisplay, unsigned int hsync_start,
		unsigned int hsync_end, unsigned int htotal, unsigned int hskew,
		unsigned int vdisplay, unsigned int vsync_start, unsigned int vsync_end,
		unsigned int vtotal, unsigned int vscan, uint32_t flags) {
	drm_mode_modeinfo mode_info{};
	mode_info.clock = clock;
	mode_info.hdisplay = hdisplay;
	mode_info.hsync_start = hsync_start;
	mode_info.hsync_end = hsync_end;
	mode_info.htotal = htotal;
	mode_info.hskew = hskew;
	mode_info.vdisplay = vdisplay;
	mode_info.vsync_start = vsync_start;
	mode_info.vsync_end = vsync_end;
	mode_info.vtotal = vtotal;
	mode_info.vscan = vscan;
	mode_info.flags = flags;
	mode_info.type = type;
	strcpy(mode_info.name, name);

	// Same computation as Linux' drm_mode_vrefresh().
	if(htotal && vtotal) {
		uint64_t num = uint64_t{clock} * 1000;
		uint64_t den = uint64_t{htotal} * vtotal;
		if(flags & DRM_MODE_FLAG_INTERLACE)
			num *= 2;
		if(flags & DRM_MODE_FLAG_DBLSCAN)
			den *= 2;
		if(vscan > 1)
			den *= vscan;
		mode_info.vrefresh = (num + den / 2) / den;
	}
	return mode_info;
};

void drm_core::addDmtModes(std::vector<drm_mode_modeinfo> &supported_modes,
		unsigned int max_width, unsigned max_height) {
	drm_mode_modeinfo modes[] = {
	/* 0x01 - 640x350@85Hz */
	makeModeInfo("640x350", DRM_MODE_TYPE_DRIVER, 31500, 640, 672,
		   736, 832, 0, 350, 382, 385, 445, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x02 - 640x400@85Hz */
	makeModeInfo("640x400", DRM_MODE_TYPE_DRIVER, 31500, 640, 672,
		   736, 832, 0, 400, 401, 404, 445, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x03 - 720x400@85Hz */
	makeModeInfo("720x400", DRM_MODE_TYPE_DRIVER, 35500, 720, 756,
		   828, 936, 0, 400, 401, 404, 446, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x04 - 640x480@60Hz */
	makeModeInfo("640x480", DRM_MODE_TYPE_DRIVER, 25175, 640, 656,
		   752, 800, 0, 480, 490, 492, 525, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x05 - 640x480@72Hz */
	makeModeInfo("640x480", DRM_MODE_TYPE_DRIVER, 31500, 640, 664,
		   704, 832, 0, 480, 489, 492, 520, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x06 - 640x480@75Hz */
	makeModeInfo("640x480", DRM_MODE_TYPE_DRIVER, 31500, 640, 656,
		   720, 840, 0, 480, 481, 484, 500, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x07 - 640x480@85Hz */
	makeModeInfo("640x480", DRM_MODE_TYPE_DRIVER, 36000, 640, 696,
		   752, 832, 0, 480, 481, 484, 509, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x08 - 800x600@56Hz */
	makeModeInfo("800x600", DRM_MODE_TYPE_DRIVER, 36000, 800, 824,
		   896, 1024, 0, 600, 601, 603, 625, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x09 - 800x600@60Hz */
	makeModeInfo("800x600", DRM_MODE_TYPE_DRIVER, 40000, 800, 840,
		   968, 1056, 0, 600, 601, 605, 628, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x0a - 800x600@72Hz */
	makeModeInfo("800x600", DRM_MODE_TYPE_DRIVER, 50000, 800, 856,
		   976, 1040, 0, 600, 637, 643, 666, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x0b - 800x600@75Hz */
	makeModeInfo("800x600", DRM_MODE_TYPE_DRIVER, 49500, 800, 816,
		   896, 1056, 0, 600, 601, 604, 625, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x0c - 800x600@85Hz */
	makeModeInfo("800x600", DRM_MODE_TYPE_DRIVER, 56250, 800, 832,
		   896, 1048, 0, 600, 601, 604, 631, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x0d - 800x600@120Hz RB */
	makeModeInfo("800x600", DRM_MODE_TYPE_DRIVER, 73250, 800, 848,
		   880, 960, 0, 600, 603, 607, 636, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x0e - 848x480@60Hz */
	makeModeInfo("848x480", DRM_MODE_TYPE_DRIVER, 33750, 848, 864,
		   976, 1088, 0, 480, 486, 494, 517, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x0f - 1024x768@43Hz, interlace */
	makeModeInfo("1024x768i", DRM_MODE_TYPE_DRIVER, 44900, 1024, 1032,
		   1208, 1264, 0, 768, 768, 776, 817, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC |
		   DRM_MODE_FLAG_INTERLACE),
	/* 0x10 - 1024x768@60Hz */
	makeModeInfo("1024x768", DRM_MODE_TYPE_DRIVER, 65000, 1024, 1048,
		   1184, 1344, 0, 768, 771, 777, 806, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x11 - 1024x768@70Hz */
	makeModeInfo("1024x768", DRM_MODE_TYPE_DRIVER, 75000, 1024, 1048,
		   1184, 1328, 0, 768, 771, 777, 806, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x12 - 1024x768@75Hz */
	makeModeInfo("1024x768", DRM_MODE_TYPE_DRIVER, 78750, 1024, 1040,
		   1136, 1312, 0, 768, 769, 772, 800, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x13 - 1024x768@85Hz */
	makeModeInfo("1024x768", DRM_MODE_TYPE_DRIVER, 94500, 1024, 1072,
		   1168, 1376, 0, 768, 769, 772, 808, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x14 - 1024x768@120Hz RB */
	makeModeInfo("1024x768", DRM_MODE_TYPE_DRIVER, 115500, 1024, 1072,
		   1104, 1184, 0, 768, 771, 775, 813, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x15 - 1152x864@75Hz */
	makeModeInfo("1152x864", DRM_MODE_TYPE_DRIVER, 108000, 1152, 1216,
		   1344, 1600, 0, 864, 865, 868, 900, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x55 - 1280x720@60Hz */
	makeModeInfo("1280x720", DRM_MODE_TYPE_DRIVER, 74250, 1280, 1390,
		   1430, 1650, 0, 720, 725, 730, 750, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x16 - 1280x768@60Hz RB */
	makeModeInfo("1280x768", DRM_MODE_TYPE_DRIVER, 68250, 1280, 1328,
		   1360, 1440, 0, 768, 771, 778, 790, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x17 - 1280x768@60Hz */
	makeModeInfo("1280x768", DRM_MODE_TYPE_DRIVER, 79500, 1280, 1344,
		   1472, 1664, 0, 768, 771, 778, 798, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x18 - 1280x768@75Hz */
	makeModeInfo("1280x768", DRM_MODE_TYPE_DRIVER, 102250, 1280, 1360,
		   1488, 1696, 0, 768, 771, 778, 805, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x19 - 1280x768@85Hz */
	makeModeInfo("1280x768", DRM_MODE_TYPE_DRIVER, 117500, 1280, 1360,
		   1496, 1712, 0, 768, 771, 778, 809, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x1a - 1280x768@120Hz RB */
	makeModeInfo("1280x768", DRM_MODE_TYPE_DRIVER, 140250, 1280, 1328,
		   1360, 1440, 0, 768, 771, 778, 813, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x1b - 1280x800@60Hz RB */
	makeModeInfo("1280x800", DRM_MODE_TYPE_DRIVER, 71000, 1280, 1328,
		   1360, 1440, 0, 800, 803, 809, 823, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x1c - 1280x800@60Hz */
	makeModeInfo("1280x800", DRM_MODE_TYPE_DRIVER, 83500, 1280, 1352,
		   1480, 1680, 0, 800, 803, 809, 831, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x1d - 1280x800@75Hz */
	makeModeInfo("1280x800", DRM_MODE_TYPE_DRIVER, 106500, 1280, 1360,
		   1488, 1696, 0, 800, 803, 809, 838, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x1e - 1280x800@85Hz */
	makeModeInfo("1280x800", DRM_MODE_TYPE_DRIVER, 122500, 1280, 1360,
		   1496, 1712, 0, 800, 803, 809, 843, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x1f - 1280x800@120Hz RB */
	makeModeInfo("1280x800", DRM_MODE_TYPE_DRIVER, 146250, 1280, 1328,
		   1360, 1440, 0, 800, 803, 809, 847, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x20 - 1280x960@60Hz */
	makeModeInfo("1280x960", DRM_MODE_TYPE_DRIVER, 108000, 1280, 1376,
		   1488, 1800, 0, 960, 961, 964, 1000, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x21 - 1280x960@85Hz */
	makeModeInfo("1280x960", DRM_MODE_TYPE_DRIVER, 148500, 1280, 1344,
		   1504, 1728, 0, 960, 961, 964, 1011, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x22 - 1280x960@120Hz RB */
	makeModeInfo("1280x960", DRM_MODE_TYPE_DRIVER, 175500, 1280, 1328,
		   1360, 1440, 0, 960, 963, 967, 1017, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x23 - 1280x1024@60Hz */
	makeModeInfo("1280x1024", DRM_MODE_TYPE_DRIVER, 108000, 1280, 1328,
		   1440, 1688, 0, 1024, 1025, 1028, 1066, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x24 - 1280x1024@75Hz */
	makeModeInfo("1280x1024", DRM_MODE_TYPE_DRIVER, 135000, 1280, 1296,
		   1440, 1688, 0, 1024, 1025, 1028, 1066, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x25 - 1280x1024@85Hz */
	makeModeInfo("1280x1024", DRM_MODE_TYPE_DRIVER, 157500, 1280, 1344,
		   1504, 1728, 0, 1024, 1025, 1028, 1072, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x26 - 1280x1024@120Hz RB */
	makeModeInfo("1280x1024", DRM_MODE_TYPE_DRIVER, 187250, 1280, 1328,
		   1360, 1440, 0, 1024, 1027, 1034, 1084, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x27 - 1360x768@60Hz */
	makeModeInfo("1360x768", DRM_MODE_TYPE_DRIVER, 85500, 1360, 1424,
		   1536, 1792, 0, 768, 771, 777, 795, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x28 - 1360x768@120Hz RB */
	makeModeInfo("1360x768", DRM_MODE_TYPE_DRIVER, 148250, 1360, 1408,
		   1440, 1520, 0, 768, 771, 776, 813, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x51 - 1366x768@60Hz */
	makeModeInfo("1366x768", DRM_MODE_TYPE_DRIVER, 85500, 1366, 1436,
		   1579, 1792, 0, 768, 771, 774, 798, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x56 - 1366x768@60Hz */
	makeModeInfo("1366x768", DRM_MODE_TYPE_DRIVER, 72000, 1366, 1380,
		   1436, 1500, 0, 768, 769, 772, 800, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x29 - 1400x1050@60Hz RB */
	makeModeInfo("1400x1050", DRM_MODE_TYPE_DRIVER, 101000, 1400, 1448,
		   1480, 1560, 0, 1050, 1053, 1057, 1080, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x2a - 1400x1050@60Hz */
	makeModeInfo("1400x1050", DRM_MODE_TYPE_DRIVER, 121750, 1400, 1488,
		   1632, 1864, 0, 1050, 1053, 1057, 1089, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x2b - 1400x1050@75Hz */
	makeModeInfo("1400x1050", DRM_MODE_TYPE_DRIVER, 156000, 1400, 1504,
		   1648, 1896, 0, 1050, 1053, 1057, 1099, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x2c - 1400x1050@85Hz */
	makeModeInfo("1400x1050", DRM_MODE_TYPE_DRIVER, 179500, 1400, 1504,
		   1656, 1912, 0, 1050, 1053, 1057, 1105, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x2d - 1400x1050@120Hz RB */
	makeModeInfo("1400x1050", DRM_MODE_TYPE_DRIVER, 208000, 1400, 1448,
		   1480, 1560, 0, 1050, 1053, 1057, 1112, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x2e - 1440x900@60Hz RB */
	makeModeInfo("1440x900", DRM_MODE_TYPE_DRIVER, 88750, 1440, 1488,
		   1520, 1600, 0, 900, 903, 909, 926, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x2f - 1440x900@60Hz */
	makeModeInfo("1440x900", DRM_MODE_TYPE_DRIVER, 106500, 1440, 1520,
		   1672, 1904, 0, 900, 903, 909, 934, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x30 - 1440x900@75Hz */
	makeModeInfo("1440x900", DRM_MODE_TYPE_DRIVER, 136750, 1440, 1536,
		   1688, 1936, 0, 900, 903, 909, 942, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x31 - 1440x900@85Hz */
	makeModeInfo("1440x900", DRM_MODE_TYPE_DRIVER, 157000, 1440, 1544,
		   1696, 1952, 0, 900, 903, 909, 948, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x32 - 1440x900@120Hz RB */
	makeModeInfo("1440x900", DRM_MODE_TYPE_DRIVER, 182750, 1440, 1488,
		   1520, 1600, 0, 900, 903, 909, 953, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x53 - 1600x900@60Hz */
	makeModeInfo("1600x900", DRM_MODE_TYPE_DRIVER, 108000, 1600, 1624,
		   1704, 1800, 0, 900, 901, 904, 1000, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x33 - 1600x1200@60Hz */
	makeModeInfo("1600x1200", DRM_MODE_TYPE_DRIVER, 162000, 1600, 1664,
		   1856, 2160, 0, 1200, 1201, 1204, 1250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x34 - 1600x1200@65Hz */
	makeModeInfo("1600x1200", DRM_MODE_TYPE_DRIVER, 175500, 1600, 1664,
		   1856, 2160, 0, 1200, 1201, 1204, 1250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x35 - 1600x1200@70Hz */
	makeModeInfo("1600x1200", DRM_MODE_TYPE_DRIVER, 189000, 1600, 1664,
		   1856, 2160, 0, 1200, 1201, 1204, 1250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x36 - 1600x1200@75Hz */
	makeModeInfo("1600x1200", DRM_MODE_TYPE_DRIVER, 202500, 1600, 1664,
		   1856, 2160, 0, 1200, 1201, 1204, 1250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x37 - 1600x1200@85Hz */
	makeModeInfo("1600x1200", DRM_MODE_TYPE_DRIVER, 229500, 1600, 1664,
		   1856, 2160, 0, 1200, 1201, 1204, 1250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x38 - 1600x1200@120Hz RB */
	makeModeInfo("1600x1200", DRM_MODE_TYPE_DRIVER, 268250, 1600, 1648,
		   1680, 1760, 0, 1200, 1203, 1207, 1271, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x39 - 1680x1050@60Hz RB */
	makeModeInfo("1680x1050", DRM_MODE_TYPE_DRIVER, 119000, 1680, 1728,
		   1760, 1840, 0, 1050, 1053, 1059, 1080, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x3a - 1680x1050@60Hz */
	makeModeInfo("1680x1050", DRM_MODE_TYPE_DRIVER, 146250, 1680, 1784,
		   1960, 2240, 0, 1050, 1053, 1059, 1089, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x3b - 1680x1050@75Hz */
	makeModeInfo("1680x1050", DRM_MODE_TYPE_DRIVER, 187000, 1680, 1800,
		   1976, 2272, 0, 1050, 1053, 1059, 1099, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x3c - 1680x1050@85Hz */
	makeModeInfo("1680x1050", DRM_MODE_TYPE_DRIVER, 214750, 1680, 1808,
		   1984, 2288, 0, 1050, 1053, 1059, 1105, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x3d - 1680x1050@120Hz RB */
	makeModeInfo("1680x1050", DRM_MODE_TYPE_DRIVER, 245500, 1680, 1728,
		   1760, 1840, 0, 1050, 1053, 1059, 1112, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x3e - 1792x1344@60Hz */
	makeModeInfo("1792x1344", DRM_MODE_TYPE_DRIVER, 204750, 1792, 1920,
		   2120, 2448, 0, 1344, 1345, 1348, 1394, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x3f - 1792x1344@75Hz */
	makeModeInfo("1792x1344", DRM_MODE_TYPE_DRIVER, 261000, 1792, 1888,
		   2104, 2456, 0, 1344, 1345, 1348, 1417, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x40 - 1792x1344@120Hz RB */
	makeModeInfo("1792x1344", DRM_MODE_TYPE_DRIVER, 333250, 1792, 1840,
		   1872, 1952, 0, 1344, 1347, 1351, 1423, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x41 - 1856x1392@60Hz */
	makeModeInfo("1856x1392", DRM_MODE_TYPE_DRIVER, 218250, 1856, 1952,
		   2176, 2528, 0, 1392, 1393, 1396, 1439, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x42 - 1856x1392@75Hz */
	makeModeInfo("1856x1392", DRM_MODE_TYPE_DRIVER, 288000, 1856, 1984,
		   2208, 2560, 0, 1392, 1393, 1396, 1500, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x43 - 1856x1392@120Hz RB */
	makeModeInfo("1856x1392", DRM_MODE_TYPE_DRIVER, 356500, 1856, 1904,
		   1936, 2016, 0, 1392, 1395, 1399, 1474, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x52 - 1920x1080@60Hz */
	makeModeInfo("1920x1080", DRM_MODE_TYPE_DRIVER, 148500, 1920, 2008,
		   2052, 2200, 0, 1080, 1084, 1089, 1125, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x44 - 1920x1200@60Hz RB */
	makeModeInfo("1920x1200", DRM_MODE_TYPE_DRIVER, 154000, 1920, 1968,
		   2000, 2080, 0, 1200, 1203, 1209, 1235, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x45 - 1920x1200@60Hz */
	makeModeInfo("1920x1200", DRM_MODE_TYPE_DRIVER, 193250, 1920, 2056,
		   2256, 2592, 0, 1200, 1203, 1209, 1245, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x46 - 1920x1200@75Hz */
	makeModeInfo("1920x1200", DRM_MODE_TYPE_DRIVER, 245250, 1920, 2056,
		   2264, 2608, 0, 1200, 1203, 1209, 1255, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x47 - 1920x1200@85Hz */
	makeModeInfo("1920x1200", DRM_MODE_TYPE_DRIVER, 281250, 1920, 2064,
		   2272, 2624, 0, 1200, 1203, 1209, 1262, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x48 - 1920x1200@120Hz RB */
	makeModeInfo("1920x1200", DRM_MODE_TYPE_DRIVER, 317000, 1920, 1968,
		   2000, 2080, 0, 1200, 1203, 1209, 1271, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x49 - 1920x1440@60Hz */
	makeModeInfo("1920x1440", DRM_MODE_TYPE_DRIVER, 234000, 1920, 2048,
		   2256, 2600, 0, 1440, 1441, 1444, 1500, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x4a - 1920x1440@75Hz */
	makeModeInfo("1920x1440", DRM_MODE_TYPE_DRIVER, 297000, 1920, 2064,
		   2288, 2640, 0, 1440, 1441, 1444, 1500, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x4b - 1920x1440@120Hz RB */
	makeModeInfo("1920x1440", DRM_MODE_TYPE_DRIVER, 380500, 1920, 1968,
		   2000, 2080, 0, 1440, 1443, 1447, 1525, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x54 - 2048x1152@60Hz */
	makeModeInfo("2048x1152", DRM_MODE_TYPE_DRIVER, 162000, 2048, 2074,
		   2154, 2250, 0, 1152, 1153, 1156, 1200, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x4c - 2560x1600@60Hz RB */
	makeModeInfo("2560x1600", DRM_MODE_TYPE_DRIVER, 268500, 2560, 2608,
		   2640, 2720, 0, 1600, 1603, 1609, 1646, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x4d - 2560x1600@60Hz */
	makeModeInfo("2560x1600", DRM_MODE_TYPE_DRIVER, 348500, 2560, 2752,
		   3032, 3504, 0, 1600, 1603, 1609, 1658, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x4e - 2560x1600@75Hz */
	makeModeInfo("2560x1600", DRM_MODE_TYPE_DRIVER, 443250, 2560, 2768,
		   3048, 3536, 0, 1600, 1603, 1609, 1672, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x4f - 2560x1600@85Hz */
	makeModeInfo("2560x1600", DRM_MODE_TYPE_DRIVER, 505250, 2560, 2768,
		   3048, 3536, 0, 1600, 1603, 1609, 1682, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x50 - 2560x1600@120Hz RB */
	makeModeInfo("2560x1600", DRM_MODE_TYPE_DRIVER, 552750, 2560, 2608,
		   2640, 2720, 0, 1600, 1603, 1609, 1694, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x57 - 4096x2160@60Hz RB */
	makeModeInfo("4096x2160", DRM_MODE_TYPE_DRIVER, 556744, 4096, 4104,
		   4136, 4176, 0, 2160, 2208, 2216, 2222, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x58 - 4096x2160@59.94Hz RB */
	makeModeInfo("4096x2160", DRM_MODE_TYPE_DRIVER, 556188, 4096, 4104,
		   4136, 4176, 0, 2160, 2208, 2216, 2222, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC)
	};
	size_t size = sizeof(modes) / sizeof(drm_mode_modeinfo);

	for(size_t i = 0; i < size; i++) {
		if(modes[i].hdisplay <= max_width && modes[i].vdisplay <= max_height)
			supported_modes.push_back(modes[i]);
	}
}

