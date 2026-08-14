#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <memory>
#include <vector>

#include <arch/dma_pool.hpp>
#include <arch/dma_structs.hpp>
#ifdef __x86_64__
#include <arch/io_space.hpp>
#endif
#include <arch/mem_space.hpp>
#include <arch/register.hpp>
#include <arch/variable.hpp>
#include <async/recurring-event.hpp>
#include <async/oneshot-event.hpp>
#include <helix/ipc.hpp>
#include <protocols/hw/client.hpp>

namespace virtio_core {

// --------------------------------------------------------
// VirtIO data structures and constants
// --------------------------------------------------------

inline constexpr arch::scalar_register<uint32_t> PCI_L_DEVICE_FEATURES(0);
inline constexpr arch::scalar_register<uint32_t> PCI_L_DRIVER_FEATURES(4);
inline constexpr arch::scalar_register<uint32_t> PCI_L_QUEUE_ADDRESS(8);
inline constexpr arch::scalar_register<uint16_t> PCI_L_QUEUE_SIZE(12);
inline constexpr arch::scalar_register<uint16_t> PCI_L_QUEUE_SELECT(14);
inline constexpr arch::scalar_register<uint16_t> PCI_L_QUEUE_NOTIFY(16);
inline constexpr arch::scalar_register<uint8_t> PCI_L_DEVICE_STATUS(18);
inline constexpr arch::scalar_register<uint8_t> PCI_L_ISR_STATUS(19);

inline constexpr arch::scalar_register<uint32_t> PCI_DEVICE_FEATURE_SELECT(0);
inline constexpr arch::scalar_register<uint32_t> PCI_DEVICE_FEATURE_WINDOW(4);
inline constexpr arch::scalar_register<uint32_t> PCI_DRIVER_FEATURE_SELECT(8);
inline constexpr arch::scalar_register<uint32_t> PCI_DRIVER_FEATURE_WINDOW(12);
inline constexpr arch::scalar_register<uint8_t> PCI_DEVICE_STATUS(20);
inline constexpr arch::scalar_register<uint16_t> PCI_QUEUE_SELECT(22);
inline constexpr arch::scalar_register<uint16_t> PCI_QUEUE_SIZE(24);
inline constexpr arch::scalar_register<uint16_t> PCI_QUEUE_MSIX_VECTOR(26);
inline constexpr arch::scalar_register<uint16_t> PCI_QUEUE_ENABLE(28);
inline constexpr arch::scalar_register<uint16_t> PCI_QUEUE_NOTIFY(30);
inline constexpr arch::scalar_register<uint32_t> PCI_QUEUE_TABLE[] = {
		arch::scalar_register<uint32_t>{32},
		arch::scalar_register<uint32_t>{36}};
inline constexpr arch::scalar_register<uint32_t> PCI_QUEUE_AVAILABLE[] = {
		arch::scalar_register<uint32_t>{40},
		arch::scalar_register<uint32_t>{44}};
inline constexpr arch::scalar_register<uint32_t> PCI_QUEUE_USED[] = {
		arch::scalar_register<uint32_t>{48},
		arch::scalar_register<uint32_t>{52}};

inline constexpr arch::scalar_register<uint8_t> PCI_ISR(0);

enum {
	PCI_L_DEVICE_SPECIFIC = 20
};

// bits of the device status register
enum {
	ACKNOWLEDGE = 1,
	DRIVER = 2,
	FEATURES_OK = 8,
	DRIVER_OK = 4,
	DEVICE_NEEDS_RESET = 64
};

enum {
	// Bits of the spec::Descriptor::flags field.
	VIRTQ_DESC_F_NEXT = 1, // descriptor is part of a chain
	VIRTQ_DESC_F_WRITE = 2, // buffer is written by device

	// Bits of the spec::UsedRing::flags field.
	VIRTQ_USED_F_NO_NOTIFY = 1 // no need to notify the device
};

namespace spec {
	struct Descriptor {
		arch::scalar_variable<uint64_t> address;
		arch::scalar_variable<uint32_t> length;
		arch::scalar_variable<uint16_t> flags;
		arch::scalar_variable<uint16_t> next;
	};
	static_assert(sizeof(Descriptor) == 16);

	struct AvailableRing {
		arch::scalar_variable<uint16_t> flags;
		arch::scalar_variable<uint16_t> headIndex;

