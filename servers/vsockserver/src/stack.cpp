#include <print>
#include <random>
#include <vector>
#include <sys/socket.h>

#include <helix/dispatcher-pool.hpp>

#include "device.hpp"
#include "socket.hpp"
#include "stack.hpp"

namespace vsock {

namespace {

// Like Linux, ephemeral ports are allocated above the privileged ones.
constexpr uint32_t lastReservedPort = 1023;
constexpr int maxPortRetries = 24;

} // anonymous namespace

VsockStack::VsockStack(Device *device)
: device_{device} {
	std::uniform_int_distribution<uint32_t> dist{lastReservedPort + 1, VMADDR_PORT_ANY - 1};
	std::mt19937 prng{std::random_device{}()};
	nextPort_ = dist(prng);
}

std::optional<uint32_t> VsockStack::allocatePort_() {
	for(int i = 0; i < maxPortRetries; i++) {
		if(nextPort_ <= lastReservedPort || nextPort_ == VMADDR_PORT_ANY)
			nextPort_ = lastReservedPort + 1;
		auto port = nextPort_++;
		if(!boundPorts_.contains(port)) {
			boundPorts_.insert(port);
			return port;
		}
	}
	return std::nullopt;
}

void VsockStack::replyRst_(const virtio_vsock_hdr &hdr, TxBuffer reply) {
	virtio_vsock_hdr rst{};
	rst.src_cid = hdr.dst_cid;
	rst.dst_cid = hdr.src_cid;
	rst.src_port = hdr.dst_port;
	rst.dst_port = hdr.src_port;
	rst.type = hdr.type;
	rst.op = VIRTIO_VSOCK_OP_RST;
	device_->send(std::move(reply), rst);
	device_->flushTx();
}

void VsockStack::handlePacket(const virtio_vsock_hdr &hdr, std::span<const char> payload,
		TxBuffer &reply) {
	if(hdr.dst_cid != device_->guestCid()) {
		std::println("vsock: Dropping packet to CID {}", hdr.dst_cid);
		return;
	}

	if(hdr.type != VIRTIO_VSOCK_TYPE_STREAM) {
		if(hdr.op != VIRTIO_VSOCK_OP_RST)
			replyRst_(hdr, std::move(reply));
		return;
	}

	ConnectionKey key{hdr.dst_port, hdr.src_cid, hdr.src_port};
	// The socket whose state changed.
	smarter::shared_ptr<Socket> socket;
	{
		std::lock_guard lock{mutex_};
		if(auto it = connections_.find(key); it != connections_.end()) {
			socket = it->second;
			socket->handlePacket_(hdr, payload, reply);
		}else if(hdr.op == VIRTIO_VSOCK_OP_REQUEST) {
			auto it = listeners_.find(hdr.dst_port);
			if(it != listeners_.end() && it->second->handleRequest_(hdr, reply))
				socket = it->second;
		}
	}

	if(socket) {
		device_->flushTx();
		socket->event_.raise();
	}else if(hdr.op != VIRTIO_VSOCK_OP_RST) {
		replyRst_(hdr, std::move(reply));
	}
}

void VsockStack::handleTransportReset() {
	std::vector<smarter::shared_ptr<Socket>> sockets;
	{
		std::lock_guard lock{mutex_};
		for(auto &[key, socket] : connections_)
			sockets.push_back(std::move(socket));
		connections_.clear();
	}

	for(auto &socket : sockets)
		socket->handleReset_();
}

managarm::fs::Errors VsockStack::serveSocket(helix::UniqueLane ctrlLane,
		helix::UniqueLane ptLane, int type, int flags) {
	if(type != SOCK_STREAM)
		return managarm::fs::Errors::ILLEGAL_ARGUMENT;

	auto socket = Socket::create(this, flags & SOCK_NONBLOCK);
	helix::DispatcherPool::global().detach(
			Socket::serve(std::move(ctrlLane), std::move(ptLane), std::move(socket)));
	return managarm::fs::Errors::SUCCESS;
}

} // namespace vsock
