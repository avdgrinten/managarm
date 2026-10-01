#include "arp.hpp"
#include "checksum.hpp"
#include "icmp.hpp"
#include "ip4.hpp"
#include "tcp4.hpp"
#include "udp4.hpp"
#include <async/recurring-event.hpp>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <print>
#include <protocols/fs/server.hpp>
#include <helix/timer.hpp>
#include <queue>

using namespace protocols::fs;

using Route = Ip4Router::Route;

namespace {

constexpr bool logDiscards = false;

constexpr uint64_t fragmentExpiryTimeoutNs = uint64_t{30} * 1'000'000'000;
constexpr uint64_t fragmentRouteExpiryTimeoutTicks = 4;

} // anonymous namespace

Ip4Router &ip4Router() {
	static Ip4Router inst;
	return inst;
}

Ip4 &ip4() {
	static Ip4 inst;
	return inst;
}

bool Ip4Router::addRoute(Route r) {
	return routes.emplace(std::move(r)).second;
}

std::optional<Route> Ip4Router::resolveRoute(uint32_t ip, std::shared_ptr<nic::Link> link) {
	for (auto i = routes.begin(); i != routes.end(); i++) {
		const auto &r = *i;
		if (r.network.sameNet(ip)) {
			if(link && r.link.lock()->index() != link->index())
				continue;

			if (r.link.expired()) {
				i = routes.erase(i);
				continue;
			}
			return { r };
		}
	}
	return {};
}

bool operator<(const CidrAddress &lhs, const CidrAddress &rhs) {
	// We prefer the more specific network, i.e. the higher-valued prefix
	return std::tie(rhs.prefix, lhs.ip) < std::tie(lhs.prefix, rhs.ip);
}

auto operator<=>(const Route &lhs, const Route &rhs) {
	// This determines route sorting, where we order by the following criteria:
	// - more specific network
	// - lower metric (== cost)
	// - higher `RT_SCOPE_*` value (so that we prefer blackholes -> host -> link -> universe etc.)
	// - bigger MTU is better, and hence sorts lower
	return std::tie(lhs.network, lhs.metric, rhs.scope, rhs.mtu, lhs.type,
					lhs.family, lhs.gateway, lhs.source, lhs.flags) <=>
		std::tie(rhs.network, rhs.metric, lhs.scope, lhs.mtu, rhs.type, rhs.family,
				rhs.gateway, rhs.source, rhs.flags);
}

bool operator==(const Route &lhs, const Route &rhs) {
	if(!lhs.link.expired() && !rhs.link.expired()) {
		if(lhs.link.lock()->index() != rhs.link.lock()->index()) {
			return false;
		}
	} else if(lhs.link.expired() ^ rhs.link.expired()) {
		return false;
	}

	return operator<=>(lhs, rhs) == 0;
}

bool Ip4::FragmentRouteIdentification::operator<(const FragmentRouteIdentification &rhs) const {
	return std::tie(sourceIp, destIp, protocol) < std::tie(rhs.sourceIp, rhs.destIp, rhs.protocol);
}

bool Ip4Packet::parse(arch::dma_buffer owner, arch::dma_buffer_view frame, bool resizeData) {
	buffer_ = std::move(owner);
	data = frame;
	if (data.size() < sizeof(header)) {
		if (logDiscards)
			std::println("netserver: Discarding IPv4 packet smaller than the header");
		return false;
	}
	std::memcpy(&header, data.byte_data(), sizeof(header));
	// 0x40: top four bits are version (4)
	if ((header.ihl & 0xf0) != 0x40) {
		if (logDiscards)
			std::println("netserver: Discarding IP packet with a version other than 4");
		return false;
	}

	header.ensureEndian();

	header.ihl = header.ihl & 0x0f;

	// The IHL counts 32-bit words and has to cover at least the fixed part of the header.
	if (header.ihl < 5) {
		if (logDiscards)
			std::println("netserver: Discarding IPv4 packet with an IHL below 5");
		return false;
	}

	if (header.length < header.ihl * 4) {
		if (logDiscards)
			std::println("netserver: Discarding IPv4 packet with a total length below its IHL");
		return false;
	}

	if (data.size() < header.length) {
		if (logDiscards)
			std::println("netserver: Discarding IPv4 packet smaller than its total length");
		return false;
	}

	// Drop link-layer padding; reassembled datagrams are already built to their exact length.
	if (resizeData)
		data = data.subview(0, header.length);

	Checksum csum;
	csum.update(header_view());
	auto sum = csum.finalize();
	if (sum != 0 && sum != 0xFFFF) {
		if (logDiscards)
			std::println("netserver: Discarding IPv4 packet with invalid checksum");
		return false;
	}

	return true;
}