		struct Element {
			arch::scalar_variable<uint16_t> tableIndex;
		} elements[];
	};
	static_assert(sizeof(AvailableRing) == 4);

	struct AvailableExtra {
		static AvailableExtra *get(AvailableRing *ring, size_t queue_size) {
			return reinterpret_cast<AvailableExtra *>(ring->elements + queue_size);
		}

		arch::scalar_variable<uint16_t> eventIndex;
	};

	struct UsedRing {
		arch::scalar_variable<uint16_t> flags;
		arch::scalar_variable<uint16_t> headIndex;

		struct Element {
			arch::scalar_variable<uint32_t> tableIndex;
			arch::scalar_variable<uint32_t> written;
		} elements[];
	};
	static_assert(sizeof(UsedRing) == 4);

	struct UsedExtra {
		static UsedExtra *get(UsedRing *ring, size_t queue_size) {
			return reinterpret_cast<UsedExtra *>(ring->elements + queue_size);
		}

		arch::scalar_variable<uint16_t> eventIndex;
	};
};

struct DeviceSpace;
struct Queue;

// --------------------------------------------------------
// Transport
// --------------------------------------------------------

struct QueueInfo {
	size_t queueSize;
	ptrdiff_t notifyOffset;
};

/* This class represents a virtio device.
 *
 * Usual initialization works as follows:
 * - Call discover() to obtain a transport.
 * - Negotiate features via Transport::checkDeviceFeature() / acknowledgeDriverFeature().
 * - Call Transport::finalizeFeatures().
 * - Call Transport::claimQueues().
 * - Call Transport::setupQueue() for each virtq.
 * - Call Transport::runDevice().
 */
struct Transport {
	DeviceSpace space();

	Transport(helix::UniqueDescriptor dmaSpace, bool iommuActive)
	: dmaSpaceHandle_{std::move(dmaSpace)},
	  dmaSpace_{memoryPool_.attachDmaSpace(dmaSpaceHandle_, iommuActive)},
	  contiguousDmaSpace_{contiguousPool_.attachDmaSpace(dmaSpaceHandle_, iommuActive)} {}

	virtual ~Transport() = default;

	virtual bool isLegacy() = 0;

	virtual protocols::hw::Device &hwDevice() = 0;

	virtual uint8_t loadConfig8(size_t offset) = 0;
	virtual uint16_t loadConfig16(size_t offset) = 0;
	virtual uint32_t loadConfig32(size_t offset) = 0;

	virtual bool checkDeviceFeature(unsigned int feature) = 0;
	virtual void acknowledgeDriverFeature(unsigned int feature) = 0;
	virtual void finalizeFeatures() = 0;

	virtual void claimQueues(unsigned int max_index) = 0;

	virtual async::result<Queue *> setupQueue(unsigned int index) = 0;

	virtual void runDevice() = 0;

	arch::contiguous_pool memoryPool_{{.addressBits = 64, .allocateContigous = false}};
	arch::contiguous_pool contiguousPool_{{.addressBits = 64, .allocateContigous = true}};
	helix::UniqueDescriptor dmaSpaceHandle_;
	arch::dma_space dmaSpace_;
	arch::dma_space contiguousDmaSpace_;
};

struct DeviceSpace {
	DeviceSpace(Transport *transport)
	: _transport{transport} { }

	template<typename RT>
	typename RT::rep_type load(RT r) const {
		if constexpr (sizeof(typename RT::rep_type) == 1) {
			auto v = _transport->loadConfig8(r.offset());
			return static_cast<typename RT::rep_type>(v);
		} else if constexpr (sizeof(typename RT::rep_type) == 2) {
			auto v = _transport->loadConfig16(r.offset());
			return static_cast<typename RT::rep_type>(v);
		} else {
			static_assert(sizeof(typename RT::rep_type) == 4,
					"Unsupported size for DeviceSpace::load()");
			auto v = _transport->loadConfig32(r.offset());
			return static_cast<typename RT::rep_type>(v);
		}
	}

private:
	Transport *_transport;
};

inline DeviceSpace Transport::space() {
	return DeviceSpace{this};
}

enum class DiscoverMode {
	null,
	legacyOnly,
	transitional,
	modernOnly
};

async::result<std::unique_ptr<Transport>> discover(protocols::hw::Device hw_device,
		DiscoverMode mode);

// --------------------------------------------------------
// Queue
// --------------------------------------------------------

struct HostToDeviceType { };
struct DeviceToHostType { };

inline constexpr HostToDeviceType hostToDevice;
inline constexpr DeviceToHostType deviceToHost;

// Handle to a virtq descriptor.
struct Handle {
	Handle()
	: _queue{nullptr}, _tableIndex{0} { }

