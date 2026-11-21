#include <core/id-allocator.hpp>
#include <format>
#include <netserver/nic.hpp>

#include <algorithm>
#include <cstring>
#include <arch/bit.hpp>
#include <frg/formatting.hpp>
#include <frg/logging.hpp>
#include <net/if.h>
#include <print>

#include "ip/arp.hpp"
#include "ip/ip4.hpp"
#include "raw.hpp"

namespace {

constexpr bool logDiscards = false;

id_allocator<int> _allocator;

std::unordered_map<std::string, id_allocator<int>> prefixedNames_;

} /* namespace */

namespace nic {
uint8_t &MacAddress::operator[](size_t idx) {
	return mac_[idx];
}

const uint8_t &MacAddress::operator[](size_t idx) const {
	return mac_[idx];
}

bool operator==(const MacAddress &l, const MacAddress &r) {
	return std::equal(std::begin(l.mac_), std::end(l.mac_),
		std::begin(r.mac_));
}

bool operator!=(const MacAddress &l, const MacAddress &r) {
	return !operator==(l, r);
}

Link::Link(unsigned int mtu, arch::dma_pool *dmaPool)
: mtu(mtu), min_mtu(mtu), max_mtu(mtu), dmaPool_(dmaPool), index_{_allocator.allocate()} {

}

MacAddress Link::deviceMac() {
	return mac_;
}

arch::dma_pool *Link::dmaPool() {
	return dmaPool_;
}

int Link::index() {
	return index_;
}

/**
 * Sets the name of this interface using the prefix, while allocating the number automatically.
 *
 * Passing a prefix of `wwan` will set the name `wwan0` if not taken, otherwise `wwan1`, etc.
 */
void Link::configureName(std::string prefix) {
	if(prefix == namePrefix_)
		return;

	if(!namePrefix_.empty()) {
		prefixedNames_.at(namePrefix_).free(nameId_);
		nameId_ = -1;
		namePrefix_ = {};
	}

	if(!prefixedNames_.contains(prefix)) {
		id_allocator<int> prefix_alloc{0};
		prefixedNames_.insert({prefix, prefix_alloc});
	}

	nameId_ = prefixedNames_.at(prefix).allocate();
	namePrefix_ = prefix;
}

std::string Link::name() {
	/* if our fallback option of naming using the MAC fails, and no prefix is set, use `ethX` */
	if(namePrefix_.empty() && !mac_)
		configureName("eth");
	else if(namePrefix_ == "eth" && mac_) {
		prefixedNames_.at(namePrefix_).free(nameId_);
		nameId_ = -1;
		namePrefix_ = {};
	} else if (namePrefix_ == "lo") {
		return "lo";
	}

	/* Construct the name from prefix and ID, if available */
	if(!namePrefix_.empty())
		return std::format("{}{}", namePrefix_, nameId_);

	/* otherwise, fall back to naming using the MAC address */
	assert(mac_);
	return std::format("enx{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}", mac_[0], mac_[1], mac_[2], mac_[3], mac_[4], mac_[5]);
}

Link::AllocatedBuffer Link::allocateFrame(size_t size) {
	using namespace arch;
	Link::AllocatedBuffer buf {
		dma_buffer { dmaPool(), size }, {}
	};

	buf.payload = buf.frame;
	return buf;
}

Link::AllocatedBuffer Link::allocateFrame(MacAddress to, EtherType type,
		size_t payloadSize) {
	// default implementation assume an Ethernet II frame
	using namespace arch;
	auto buf = allocateFrame(ethernetHeaderSize + payloadSize);

	uint16_t et = static_cast<uint16_t>(type);
	et = convert_endian<endian::big>(et);
	std::memcpy(buf.frame.data(), to.data(), sizeof(MacAddress));
	std::memcpy(buf.frame.subview(6).data(), deviceMac().data(),
		sizeof(MacAddress));
	std::memcpy(buf.frame.subview(12).data(), &et, sizeof(et));

	buf.payload = buf.frame.subview(ethernetHeaderSize);
	return buf;
}

unsigned int Link::iff_flags() {
	unsigned int flags = 0;

	if(promiscuous_)
		flags |= IFF_PROMISC;
	if(multicast_)
		flags |= IFF_MULTICAST;
	if(all_multicast_)
		flags |= IFF_ALLMULTI;
	if(broadcast_)
		flags |= IFF_BROADCAST;
	if(l1_up_)
		flags |= IFF_LOWER_UP;

	return flags | extra_iff_flags_;
}

void Link::feedPacket(arch::dma_buffer_view frame) {
	if(!rawIp()) {
		if(frame.size() < ethernetHeaderSize) {
			if(logDiscards)
				std::println("netserver: Discarding Ethernet frame smaller than the header");
			return;
		}

		auto capsule = frame.subview(ethernetHeaderSize);
		auto data = reinterpret_cast<uint8_t*>(frame.data());
		uint16_t ethertype = data[12] << 8 | data[13];
		nic::MacAddress dstsrc[2];
		std::memcpy(dstsrc, data, sizeof(dstsrc));

		// Packet sockets observe all frames that the device hands to us.
		raw().feedPacket(frame);

		if(dstsrc[0] != deviceMac() && !isGroupMac(dstsrc[0])) {
			if(logDiscards)
				std::println("netserver: Discarding Ethernet frame with foreign destination MAC");
			return;
		}

		switch (ethertype) {
		case ETHER_TYPE_IP4:
			ip4().feedPacket(dstsrc[0], dstsrc[1], capsule, weak_from_this());
			break;
		case ETHER_TYPE_ARP:
			neigh4().feedArp(dstsrc[0], capsule, weak_from_this());
			break;
		default:
			break;
		}
	} else {
		ip4().feedPacket({}, {}, frame, weak_from_this());
	}
}

namespace {

constexpr bool debugLoopback = false;

class Loopback : public Link {
public:
	Loopback()
	: Link(1500, nullptr) {
		namePrefix_ = "lo";
		raw_ip_ = true;
		extra_iff_flags_ = IFF_LOOPBACK;
	}

	async::result<void> send(const arch::dma_buffer_view view) override {
		arch::dma_buffer buf{nullptr, view.size()};
		memcpy(buf.data(), view.data(), view.size());
		if (debugLoopback)
			std::println("loopback: Sending packet (size {})", view.size());
		queue_.put(std::move(buf));
		co_return;
	}

	// put() only posts this coroutine to its run queue, so send() never re-enters feedPacket().
	async::detached feedFrames() {
		while (true) {
			auto buf = co_await queue_.async_get();
			assert(buf); // Since async_get() is never cancelled.
			if (debugLoopback)
				std::println("loopback: Received packet (size {})", buf->size());
			feedPacket(*buf);
		}
	}

private:
	async::queue<arch::dma_buffer, frg::stl_allocator> queue_;
};

} // namespace

std::shared_ptr<Link> getLoopback() {
	static std::shared_ptr<Loopback> singleton = [] {
		auto loopback = std::make_shared<Loopback>();
		loopback->feedFrames();
		return loopback;
	}();
	return singleton;
}

} // namespace nic