namespace {
auto checkAddress(const void *addr_ptr, size_t addr_len, uint32_t &ip) {
	struct sockaddr_in addr;
	if (addr_len < sizeof(addr)) {
		return protocols::fs::Error::illegalArguments;
	}
	std::memcpy(&addr, addr_ptr, sizeof(addr));
	if (addr.sin_family != AF_INET) {
		return protocols::fs::Error::afNotSupported;
	}
	ip = addr.sin_addr.s_addr;
	return protocols::fs::Error::none;
}
}

struct Ip4Socket {
	explicit Ip4Socket(int proto) : proto(proto) {}

	static async::result<RecvResult> recvmsg(void *obj,
			helix_ng::CredentialsView creds,
			uint32_t flags, void *data, size_t len,
			void *addr_buf, size_t addr_size, size_t max_ctrl_len) {
		(void) creds;
		(void) flags;
		(void) max_ctrl_len;

		using arch::convert_endian;
		using arch::endian;
		auto self = static_cast<Ip4Socket *>(obj);
		while (self->pqueue.empty()) {
			co_await self->bell.async_wait();
		}
		auto element = std::move(self->pqueue.front());
		self->pqueue.pop();
		auto packet = element->data;
		auto copy_size = std::min(packet.size(), len);
		std::memcpy(data, packet.data(), copy_size);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = convert_endian<endian::big>(element->header.protocol);
		addr.sin_addr = { convert_endian<endian::big>(element->header.source) };
		std::memset(addr_buf, 0, addr_size);
		std::memcpy(addr_buf, &addr, std::min(addr_size, sizeof(addr)));
		co_return RecvData{{}, copy_size, sizeof(addr), 0};
	}

	static async::result<frg::expected<protocols::fs::Error, size_t>> sendmsg(void *obj,
			helix_ng::CredentialsView creds, uint32_t flags,
			void *data, size_t len,
			void *addr_ptr, size_t addr_size,
			std::vector<uint32_t> fds, struct ucred ucreds);

	static async::result<Error> connect(void* obj,
			helix_ng::CredentialsView creds,
			const void *addr_ptr, size_t addr_size) {
		(void) creds;

		auto self = static_cast<Ip4Socket *>(obj);
		uint32_t ip;
		if (auto e = checkAddress(addr_ptr, addr_size, ip);
			e != protocols::fs::Error::none) {
			co_return e;
		}

		// TODO(arsen): check other broadcast addresses too
		if (ip == INADDR_ANY) {
			co_return protocols::fs::Error::accessDenied;
		}
		self->remote = ip;
		co_return protocols::fs::Error::none;
	}

	constexpr static FileOperations ops {
		.connect = &connect,
		.recvMsg = &recvmsg,
		.sendMsg = &sendmsg,
	};
private:
	friend struct Ip4;
	int proto;
	uint32_t remote = 0;
	std::queue<smarter::shared_ptr<const Ip4Packet>> pqueue;
	async::recurring_event bell;
};

async::result<frg::expected<protocols::fs::Error, size_t>> Ip4Socket::sendmsg(void *obj,
		helix_ng::CredentialsView creds, uint32_t flags,
		void *data, size_t len,
		void *addr_ptr, size_t addr_size,
		std::vector<uint32_t> fds, struct ucred) {
	(void) creds;
	(void) flags;
	(void) fds;

	using arch::convert_endian;
	using arch::endian;
	auto self = static_cast<Ip4Socket *>(obj);
	uint32_t address;
	if (addr_size != 0) {
		if (auto e = checkAddress(addr_ptr, addr_size, address);
			e != protocols::fs::Error::none) {
			co_return e;
		}
	} else {
		address = self->remote;
	}

	address = convert_endian<endian::big>(address);

	if (address == 0) {
		std::cout << "netserver: needs destination" << std::endl;
		co_return protocols::fs::Error::destAddrRequired;
	}

	if (address == INADDR_BROADCAST) {
		co_return protocols::fs::Error::accessDenied;
	}

	auto ti = co_await ip4().targetByRemote(address);
	if (!ti) {
		co_return protocols::fs::Error::netUnreachable;
	}

	auto error = co_await ip4().sendFrame(std::move(*ti),
		data, len, self->proto);
	if (error != protocols::fs::Error::none) {
		co_return error;
	}

	co_return len;
}

