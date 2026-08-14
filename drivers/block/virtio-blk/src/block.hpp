
#include <memory>

#include <blockfs.hpp>
#include <core/virtio/core.hpp>
#include <async/mutex.hpp>
#include <async/oneshot-event.hpp>

namespace block {
namespace virtio {

// --------------------------------------------------------
// VirtIO data structures and constants
// --------------------------------------------------------

struct VirtRequest {
	uint32_t type;
	uint32_t reserved;
	uint64_t sector;
};
static_assert(sizeof(VirtRequest) == 16, "Bad sizeof(VirtRequest)");

enum {
	VIRTIO_BLK_T_IN = 0,
	VIRTIO_BLK_T_OUT = 1
};

namespace spec::regs {
	inline constexpr arch::scalar_register<uint32_t> capacity[] = {
			arch::scalar_register<uint32_t>{0},
			arch::scalar_register<uint32_t>{4}};
}

struct Device;

// --------------------------------------------------------
// UserRequest
// --------------------------------------------------------

struct UserRequest : virtio_core::Request {
	UserRequest(bool write, uint64_t sector, arch::dma_buffer_view view);

	bool write;
	uint64_t sector;
	arch::dma_buffer_view view;

	async::oneshot_primitive event;

	// Timestamps and durations used for ostrace instrumentation.
	uint64_t enqueueTs = 0;
	uint64_t dequeueTs = 0;
	uint64_t submitTs = 0;
	uint64_t completeTs = 0;
	uint64_t obtainTime = 0;
};

// --------------------------------------------------------
// Device
// --------------------------------------------------------

struct Device : blockfs::BlockDevice {
	Device(std::unique_ptr<virtio_core::Transport> transport, int64_t parent_id);

	async::result<void> runDevice();

	async::result<void> readSectors(uint64_t sector, arch::dma_buffer_view view) override;
	async::result<void> writeSectors(uint64_t sector, arch::dma_buffer_view view) override;

	async::result<size_t> getSize() override;

private:
	// Sets up the descriptor chain of the request and posts it to the device.
	// Returns after submission without waiting for the request's completion.
	async::result<void> _submitRequest(UserRequest *request);

	std::unique_ptr<virtio_core::Transport> _transport;

	// The single virtq of this device.
	virtio_core::Queue *_requestQueue;

	// Serializes descriptor acquisition; two requests that interleave partial
	// acquisition could deadlock once the virtq runs out of descriptors.
	async::mutex _submitMutex;

	// these two buffer store virtio-block request header and status bytes
	// they are indexed by the index of the request's first descriptor
	arch::dma_array<VirtRequest> virtRequestBuffer;
	arch::dma_array<uint8_t> statusBuffer;

	// The size of the disk
	size_t _size;
};

} } // namespace block::virtio

