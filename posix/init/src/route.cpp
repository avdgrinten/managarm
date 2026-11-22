#include <arpa/inet.h>
#include <errno.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <string.h>
#include <sys/socket.h>
#include <vector>

#include "route.hpp"

namespace {

// Sends a NETLINK_ROUTE dump request and invokes fn on each message of the reply.
template<typename Payload, typename F>
std::expected<void, int> netlinkDump(int fd, uint16_t type, Payload payload, F fn) {
	struct {
		nlmsghdr hdr;
		Payload payload;
	} req{};
	req.hdr.nlmsg_len = NLMSG_LENGTH(sizeof(Payload));
	req.hdr.nlmsg_type = type;
	req.hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	req.payload = payload;
	if(send(fd, &req, req.hdr.nlmsg_len, 0) < 0)
		return std::unexpected{errno};

	std::vector<char> buffer(16384);
	while(true) {
		ssize_t len = recv(fd, buffer.data(), buffer.size(), 0);
		if(len < 0)
			return std::unexpected{errno};
		for(auto hdr = reinterpret_cast<nlmsghdr *>(buffer.data()); NLMSG_OK(hdr, len);
				hdr = NLMSG_NEXT(hdr, len)) {
			if(hdr->nlmsg_type == NLMSG_DONE)
				return {};
			if(hdr->nlmsg_type == NLMSG_ERROR) {
				auto err = reinterpret_cast<nlmsgerr *>(NLMSG_DATA(hdr));
				if(err->error)
					return std::unexpected{-err->error};
				return {};
			}
			fn(hdr);
		}
	}
}

// Returns the index of the interface that the most specific route to addr goes through.
std::expected<std::optional<int>, int> routeInterface(int fd, in_addr addr) {
	std::optional<int> index;
	int bestPrefix = -1;
	auto dump = netlinkDump(fd, RTM_GETROUTE, rtmsg{.rtm_family = AF_INET}, [&](nlmsghdr *hdr) {
		if(hdr->nlmsg_type != RTM_NEWROUTE)
			return;
		auto rtm = reinterpret_cast<rtmsg *>(NLMSG_DATA(hdr));
		if(rtm->rtm_family != AF_INET || rtm->rtm_dst_len > 32)
			return;
		uint32_t dst = 0;
		int oif = 0;
		int attrLen = RTM_PAYLOAD(hdr);
		for(auto rta = RTM_RTA(rtm); RTA_OK(rta, attrLen); rta = RTA_NEXT(rta, attrLen)) {
			if(rta->rta_type == RTA_DST)
				memcpy(&dst, RTA_DATA(rta), sizeof(dst));
			else if(rta->rta_type == RTA_OIF)
				memcpy(&oif, RTA_DATA(rta), sizeof(oif));
		}
		uint32_t mask = rtm->rtm_dst_len ? ~uint32_t{0} << (32 - rtm->rtm_dst_len) : 0;
		if(!oif || ((ntohl(addr.s_addr) ^ ntohl(dst)) & mask) || rtm->rtm_dst_len <= bestPrefix)
			return;
		index = oif;
		bestPrefix = rtm->rtm_dst_len;
	});
	if(!dump)
		return std::unexpected{dump.error()};
	return index;
}

} // namespace

// Returns the interface that the most specific route to addr goes through.
std::expected<std::optional<RouteLink>, int> routeLink(int fd, in_addr addr) {
	auto index = routeInterface(fd, addr);
	if(!index)
		return std::unexpected{index.error()};
	if(!*index)
		return std::nullopt;

	std::optional<RouteLink> link;
	auto dump = netlinkDump(fd, RTM_GETLINK, ifinfomsg{.ifi_family = AF_UNSPEC}, [&](nlmsghdr *hdr) {
		if(hdr->nlmsg_type != RTM_NEWLINK)
			return;
		auto ifi = reinterpret_cast<ifinfomsg *>(NLMSG_DATA(hdr));
		if(ifi->ifi_index != **index)
			return;
		link.emplace();
		int attrLen = IFLA_PAYLOAD(hdr);
		for(auto rta = IFLA_RTA(ifi); RTA_OK(rta, attrLen); rta = RTA_NEXT(rta, attrLen)) {
			if(rta->rta_type == IFLA_IFNAME)
				link->name = reinterpret_cast<char *>(RTA_DATA(rta));
		}
		link->lowerUp = ifi->ifi_flags & IFF_LOWER_UP;
	});
	if(!dump)
		return std::unexpected{dump.error()};
	return link;
}