Ip4::Ip4()
: fragmentIdentPrng{std::random_device{}()},
	icmp{std::make_unique<Icmp>()},
	tcp{std::make_unique<Tcp4>()},
	udp{std::make_unique<Udp4>()} {

	fragmentTimer_();
}

async::result<std::optional<Ip4TargetInfo>>
Ip4::targetByRemote(uint32_t remote, std::shared_ptr<nic::Link> link) {
	auto oroute = ip4Router().resolveRoute(remote, link);
	if (!oroute) {
		std::cout << "netserver: net unreachable" << std::endl;
		co_return std::nullopt;
	} else if (oroute->type == RTN_LOCAL) {
		oroute->link = nic::getLoopback();
	}

	auto target = oroute->link.lock();
	if (!target) {
		std::cout << "netserver: route link disappeared"
			<< std::endl;
		// TODO(arsen): remove route too
		co_return std::nullopt;
	}

	auto source = oroute->source;
	if (source == 0) {
		auto elvis = oroute->gateway ? oroute->gateway : remote;
		source = findLinkIp(elvis, target.get()).value_or(0);
	}
	if (source == 0) {
		std::cout << "netserver: could not find same network ip for "
			<< std::hex << std::setw(8) << remote << std::dec << std::endl;
		co_return std::nullopt;
	}

	co_return Ip4TargetInfo { remote, source, *oroute, std::move(target) };
}

bool Ip4::hasIp(uint32_t addr) {
	return std::any_of(ips.cbegin(), ips.cend(),
		[addr] (auto &x) {
			return x.first.ip == addr;
		});
}

auto Ip4::classifyAddress(uint32_t addr) -> AddressType {
	if (addr == ipv4Broadcast)
		return AddressType::broadcast;
	if (isIpv4Multicast(addr))
		return AddressType::multicast;

	// Like Linux, we accept every local address on every link (i.e. the weak host model).
	for (auto &entry : ips) {
		if (entry.second.expired())
			continue;

		if (entry.first.ip == addr)
			return AddressType::unicast;
		// The entire loopback network is local to a host that carries a loopback address.
		if (isIpv4Loopback(entry.first.ip) && isIpv4Loopback(addr))
			return AddressType::unicast;
		// Directed broadcast, e.g. 192.168.1.255 within 192.168.1.0/24 (RFC 3021: none on /31).
		if (entry.first.prefix < 31 && entry.first.sameNet(addr)
				&& (addr | entry.first.mask()) == ipv4Broadcast)
			return AddressType::broadcast;
	}

	return AddressType::foreign;
}

async::result<protocols::fs::Error> Ip4::sendFrame(Ip4TargetInfo ti,
		void *data, size_t len, uint16_t proto) {
	using arch::convert_endian;
	using arch::endian;

	// calculate header size
	size_t header_size = sizeof(Ip4Packet::Header);
	size_t fragmentMtu = len + header_size;

	// TODO(arsen): options
	if (ti.route.mtu != 0 && ti.route.mtu < fragmentMtu) {
		fragmentMtu = ti.route.mtu;
	}

	auto &target = ti.link;
	if (target->mtu < fragmentMtu) {
		fragmentMtu = target->mtu;
	}

	std::optional<nic::MacAddress> mac;
	if(!target->rawIp()) {
		auto macTarget = ti.route.gateway;
		if (macTarget == 0) {
			macTarget = ti.remote;
		}

		mac = co_await neigh4().tryResolve(macTarget, ti.source);
		if (!mac) {
			co_return protocols::fs::Error::hostUnreachable;
		}
	}

	if(fragmentMtu < header_size + 8) {
		std::cout << "netserver: fragment MTU is less than IP header size + 8"
			<< std::endl;
		co_return protocols::fs::Error::messageSize;
	}

	Ip4Packet::Header originalHdr;
	// TODO(arsen): options
	originalHdr.ihl = 0x45;
	originalHdr.tos = 0;
	originalHdr.length = 0;
	originalHdr.ident = 0;
	originalHdr.flags_offset = 0;
	originalHdr.ttl = 64;
	originalHdr.protocol = proto;
	// filled out later, 0 for purposes of computation
	originalHdr.checksum = 0;
	originalHdr.source = ti.source;
	originalHdr.destination = ti.remote;

	size_t fragment8Bytes = (fragmentMtu - header_size) / 8;

	// If the data fits to one packet increment fragment8Bytes to make sure that the data
	// is fully sent in one packet even if the length is not evenly divisible by 8.
	if(len <= fragmentMtu - header_size) {
		fragment8Bytes++;
	} else {
		// The data is fragmented and needs an identifier.

		FragmentRouteIdentification identification{
			.sourceIp = ti.source,
			.destIp = ti.remote,
			.protocol = static_cast<uint8_t>(proto)
		};

		auto fragmentRoute = getOrCreateFragmentRoute_(identification);
		originalHdr.ident = fragmentRoute->sendIdent++;
	}

	size_t progress = 0;
	do {
		size_t fragmentLength = std::min(len - progress, fragment8Bytes * 8);
		bool last = fragmentLength == len - progress;

		Ip4Packet::Header hdr = originalHdr;

		hdr.length = fragmentLength + header_size;
		if (!last) {
			hdr.flags_offset |= ip4FlagMoreFragments << 13;
		}

		hdr.flags_offset |= (progress / 8);

		hdr.ensureEndian();

		Checksum chk;
		// TODO(arsen): accomodate for options
		chk.update(reinterpret_cast<void *>(&hdr), sizeof(hdr));
		hdr.checksum = convert_endian<endian::big>(chk.finalize());

		nic::Link::AllocatedBuffer fb;
		if(mac) {
			fb = target->allocateFrame(*mac, nic::ETHER_TYPE_IP4, fragmentLength + header_size);
		} else {
			fb = target->allocateFrame(fragmentLength + header_size);
		}

		auto dataPtr = reinterpret_cast<const char *>(data) + progress;
		std::memcpy(fb.payload.data(), &hdr, sizeof(hdr));
		std::memcpy(fb.payload.subview(header_size).byte_data(), dataPtr, fragmentLength);

		co_await target->send(std::move(fb.frame));

		progress += fragmentLength;
	} while (progress < len);

	co_return protocols::fs::Error::none;
}

