#pragma once

#include <algorithm>
#include <arch/bit.hpp>
#include <arch/dma_structs.hpp>
#include <helix/ipc.hpp>
#include <map>
#include <unordered_map>
#include <smarter.hpp>
#include <netserver/nic.hpp>
#include <protocols/fs/common.hpp>
#include <set>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <vector>

#include <netserver/nic.hpp>
#include "fs.bragi.hpp"

struct Icmp;
struct Tcp4;
struct Udp4;

enum class IpProto : uint16_t {
	icmp = 1,
	tcp = 6,
	udp = 17,
};

constexpr uint32_t ipv4Broadcast = 0xFFFFFFFF;

inline bool isIpv4Multicast(uint32_t ip) {
	return (ip & 0xF0000000) == 0xE0000000;
}

inline bool isIpv4Loopback(uint32_t ip) {
	return (ip & 0xFF000000) == 0x7F000000;
}

inline bool isIpv4Zeronet(uint32_t ip) {
	return (ip & 0xFF000000) == 0x00000000;
}

struct CidrAddress {
	uint32_t ip;
	uint8_t prefix;

	inline uint32_t mask() const {
		return (uint64_t(0xFFFFFFFF) << (32 - prefix))
			& 0xFFFFFFFF;
	}

	inline bool sameNet(uint32_t other) const {
		return (other & mask()) == (ip & mask());
	}

	friend bool operator<(const CidrAddress &, const CidrAddress &);
};

struct Ip4Router {
	struct Route {
		inline Route(CidrAddress net, std::weak_ptr<nic::Link> link)
			: network(net), link(link) {}

		CidrAddress network;
		std::weak_ptr<nic::Link> link;
		unsigned int mtu = 0;
		uint32_t gateway = 0;
		unsigned int metric = 0;
		uint32_t source = 0;
		uint8_t scope = 0;
		uint8_t type = 0;
		uint8_t protocol = 0;
		uint32_t flags = 0;
		uint8_t family = 0;

		friend auto operator<=>(const Route &, const Route &);
		friend bool operator==(const Route &, const Route &);
	};

	// false if insertion fails
	bool addRoute(Route r);
	// Removes the route that compares equal to r; false if there is none.
	bool deleteEquivalentRoute(const Route &r);
	// Removes the first route that matches pred; false if there is none.
	template<typename Pred>
	bool deleteRoute(Pred pred) {
		auto it = std::ranges::find_if(routes, pred);
		if(it == routes.end())
			return false;
		routes.erase(it);
		return true;
	}
	template<typename Pred>
	size_t deleteRoutes(Pred pred) {
		return std::erase_if(routes, pred);
	}
	std::optional<Route> resolveRoute(uint32_t ip, std::shared_ptr<nic::Link> link = {});

	inline const std::set<Route> &getRoutes() const {
		return routes;
	}
private:
	std::set<Route> routes;
};

enum Ip4Flags {
	ip4FlagMoreFragments = 1 << 0,
	ip4FlagDontFragment = 1 << 1
};

class Ip4Packet {
	arch::dma_buffer buffer_;
public:
	struct Header {
		uint8_t ihl;
		uint8_t tos;
		uint16_t length;

		uint16_t ident;
		uint16_t flags_offset;

		uint8_t ttl;
		uint8_t protocol;
		uint16_t checksum;

		uint32_t source;
		uint32_t destination;

		inline void ensureEndian() {
			auto nendian = [] (auto &x) {
				x = arch::convert_endian<
					arch::endian::big,
					arch::endian::native>(x);
			};
			nendian(length);
			nendian(ident);
			nendian(flags_offset);
			nendian(checksum);
			nendian(source);
			nendian(destination);
		}
	} header;
	static_assert(sizeof(header) == 20, "bad header size");
	arch::dma_buffer_view data;
	std::weak_ptr<nic::Link> link;

	inline arch::dma_buffer_view payload() const {
		return data.subview(header.ihl * 4);
	}

	inline arch::dma_buffer_view header_view() const {
		return data.subview(0, header.ihl * 4);
	}

	// assumes frame is a valid view into owner
	// Without an owner, the packet borrows the frame and is only valid during Ip4::feedPacket().
	bool parse(arch::dma_buffer owner, arch::dma_buffer_view frame, bool resizeData);

	// Everything that keeps a packet beyond Ip4::feedPacket() has to go through this.
	static smarter::shared_ptr<const Ip4Packet> retain(smarter::shared_ptr<const Ip4Packet> packet);
};

struct Ip4TargetInfo {
	uint32_t remote;
	uint32_t source;
	Ip4Router::Route route;
	std::shared_ptr<nic::Link> link;
};

struct Ip4Socket;

struct Ip4 {
	enum class AddressType {
		foreign,
		unicast,
		broadcast,
		multicast
	};

	Ip4();

	managarm::fs::Errors serveSocket(helix::UniqueLane ctrlLane, helix::UniqueLane ptLane, int type, int proto, int flags);
	// frame is borrowed for the duration of the call, stripping away eth bits
	void feedPacket(nic::MacAddress dest, nic::MacAddress src,
		arch::dma_buffer_view frame, std::weak_ptr<nic::Link> link);

	bool hasIp(uint32_t ip);
	AddressType classifyAddress(uint32_t ip);
	std::shared_ptr<nic::Link> getLink(uint32_t ip);
	std::optional<CidrAddress> getCidrByIndex(int index);
	bool deleteLink(CidrAddress addr);
	void setLink(CidrAddress addr, std::weak_ptr<nic::Link> link);

	inline const std::map<CidrAddress, std::weak_ptr<nic::Link>> &getAddresses() const {
		return ips;
	}
	std::optional<uint32_t> findLinkIp(uint32_t ipOnNet, nic::Link *link);

	async::result<std::optional<Ip4TargetInfo>> targetByRemote(uint32_t, std::shared_ptr<nic::Link> link = {});
	async::result<protocols::fs::Error> sendFrame(Ip4TargetInfo,
		void*, size_t,
		uint16_t);
private:
	struct FragmentedPacket {
		struct Fragment {
			uint32_t size;
		};

		std::vector<char> data;
		std::unordered_map<uint32_t, Fragment> fragments;
		uint64_t packetReceivedTimerTick;
		bool lastReceived;
	};

	struct FragmentRouteIdentification {
		uint32_t sourceIp;
		uint32_t destIp;
		uint8_t protocol;

		bool operator<(const FragmentRouteIdentification &rhs) const;
	};

	struct FragmentRouteInfo {
		std::unordered_map<uint16_t, FragmentedPacket> receivedPackets;
		uint16_t sendIdent;
		uint64_t lastAccessedTimerTick;
	};

	FragmentRouteInfo *getOrCreateFragmentRoute_(const FragmentRouteIdentification &ident);

	async::detached fragmentTimer_();

	// TODO: Use a CSPRNG, see also TCP/UDP.
	std::mt19937 fragmentIdentPrng;
	uint64_t fragmentTimerTick = 0;

	std::multimap<int, smarter::shared_ptr<Ip4Socket>> sockets;
	std::map<CidrAddress, std::weak_ptr<nic::Link>> ips;
	std::map<FragmentRouteIdentification, FragmentRouteInfo> fragmentRoutes;

	std::unique_ptr<Icmp> icmp;
	std::unique_ptr<Tcp4> tcp;
	std::unique_ptr<Udp4> udp;
};

Ip4 &ip4();
Ip4Router &ip4Router();
