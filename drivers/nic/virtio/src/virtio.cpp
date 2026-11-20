#include <nic/virtio/virtio.hpp>

#include <algorithm>
#include <arch/dma_pool.hpp>
#include <async/recurring-event.hpp>
#include <core/virtio/core.hpp>
#include <frg/list.hpp>
#include <memory>
#include <print>
#include <vector>

namespace {
	constexpr bool logFrames = false;

	// Bounds the RX buffers to 128 * ~1.5 KiB.
	constexpr size_t maxRxBuffers = 128;
}

namespace {
// Device feature bits.
enum {
	VIRTIO_NET_F_MAC = 5
};

// Bits for VirtHeader::flags.
enum {
	VIRTIO_NET_HDR_F_NEEDS_CSUM = 1
};

// Values for VirtHeader::gsoType.
enum {
	VIRTIO_NET_HDR_GSO_NONE = 0,
	VIRTIO_NET_HDR_GSO_TCPV4 = 1,
	VIRTIO_NET_HDR_GSO_UDP = 2,
	VIRTIO_NET_HDR_GSO_TCPV6 = 3,
	VIRTIO_NET_HDR_GSO_ECN = 0x80
};

struct VirtHeader {
	uint8_t flags;
	uint8_t gsoType;
	uint16_t hdrLen;
	uint16_t gsoSize;
	uint16_t csumStart;
	uint16_t csumOffset;
	uint16_t numBuffers;
};

struct VirtioNic : nic::Link {
	VirtioNic(mbus_ng::EntityId entity, std::unique_ptr<virtio_core::Transport> transport);
	async::result<void> initialize();

	async::result<void> send(const arch::dma_buffer_view) override;

	~VirtioNic() override = default;
private:
	// Holds the virtio-net header followed by the frame.
	struct RxBuffer : virtio_core::Request {
		VirtioNic *nic;
		arch::dma_buffer buffer;
		// Header and frame are separate descriptors, as legacy devices without
		// VIRTIO_F_ANY_LAYOUT require.
		std::vector<virtio_core::DmaChunk> chunks;
		std::vector<virtio_core::Handle> handles;
		// Links the buffer into rxDone_.
		frg::default_list_hook<RxBuffer> hook;
	};

	async::result<void> postRx_(RxBuffer *rx);
	async::detached processRx_();

	mbus_ng::EntityId entity_;
	std::unique_ptr<virtio_core::Transport> transport_;
	virtio_core::Queue *receiveVq_;
	virtio_core::Queue *transmitVq_;
	size_t headerSize_;

	std::vector<std::unique_ptr<RxBuffer>> rxBuffers_;
	// Completed RX buffers in the order in which the device used them.
	frg::intrusive_list<
		RxBuffer,
		frg::locate_member<RxBuffer, frg::default_list_hook<RxBuffer>, &RxBuffer::hook>
	> rxDone_;
	async::recurring_event rxEvent_;
};

VirtioNic::VirtioNic(mbus_ng::EntityId entity, std::unique_ptr<virtio_core::Transport> transport)
: nic::Link(1500, &transport->memoryPool_),
  entity_{entity},
  transport_{std::move(transport)} {
	headerSize_ = transport_->isLegacy() ? 10 : sizeof(VirtHeader);

	if(transport_->checkDeviceFeature(VIRTIO_NET_F_MAC)) {
		for (int i = 0; i < 6; i++) {
			mac_[i] = transport_->loadConfig8(i);
		}
		char ms[3 * 6 + 1];
		sprintf(ms, "%.2x:%.2x:%.2x:%.2x:%.2x:%.2x",
				mac_[0], mac_[1], mac_[2],
				mac_[3], mac_[4], mac_[5]);
		std::cout << "virtio-driver: Device has a hardware MAC: "
			<< ms << std::endl;
		transport_->acknowledgeDriverFeature(VIRTIO_NET_F_MAC);
	}
}

async::result<void> VirtioNic::initialize() {
	transport_->finalizeFeatures();
	transport_->claimQueues(2);
	receiveVq_ = co_await transport_->setupQueue(0);
	transmitVq_ = co_await transport_->setupQueue(1);

	// This driver runs on a single thread, so both queues share one interrupt.
	async::detach(receiveVq_->serviceQueue());
	async::detach(transmitVq_->serviceQueue());

	promiscuous_ = true;
	all_multicast_ = true;
	multicast_ = true;
	broadcast_ = true;
	l1_up_ = true;

	transport_->runDevice();

	// Keep the RX queue filled; buffers are reposted once their frame was fed to the stack.
	auto &space = receiveVq_->dmaSpace();
	size_t numRxBuffers = std::min(receiveVq_->numDescriptors() / 2, maxRxBuffers);
	for(size_t i = 0; i < numRxBuffers; i++) {
		auto rx = std::make_unique<RxBuffer>();
		rx->nic = this;
		rx->buffer = arch::dma_buffer{&transport_->memoryPool_, headerSize_ + 1514};
		co_await space.ensure_mapped(rx->buffer);
		rx->chunks = receiveVq_->splitContiguous(rx->buffer.subview(0, headerSize_));
		auto frameChunks = receiveVq_->splitContiguous(rx->buffer.subview(headerSize_));
		rx->chunks.insert(rx->chunks.end(), frameChunks.begin(), frameChunks.end());
		rx->handles.resize(rx->chunks.size());
		rxBuffers_.push_back(std::move(rx));
	}
	for(auto &rx : rxBuffers_)
		co_await postRx_(rx.get());
	processRx_();

	mbus_ng::Properties netProperties{
		{"drvcore.mbus-parent", mbus_ng::StringItem{std::to_string(entity_)}},
		{"unix.subsystem", mbus_ng::StringItem{"net"}},
	};
	netProperties.merge(mbusNetworkProperties());

	auto netClassEntity = (co_await mbus_ng::Instance::global().createEntity(
		"net", netProperties)).unwrap();

	[] (mbus_ng::EntityManager entity) -> async::detached {
		while (true) {
			auto [localLane, remoteLane] = helix::createStream();

			// If this fails, too bad!
			(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));
		}
	}(std::move(netClassEntity));
}