void Ip4::feedPacket(nic::MacAddress, nic::MacAddress,
		arch::dma_buffer owner, arch::dma_buffer_view frame, std::weak_ptr<nic::Link> link) {
	Ip4Packet hdr{};
	hdr.link = link;

	if (!hdr.parse(std::move(owner), frame, true)) {
		return;
	}

	auto linkPtr = link.lock();
	if (!linkPtr)
		return;

	auto source = hdr.header.source;
	auto destination = hdr.header.destination;
	bool onLoopback = linkPtr->iff_flags() & IFF_LOOPBACK;

	if (source == ipv4Broadcast || isIpv4Multicast(source)) {
		if (logDiscards)
			std::println("netserver: Discarding IPv4 packet with a group source address");
		return;
	}

	// Limited broadcasts (e.g. DHCP) are the only packets that may come from 0.0.0.0.
	if (isIpv4Zeronet(source) && destination != ipv4Broadcast) {
		if (logDiscards)
			std::println("netserver: Discarding IPv4 packet with a zero source address");
		return;
	}

	// Loopback addresses never appear on a link that does not carry loopback traffic.
	if (!onLoopback && (isIpv4Loopback(source) || isIpv4Loopback(destination))) {
		if (logDiscards)
			std::println("netserver: Discarding IPv4 packet with a loopback address");
		return;
	}

	// Loopback legitimately carries packets that originate from one of our own addresses.
	if (!onLoopback && classifyAddress(source) != AddressType::foreign) {
		if (logDiscards)
			std::println("netserver: Discarding IPv4 packet with a local or directed broadcast source address");
		return;
	}

	if (classifyAddress(destination) == AddressType::foreign) {
		if (logDiscards)
			std::println("netserver: Discarding IPv4 packet with a foreign destination address");
		return;
	}

	auto proto = hdr.header.protocol;

	auto begin = sockets.lower_bound(proto);
	if (begin == sockets.end()
			&& proto != static_cast<uint16_t>(IpProto::icmp)
			&& proto != static_cast<uint16_t>(IpProto::udp)
			&& proto != static_cast<uint16_t>(IpProto::tcp)) {
		return;
	}

	uint8_t flags = hdr.header.flags_offset >> 13;
	uint16_t fragmentOffset = hdr.header.flags_offset & 0x1fff;

	if (fragmentOffset != 0 || (flags & ip4FlagMoreFragments)) {
		uint32_t fragmentOffsetBytes = static_cast<uint32_t>(fragmentOffset) * 8;
		uint32_t fragmentSize = hdr.payload().size();
		size_t end = fragmentOffsetBytes + fragmentSize;

		// The reassembled datagram plus its header has to fit into the total length field.
		if (end > UINT16_MAX - sizeof(Ip4Packet::Header)) {
			if (logDiscards)
				std::println("netserver: Discarding IPv4 fragment beyond the maximum datagram size");
			return;
		}

		FragmentRouteIdentification routeIdentification{
			.sourceIp = hdr.header.source,
			.destIp = hdr.header.destination,
			.protocol = hdr.header.protocol
		};

		uint16_t fragmentIdent = hdr.header.ident;

		auto fragmentRoute = getOrCreateFragmentRoute_(routeIdentification);

		auto &fragmentedPackets = fragmentRoute->receivedPackets;
		auto fragmentPacketInsertResult = fragmentedPackets.insert({fragmentIdent, FragmentedPacket{}});
		auto fragmentedPacket = &fragmentPacketInsertResult.first->second;

		// If this is the first packet set the timer tick.
		if (fragmentPacketInsertResult.second)
			fragmentedPacket->packetReceivedTimerTick = fragmentTimerTick;

		if (fragmentSize != 0) {
			auto it = std::find_if(fragmentedPacket->fragments.begin(), fragmentedPacket->fragments.end(),
					[&](const auto &existing) {
				auto start = existing.first;
				auto end = start + existing.second.size;
				return fragmentOffsetBytes < end && start < fragmentOffsetBytes + fragmentSize;
			});

			if (it != fragmentedPacket->fragments.end()) {
				if (logDiscards)
					std::println("netserver: Discarding overlapping IPv4 fragment");
				return;
			}

			auto result = fragmentedPacket->fragments.insert({fragmentOffsetBytes, {fragmentSize}});
			assert(result.second);
		}

		if (end > fragmentedPacket->data.size()) {
			if (fragmentedPacket->lastReceived) {
				if (logDiscards)
					std::println("netserver: Discarding IPv4 fragment beyond the final packet size");
				return;
			}

			fragmentedPacket->data.resize(end);
		}

		memcpy(fragmentedPacket->data.data() + fragmentOffsetBytes, hdr.payload().data(), fragmentSize);

		if (!(flags & ip4FlagMoreFragments)) {
			if (fragmentedPacket->lastReceived) {
				if (logDiscards)
					std::println("netserver: Discarding duplicate final IPv4 fragment");
				return;
			}

			fragmentedPacket->lastReceived = true;
		}

		if (fragmentedPacket->lastReceived) {
			uint32_t progress = 0;
			while (progress < fragmentedPacket->data.size()) {
				auto it = fragmentedPacket->fragments.find(progress);
				if (it == fragmentedPacket->fragments.end()) {
					return;
				}

				progress += it->second.size;
			}

			arch::dma_buffer buffer{nullptr, fragmentedPacket->data.size() + sizeof(Ip4Packet::Header)};
			memcpy(static_cast<char *>(buffer.data()) + sizeof(Ip4Packet::Header), fragmentedPacket->data.data(),
					fragmentedPacket->data.size());

			auto newHdr = hdr.header;
			newHdr.ihl = 0x45;
			newHdr.flags_offset = 0;
			newHdr.checksum = 0;
			newHdr.length = fragmentedPacket->data.size() + sizeof(Ip4Packet::Header);
			newHdr.ensureEndian();

			Checksum chk;
			chk.update(&newHdr, sizeof(newHdr));
			newHdr.checksum = arch::convert_endian<arch::endian::big>(chk.finalize());

			memcpy(buffer.data(), &newHdr, sizeof(newHdr));

			arch::dma_buffer_view view = buffer;

			// The header is already parsed once so it has to be valid.
			bool hdrParseResult = hdr.parse(std::move(buffer), view, false);
			assert(hdrParseResult);

			fragmentedPackets.erase(fragmentIdent);
		} else {
			return;
		}
	}

	auto hdrs = smarter::make_shared<const Ip4Packet>(std::move(hdr));

	switch (static_cast<IpProto>(proto)) {
	case IpProto::icmp: icmp->feedDatagram(hdrs, link); break;
	case IpProto::udp: udp->feedDatagram(hdrs, link); break;
	case IpProto::tcp: tcp->feedDatagram(hdrs); break;
	default: break;
	}

	for (; begin != sockets.end() && begin->first == proto; begin++) {
		begin->second->pqueue.emplace(hdrs);
		begin->second->bell.raise();
	}
}

