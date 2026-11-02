
#include <atomic>
#include <memory>
#include <vector>

#include <blockfs.hpp>
#include <core/virtio/core.hpp>
#include <async/oneshot-event.hpp>

namespace block {
namespace virtio {

// --------------------------------------------------------
// VirtIO data structures and constants
// --------------------------------------------------------

// Natural align to guarantee that we do not cross page boundaries.
struct alignas(16) VirtRequest {
	uint32_t type;
	uint32_t reserved;
	uint64_t sector;
};
static_assert(sizeof(VirtRequest) == 16, "Bad sizeof(VirtRequest)");

enum {
	VIRTIO_BLK_T_IN = 0,
	VIRTIO_BLK_T_OUT = 1,
	VIRTIO_BLK_T_FLUSH = 4,
};

enum {
	VIRTIO_BLK_F_FLUSH = 9,
	VIRTIO_BLK_F_MQ = 12,
};

enum {
	VIRTIO_BLK_S_OK = 0,
	VIRTIO_BLK_S_IOERR = 1,
	VIRTIO_BLK_S_UNSUPP = 2
};

namespace spec::regs {
	inline constexpr arch::scalar_register<uint32_t> capacity[] = {
			arch::scalar_register<uint32_t>{0},
			arch::scalar_register<uint32_t>{4}};
	inline constexpr arch::scalar_register<uint16_t> numQueues{34};
}

struct Device;

// --------------------------------------------------------
// UserRequest
// --------------------------------------------------------

struct UserRequest : virtio_core::Request {
	UserRequest(bool write, uint64_t sector, arch::dma_buffer_view view,
			arch::dma_pool *pool);

	bool write;
	uint64_t sector;
	arch::dma_buffer_view view;

	// Request header and status byte of this request.
	arch::dma_object<VirtRequest> header;
	arch::dma_object<uint8_t> status;

	async::oneshot_primitive event;

	// Timestamps and durations used for ostrace instrumentation.
	uint64_t enqueueTs = 0;
	uint64_t submitTs = 0;
	uint64_t completeTs = 0;
	uint64_t obtainTime = 0;
	uint64_t setupTime = 0;
};

// --------------------------------------------------------
// Device
// --------------------------------------------------------

struct Device : blockfs::BlockDevice {
	Device(std::unique_ptr<virtio_core::Transport> transport, int64_t parent_id);

	async::result<void> runDevice();

	async::result<std::expected<void, blockfs::Error>> flush() override;

	async::result<size_t> getSize() override;

protected:
	async::result<void> prepareIo(unsigned int queueIndex, arch::dma_buffer_view view) override;
	size_t issueCost(unsigned int queueIndex, arch::dma_buffer_view view) override;
	async::result<std::expected<void, blockfs::Error>> issueRead(unsigned int queueIndex, uint64_t sector, arch::dma_buffer_view view) override;
	async::result<std::expected<void, blockfs::Error>> issueWrite(unsigned int queueIndex, uint64_t sector, arch::dma_buffer_view view) override;

private:
	// Returns the given virtq after starting its completion servicer if necessary.
	virtio_core::Queue *_useQueue(unsigned int index);

	// Starts the completion servicer of a queue on the calling thread, at most once.
	void _startServicer(unsigned int index);

	// Sets up the descriptor chain of the request and posts it to the given queue.
	// Returns after submission without waiting for the request's completion.
	async::result<void> _issueRequest(virtio_core::Queue *queue, UserRequest *request);

	std::unique_ptr<virtio_core::Transport> _transport;

	// The request virtqs of this device.
	std::vector<virtio_core::Queue *> _queues;

	// Whether the completion servicer of each virtq was already started.
	std::unique_ptr<std::atomic_flag[]> _queueLive;

	// The size of the disk
	size_t _size;

	// Whether the device supports VIRTIO_BLK_T_FLUSH.
	bool _hasFlush = false;
};

} } // namespace block::virtio

