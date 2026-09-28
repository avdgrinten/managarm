#pragma once

#include <queue>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <variant>

#include <arch/mem_space.hpp>
#include <async/cancellation.hpp>
#include <async/recurring-event.hpp>
#include <async/oneshot-event.hpp>
#include <async/mutex.hpp>
#include <async/result.hpp>
#include <core/id-allocator.hpp>
#include <helix/memory.hpp>

#include "fwd-decls.hpp"

#include "device.hpp"
#include "range-allocator.hpp"
#include "property.hpp"

#include <libdrm/drm.h>
#include <libdrm/drm_mode.h>

#include <protocols/fs/defs.hpp>
#include <protocols/fs/server.hpp>

namespace drm_core {

struct Event {
	uint64_t cookie;
	uint32_t crtcId;
	uint64_t timestamp;
};

/**
 * This structure tracks DRM state per open file descriptor.
 */
struct File {
	File(std::shared_ptr<Device> device);

	struct HandleIoctl;

	static async::result<protocols::fs::ReadResult>

	/**
	 * A read operation on a DRM fd returning pending events, if any.
	 */
	read(void *object, helix_ng::CredentialsView, void *buffer, size_t length, async::cancellation_token ce);

	static async::result<helix::BorrowedDescriptor>
	accessMemory(void *object);

	static async::result<void>
	ioctl(void *object, uint32_t id, helix_ng::RecvInlineResult, helix::UniqueLane conversation);

	static async::result<frg::expected<protocols::fs::Error, protocols::fs::PollWaitResult>>
	pollWait(void *object, uint64_t sequence, int mask,
			async::cancellation_token cancellation);

	static async::result<frg::expected<protocols::fs::Error, protocols::fs::PollStatusResult>>
	pollStatus(void *object);

	void setBlocking(bool blocking);

	void attachFrameBuffer(std::shared_ptr<FrameBuffer> frame_buffer);
	void detachFrameBuffer(FrameBuffer *frame_buffer);
	const std::vector<std::shared_ptr<FrameBuffer>> &getFrameBuffers();

	/**
	 * Prepare a BufferObject to be mmap'ed by userspace.
	 *
	 * mmap()ing buffers works by providing a (fake) offset that can be used on the
	 * DRM fd to map the requested BufferObject. The (fake) offset is returned.
	 * Obviously, this offset is only valid on the DRM fd that is was set up on.
	 *
	 * @param bo BufferObject to be set up for mapping
	 * @return uint32_t The offset to be used to mmap the BufferObject
	 */
	async::result<uint32_t> createHandle(std::shared_ptr<BufferObject> bo);
	BufferObject *resolveHandle(uint32_t handle);
	std::optional<uint32_t> getHandle(std::shared_ptr<drm_core::BufferObject> bo);

	bool exportBufferObject(uint32_t handle, helix_ng::Credentials creds);

	async::result<std::pair<std::shared_ptr<drm_core::BufferObject>, uint32_t>>
	importBufferObject(helix_ng::Credentials creds);

	/**
	 * Add an @p event to the queue of pending events to be read by userspace.
	 *
	 * @param[in] event
	 */
	void postEvent(Event event);

	helix::BorrowedDescriptor statusPageMemory() {
		return _statusPage.getMemory();
	}

private:
	async::detached pageFlipEvent(std::unique_ptr<drm_core::Configuration> config,
		drm_core::File *self, uint64_t cookie, uint32_t crtc_id);
	async::detached pageFlipEvent(std::unique_ptr<drm_core::Configuration> config,
		drm_core::File *self, uint64_t cookie, std::unordered_set<uint32_t> crtc_id);
	void _retirePageFlip(uint64_t cookie, uint32_t crtc_id);

	std::shared_ptr<Device> _device;

	helix::UniqueDescriptor _memory;

	std::vector<std::shared_ptr<FrameBuffer>> _frameBuffers;

	// BufferObjects associated with this file.
	std::unordered_map<uint32_t, std::shared_ptr<BufferObject>> _buffers;
	// id allocator for mapping BufferObjects
	id_allocator<uint32_t> _allocator;

	// Event queuing structures.
	bool _isBlocking = true;
	std::deque<Event> _pendingEvents;
	uint64_t _eventSequence;
	async::recurring_event _eventBell;

	protocols::fs::StatusPageProvider _statusPage;

	bool universalPlanes;
	bool atomic;
};

struct PrimeFile {
	PrimeFile(helix::BorrowedDescriptor handle, size_t size);

	static async::result<helix::BorrowedDescriptor> accessMemory(void *object);

	static async::result<protocols::fs::SeekResult> seekAbs(void *object, int64_t offset);
	static async::result<protocols::fs::SeekResult> seekRel(void *object, int64_t offset);
	static async::result<protocols::fs::SeekResult> seekEof(void *object, int64_t offset);
	static async::result<frg::expected<protocols::fs::Error, protocols::fs::PollStatusResult>> pollStatus(void *object);
	static async::result<frg::expected<protocols::fs::Error, protocols::fs::PollWaitResult>> pollWait(void *object, uint64_t sequence, int mask, async::cancellation_token cancellation);

	helix::BorrowedDescriptor _memory;

	size_t offset;
	size_t size;
};

struct Configuration {
	virtual ~Configuration() = default;

	virtual bool capture(std::vector<Assignment> assignment, std::unique_ptr<AtomicState> &state) = 0;
	virtual void dispose() = 0;
	virtual void commit(std::unique_ptr<AtomicState> state) = 0;

	auto waitForCompletion() {
		return _ev.wait();
	}

protected:
	// TODO: Let derive classes handle the event?
	void complete() {
		_ev.raise();
	}

private:
	async::oneshot_primitive _ev;
};

async::detached serveDrmDevice(std::shared_ptr<drm_core::Device> device,
		helix::UniqueLane lane);

// ---------------------------------------------
// Formats
// ---------------------------------------------

uint32_t convertLegacyFormat(uint32_t bpp, uint32_t depth);

#define DRM_FORMAT_MAX_PLANES 4

struct FormatInfo {
	uint32_t format;
	bool has_alpha;
	uint8_t char_per_block[DRM_FORMAT_MAX_PLANES];
	uint8_t block_w[DRM_FORMAT_MAX_PLANES] = {1, 0, 0, 0};
	uint8_t block_h[DRM_FORMAT_MAX_PLANES] = {1, 0, 0, 0};
	size_t planes = 1;
};

std::optional<FormatInfo> getFormatInfo(uint32_t fourcc);
uint8_t getFormatBlockHeight(const FormatInfo &info, size_t plane);
uint8_t getFormatBlockWidth(const FormatInfo &info, size_t plane);
uint8_t getFormatBpp(const FormatInfo &info, size_t plane);

drm_mode_modeinfo makeModeInfo(const char *name, uint32_t type,
		uint32_t clock, unsigned int hdisplay, unsigned int hsync_start,
		unsigned int hsync_end, unsigned int htotal, unsigned int hskew,
		unsigned int vdisplay, unsigned int vsync_start, unsigned int vsync_end,
		unsigned int vtotal, unsigned int vscan, uint32_t flags);

void addDmtModes(std::vector<drm_mode_modeinfo> &supported_modes,
		unsigned int max_width, unsigned max_height);

// Copies 16-byte aligned buffers. Expected to be faster than plain memcpy().
#if defined(__x86_64__) || defined(__aarch64__)
	extern "C" void fastCopy16(void *, const void *, size_t);
#else
	inline void fastCopy16(void *dst, const void *src, size_t size) {
		memcpy(dst, src, size);
	}
#endif

} //namespace drm_core