void Ip4::setLink(CidrAddress addr, std::weak_ptr<nic::Link> l) {
	ips.emplace(addr, std::move(l));
}

std::shared_ptr<nic::Link> Ip4::getLink(uint32_t addr) {
	auto iter = std::find_if(ips.begin(), ips.end(),
		[addr] (const auto &e) { return e.first.ip == addr; });
	if (iter == ips.end()) {
		return {};
	}
	auto ptr = iter->second.lock();
	if (!ptr) {
		ips.erase(iter);
		return {};
	}
	return ptr;
}

std::optional<CidrAddress> Ip4::getCidrByIndex(int index) {
	auto iter = std::find_if(ips.begin(), ips.end(),
		[index] (const auto &e) {
			auto ptr = e.second.lock();
			if(ptr)
				return ptr->index() == index;
			return false;
		});

	if(iter == ips.end()) {
		return std::nullopt;
	}

	return iter->first;
}

bool Ip4::deleteLink(CidrAddress addr) {
	return ips.erase(addr) > 0;
}

std::optional<uint32_t> Ip4::findLinkIp(uint32_t ipOnNet, nic::Link *link) {
	for (auto &entry : ips) {
		if (!entry.second.expired() && entry.first.sameNet(ipOnNet)) {
			auto o = entry.second.lock();
			if (o.get() == link) {
				return entry.first.ip;
			}
		}
	}
	return {};
}