	Handle(Queue *queue, size_t table_index);

	explicit operator bool() {
		return _queue;
	}

	size_t tableIndex() {
		return _tableIndex;
	}

	// setupBuffer() assumes that the buffer is contiguous in physical memory.
	// Use scatterGather() for a more convenient API.
	async::result<void> setupBuffer(HostToDeviceType, arch::dma_buffer_view view);
	async::result<void> setupBuffer(DeviceToHostType, arch::dma_buffer_view view);

	void setupLink(Handle other);

private:
	Queue *_queue;
	size_t _tableIndex;
};

// Helper class to create Handle chains.
struct Chain {
	Chain() = default;

	Chain(const Chain &) = delete;

	Chain &operator= (const Chain &) = delete;

	void append(Handle handle) {
		if(_front) {
			_back.setupLink(handle);
			_back = handle;
		}else{
			_front = handle;
			_back = handle;
		}
	}

	Handle front() {
		return _front;
	}

	// Note the remarks on Handle::setupBuffer().
	async::result<void> setupBuffer(HostToDeviceType, arch::dma_buffer_view view) {
		return _back.setupBuffer(hostToDevice, view);
	}
	async::result<void> setupBuffer(DeviceToHostType, arch::dma_buffer_view view) {
		return _back.setupBuffer(deviceToHost, view);
	}

private:
	Handle _front;
	Handle _back;
};

// Helper functions that obtain descriptor from a queue as needed.
async::result<void> scatterGather(HostToDeviceType, Chain &chain, Queue *queue,
		arch::dma_buffer_view view);
async::result<void> scatterGather(DeviceToHostType, Chain &chain, Queue *queue,
		arch::dma_buffer_view view);

struct Request {
	void (*complete)(Request *);

	size_t len = 0;
};

// Represents a single virtq.
struct Queue {
	friend struct Handle;

	Queue(unsigned int queue_index, size_t queue_size, arch::dma_buffer virtq, spec::Descriptor *table,
			spec::AvailableRing *available, spec::UsedRing *used);
protected:
	~Queue() = default;

public:
	unsigned int queueIndex() {
		return _queueIndex;
	}

	// Returns the number of descriptors in this virtq.
	size_t numDescriptors() {
		return _queueSize;
	}

	// Returns the number of descriptors that are currently unused.
	size_t numFreeDescriptors() {
		return _descriptorStack.size();
	}

	// Allocates a single descriptor.
	// The descriptor is automatically freed when the device returns it.
	async::result<Handle> obtainDescriptor();

	// Posts a descriptor to the virtq's available ring.
	void postDescriptor(Handle descriptor, Request *request,
			void (*complete)(Request *));

	// Notifies the device that new descriptors have been posted.
	void notify();

	async::result<size_t> submitDescriptor(Handle descriptor) {
		struct OneshotRequest : Request {
			async::oneshot_primitive event;
		} ev_req;

		postDescriptor(descriptor, &ev_req,
				[] (virtio_core::Request *base_request) {
			auto request = static_cast<OneshotRequest *>(base_request);
			request->event.raise();
		});
		notify();

		co_await ev_req.event.wait();
		co_return ev_req.len;
	}

	// Processes interrupts for this virtq.
	// Calls retrieveDescriptor() to complete individual requests.
	void processInterrupt();

protected:
	virtual void notifyTransport() = 0;
	virtual arch::dma_space &dmaSpace() = 0;

private:
	// Index of this queue as part of its owning device.
	unsigned int _queueIndex;

	// Number of descriptors in this queue.
	size_t _queueSize;

	arch::dma_buffer virtq_;

	// Pointers to different data structures of this virtq.
	spec::Descriptor *_table;
	spec::AvailableRing *_availableRing;
	spec::UsedRing *_usedRing;
	spec::AvailableExtra *_availableExtra;
	spec::UsedExtra *_usedExtra;

	// Keeps track of unused descriptor indices.
	std::vector<uint16_t> _descriptorStack;

	async::recurring_event _descriptorDoorbell;

	std::vector<Request *> _activeRequests;

	// Keeps track of which entries in the used ring have already been processed.
	uint16_t _progressHead;
};

} // namespace virtio_core

