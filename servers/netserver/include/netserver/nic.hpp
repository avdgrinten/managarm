#pragma once

#include <array>
#include <arch/dma_pool.hpp>
#include <async/result.hpp>
#include <frg/logging.hpp>
#include <frg/formatting.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <format>
#include <ostream>
#include <print>
#include <protocols/mbus/client.hpp>
#include <unordered_map>

namespace nic {
struct MacAddress {
	MacAddress() = default;
	explicit MacAddress(std::array<uint8_t, 6> data) : mac_{data} {}

	uint8_t &operator[](size_t idx);
	const uint8_t &operator[](size_t idx) const;
	inline uint8_t *data() {
		return mac_.data();
	}
	inline const uint8_t *data() const {
		return mac_.data();
	}

	friend bool operator==(const MacAddress &l, const MacAddress &r);
	friend bool operator!=(const MacAddress &l, const MacAddress &r);

	explicit operator bool() const {
		return !(mac_[0] == 0 && mac_[1] == 0 && mac_[2] == 0 && mac_[3] == 0 && mac_[4] == 0 && mac_[5] == 0);
	}

	inline friend uint8_t *begin(MacAddress &m) {
		return m.mac_.begin();
	}

	inline friend uint8_t *end(MacAddress &m) {
		return m.mac_.end();
	}
private:
	std::array<uint8_t, 6> mac_ = {};
};

enum EtherType : uint16_t {
	ETHER_TYPE_IP4 = 0x0800,
	ETHER_TYPE_ARP = 0x0806,
};

constexpr size_t ethernetHeaderSize = 14;

// The I/G bit of the first octet marks group (i.e. multicast and broadcast) addresses.
inline bool isGroupMac(const MacAddress &mac) {
	return mac[0] & 0x01;
}

// TODO(arsen): Expose interface for csum offloading, constructing frames, and
// other features of NICs
struct Link : std::enable_shared_from_this<Link> {
	struct AllocatedBuffer {
		arch::dma_buffer frame;
		arch::dma_buffer_view payload;
	};

	Link(unsigned int mtu, arch::dma_pool *dmaPool);
	virtual ~Link() = default;
	//! Receives an entire frame from the network
	virtual async::result<size_t> receive(arch::dma_buffer_view) = 0;
	//! Sends an entire ethernet frame
	virtual async::result<void> send(const arch::dma_buffer_view) = 0;
	//! Called by the driver for each received frame, in order.
	//! The frame is borrowed for the duration of the call; the driver reposts it afterwards.
	void feedPacket(arch::dma_buffer_view frame);
	arch::dma_pool *dmaPool();
	AllocatedBuffer allocateFrame(size_t payloadSize);
	AllocatedBuffer allocateFrame(MacAddress to, EtherType type,
		size_t payloadSize);

	MacAddress deviceMac();
	int index();
	void configureName(std::string prefix);
	std::string name();
	unsigned int mtu;
	unsigned int min_mtu;
	unsigned int max_mtu;
	unsigned int iff_flags();

	bool rawIp() {
		return raw_ip_;
	}

	mbus_ng::Properties mbusNetworkProperties() {
		return {
			{"net.ifname", mbus_ng::StringItem{name()}},
			{"net.ifindex", mbus_ng::StringItem{std::to_string(index())}},
		};
	}

	static std::shared_ptr<Link> byIndex(int index);
	static std::shared_ptr<Link> byName(std::string name);

	static std::unordered_map<int64_t, std::shared_ptr<nic::Link>> &getLinks();
protected:
	arch::dma_pool *dmaPool_;
	MacAddress mac_;
	int index_;
	std::string namePrefix_;
	int nameId_ = -1;

	bool promiscuous_ = false;
	bool multicast_ = false;
	bool all_multicast_ = false;
	bool broadcast_ = false;
	bool l1_up_ = false;

	int extra_iff_flags_ = 0;

	bool raw_ip_ = false;
};

std::shared_ptr<Link> getLoopback();

async::detached runDevice(std::shared_ptr<Link> dev);
} // namespace nic

inline std::ostream &operator<<(std::ostream &os, const nic::MacAddress &mac) {
    frg::to(os) << frg::fmt("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return os;
}

template<>
struct std::formatter<nic::MacAddress, char> {
	template<class Ctx>
	constexpr Ctx::iterator parse(Ctx &ctx) {
		return ctx.begin();
	}

	template<class Ctx>
	Ctx::iterator format(const nic::MacAddress &mac, Ctx &ctx) const {
		return std::format_to(ctx.out(), "{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
				mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	}
};