async::result<void> VirtioNic::postRx_(RxBuffer *rx) {
	co_await receiveVq_->obtainDescriptors(rx->handles);

	for(size_t i = 1; i < rx->handles.size(); i++)
		rx->handles[i - 1].setupLink(rx->handles[i]);
	for(size_t i = 0; i < rx->chunks.size(); i++)
		rx->handles[i].setupBuffer(virtio_core::deviceToHost, rx->chunks[i]);

	receiveVq_->postDescriptor(rx->handles.front(), rx, [] (virtio_core::Request *base) {
		auto rx = static_cast<RxBuffer *>(base);
		rx->nic->rxDone_.push_back(rx);
		rx->nic->rxEvent_.raise();
	});
	receiveVq_->notify();
}

async::detached VirtioNic::processRx_() {
	while(true) {
		RxBuffer *rx = nullptr;
		co_await rxEvent_.async_wait_if([&] {
			if(rxDone_.empty())
				return true;
			rx = rxDone_.pop_front();
			return false;
		});
		if(!rx)
			continue;

		if(rx->len < headerSize_ || rx->len > rx->buffer.size()) {
			std::println("virtio-driver: Dropping RX buffer with a used length of {}", rx->len);
		}else{
			feedPacket(rx->buffer.subview(headerSize_, rx->len - headerSize_));
		}

		co_await postRx_(rx);
	}
}

async::result<void> VirtioNic::send(const arch::dma_buffer_view payload) {
	if (payload.size() > 1514) {
		throw std::runtime_error("data exceeds mtu");
	}

	arch::dma_object<VirtHeader> header { &transport_->memoryPool_ };
	memset(header.data(), 0, sizeof(VirtHeader));

	arch::dma_buffer_view payloadView{payload};

	auto &space = transmitVq_->dmaSpace();
	co_await space.ensure_mapped(header.view_buffer());
	co_await space.ensure_mapped(payloadView);

	virtio_core::Chain chain;
	chain.append(co_await transmitVq_->obtainDescriptor());
	chain.setupBuffer(virtio_core::hostToDevice, header.view_buffer().subview(0, headerSize_));
	chain.append(co_await transmitVq_->obtainDescriptor());
	chain.setupBuffer(virtio_core::hostToDevice, payloadView);

	if(logFrames) {
		std::cout << "virtio-driver: sending frame" << std::endl;
	}
	co_await transmitVq_->submitDescriptor(chain.front());
	if(logFrames) {
		std::cout << "virtio-driver: sent frame" << std::endl;
	}
}
} // namespace

namespace nic::virtio {

async::result<std::shared_ptr<nic::Link>> makeShared(mbus_ng::EntityId entity,
		std::unique_ptr<virtio_core::Transport> transport) {
	auto nic = std::make_shared<VirtioNic>(entity, std::move(transport));
	co_await nic->initialize();

	co_return nic;
}

} // namespace nic::virtio