managarm::fs::Errors Ip4::serveSocket(helix::UniqueLane ctrlLane, helix::UniqueLane ptLane, int type, int proto, int flags) {
	using namespace protocols::fs;
	switch (type) {
	case SOCK_RAW: {
		auto sock = smarter::make_shared<Ip4Socket>(proto);
		sockets.emplace(proto, sock);
		async::detach(servePassthrough(std::move(ptLane),
				sock, &Ip4Socket::ops),
			[this, socket = sock.get()] {
				for (auto i = sockets.begin();
					i != sockets.end();
					i++) {
					if (i->second.get() == socket) {
						sockets.erase(i);
						break;
					}
				}
			});
		return managarm::fs::Errors::SUCCESS;
	}
	case SOCK_DGRAM:
		switch(proto) {
		case IPPROTO_ICMP:
			icmp->serveSocket(std::move(ctrlLane), std::move(ptLane));
			break;
		default:
			udp->serveSocket(flags, std::move(ctrlLane), std::move(ptLane));
			break;
		}
		return managarm::fs::Errors::SUCCESS;
	case SOCK_STREAM:
		tcp->serveSocket(flags, std::move(ctrlLane), std::move(ptLane));
		return managarm::fs::Errors::SUCCESS;
	default:
		return managarm::fs::Errors::ILLEGAL_ARGUMENT;
	}
}

Ip4::FragmentRouteInfo *Ip4::getOrCreateFragmentRoute_(const FragmentRouteIdentification &ident) {
	static std::uniform_int_distribution<uint16_t> dist{
		0, 0xffff
	};

	if(auto it = fragmentRoutes.find(ident); it != fragmentRoutes.end()) {
		it->second.lastAccessedTimerTick = fragmentTimerTick;
		return &it->second;
	} else {
		FragmentRouteInfo fragmentRouteInfo{};

		fragmentRouteInfo.sendIdent = dist(fragmentIdentPrng);
		fragmentRouteInfo.lastAccessedTimerTick = fragmentTimerTick;

		return &fragmentRoutes.insert({ident, std::move(fragmentRouteInfo)}).first->second;
	}
}

async::detached Ip4::fragmentTimer_() {
	std::cout << "netserver: starting fragment timer" << std::endl;

	while (true) {
		co_await helix::sleepFor(fragmentExpiryTimeoutNs);

		for (auto routeIt = fragmentRoutes.begin(); routeIt != fragmentRoutes.end();) {
			auto &route = routeIt->second;

			// Discard fragment routes if they aren't active anymore.
			if (route.lastAccessedTimerTick + fragmentRouteExpiryTimeoutTicks < fragmentTimerTick) {
				routeIt = fragmentRoutes.erase(routeIt);
				continue;
			}

			auto &fragmentedPackets = route.receivedPackets;
			for (auto it = fragmentedPackets.begin(); it != fragmentedPackets.end();) {
				auto &packet = it->second;
				if (packet.packetReceivedTimerTick != fragmentTimerTick) {
					std::cout << "netserver: discarding fragmented packet due to timeout" << std::endl;
					it = fragmentedPackets.erase(it);
				}
				else {
					it++;
				}
			}

			routeIt++;
		}

		fragmentTimerTick++;
	}
}
