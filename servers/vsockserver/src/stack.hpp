#pragma once

#include <compare>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <span>

#include <helix/ipc.hpp>
#include <linux/virtio_vsock.h>
#include <smarter.hpp>

#include "fs.bragi.hpp"

namespace vsock {

struct Device;
struct Socket;
struct TxBuffer;

// Demultiplexes packets to sockets and keeps track of bound ports.
//
// Locking: VsockStack::mutex_ may be held while taking Socket::mutex_,
// never the other way around.
struct VsockStack {
	friend struct Socket;

	VsockStack(Device *device);

	VsockStack(const VsockStack &) = delete;

	VsockStack &operator= (const VsockStack &) = delete;

	Device *device() {
		return device_;
	}

	// Called by the device for each received packet, in the order of reception.
	// The reply buffer (of TxClass::reply) is consumed if the packet is answered.
	void handlePacket(const virtio_vsock_hdr &hdr, std::span<const char> payload,
			TxBuffer &reply);

	// Called by the device after the device reset all connections (e.g., due to live migration).
	void handleTransportReset();

	managarm::fs::Errors serveSocket(helix::UniqueLane ctrlLane, helix::UniqueLane ptLane,
			int type, int flags);

private:
	struct ConnectionKey {
		uint32_t localPort;
		uint64_t peerCid;
		uint32_t peerPort;

		auto operator<=> (const ConnectionKey &) const = default;
	};

	// Replies with a RST to a packet that does not belong to any connection.
	void replyRst_(const virtio_vsock_hdr &hdr, TxBuffer reply);

	// Requires mutex_.
	std::optional<uint32_t> allocatePort_();

	Device *device_;

	std::mutex mutex_;

	// Protected by mutex_.
	std::set<uint32_t> boundPorts_;
	// Protected by mutex_.
	std::map<uint32_t, smarter::shared_ptr<Socket>> listeners_;
	// Protected by mutex_.
	std::map<ConnectionKey, smarter::shared_ptr<Socket>> connections_;
	// Protected by mutex_.
	uint32_t nextPort_;
};

} // namespace vsock
