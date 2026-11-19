#include "core/netlink.hpp"
#include "netlink.hpp"
#include "src/ip/arp.hpp"

#include <arpa/inet.h>
#include <linux/neighbour.h>

#include <optional>

namespace nl {

using core::netlink::netlinkMessage;
using core::netlink::netlinkAttr;

void NetlinkSocket::getLink(struct nlmsghdr *hdr) {
	const struct ifinfomsg *msg;

	if(auto opt = netlinkMessage<struct ifinfomsg>(hdr, hdr->nlmsg_len))
		msg = *opt;
	else {
		auto rtgm = netlinkMessage<struct rtgenmsg>(hdr, hdr->nlmsg_len);

		if(rtgm) {
			msg = new ifinfomsg{.ifi_family = (*rtgm)->rtgen_family};
		} else {
			sendError(this, hdr, EINVAL);
			return;
		}
	}

	std::optional<std::string> if_name = std::nullopt;
	[[maybe_unused]] uint32_t ext_mask = 0;

	if(msg) {
		auto attrs = netlinkAttr(hdr, core::netlink::nl::packets::ifinfo{});

		if(attrs.has_value()) {
			for(auto attr : *attrs) {
				switch(attr.type()) {
					case IFLA_IFNAME: {
						if(auto opt = attr.str()) {
							if_name = opt;
						} else {
							std::cout << "netlink: string parsing from rtattr failed unexpectedly" << std::endl;
							sendError(this, hdr, EINVAL);
							return;
						}
						break;
					}
					case IFLA_EXT_MASK: {
						ext_mask = attr.data<uint32_t>().value_or(0);
						break;
					}
					default: {
						std::cout << "netlink: ignoring unknown attr " << attr.type() << std::endl;
						if(attr.type() > RTA_MAX) {
							sendError(this, hdr, EINVAL);
							return;
						}
						break;
					}
				}
			}
		}
	}

	if(!msg || msg->ifi_index == 0) {
		auto links = nic::Link::getLinks();

		for(auto m = links.begin(); m != links.end(); m++) {
			if(!if_name.has_value() || if_name == m->second->name()) {
				sendLinkPacket(m->second, hdr, NLM_F_MULTI);
			}
		}
	} else {
		auto nic = nic::Link::byIndex(msg->ifi_index);

		if(!nic) {
			sendError(this, hdr, ENODEV);
			return;
		}

		if(!if_name.has_value() || if_name == nic->name()) {
			sendLinkPacket(nic, hdr, 0);
		} else {
			sendError(this, hdr, ENODEV);
			return;
		}
	}

	if(hdr->nlmsg_flags & NLM_F_DUMP)
		sendDone(this, hdr);

	return;
}

void NetlinkSocket::newRoute(struct nlmsghdr *hdr) {
	const struct rtmsg *msg;

	if(auto opt = netlinkMessage<struct rtmsg>(hdr, hdr->nlmsg_len))
		msg = *opt;
	else {
		sendError(this, hdr, EINVAL);
		return;
	}

	auto attrs = netlinkAttr(hdr, core::netlink::nl::packets::rt{});

	if(!attrs.has_value()) {
		sendError(this, hdr, EINVAL);
		return;
	}

	Ip4Router::Route route { { 0, 0 }, {} };
	bool route_changed = false;

	for(auto attr : *attrs) {
		switch(attr.type()) {
			case RTA_DST: {
				uint32_t dst = ntohl(attr.data<uint32_t>().value_or(0));
				uint32_t dst_len = msg->rtm_dst_len;
				route.network.ip = dst;
				route.network.prefix = dst_len;
				route_changed = true;
				break;
			}
			case RTA_GATEWAY: {
				uint32_t gateway = ntohl(attr.data<uint32_t>().value_or(0));
				route.gateway = gateway;
				route_changed = true;
				break;
			}
			case RTA_PREFSRC: {
				uint32_t prefsrc = ntohl(attr.data<uint32_t>().value_or(0));
				route.source = prefsrc;
				route_changed = true;
				break;
			}
			case RTA_OIF: {
				int if_index = attr.data<int>().value_or(0);
				auto nic = nic::Link::byIndex(if_index);
				if(nic) {
					route.link = nic;
					route_changed = true;
				}
				break;
			}
			case RTA_PRIORITY: {
				int metric = attr.data<int>().value_or(0);
				route.metric = metric;
				route_changed = true;
				break;
			}
			case RTA_METRICS: {
				std::optional<uint32_t> rtax_mtu;

				for(auto rtax_attr : attr) {
					switch(rtax_attr.type()) {
						case RTAX_MTU: {
							if(auto value = rtax_attr.data<uint32_t>()) {
								rtax_mtu = value;
							} else {
								sendError(this, hdr, EINVAL);
								return;
							}
							break;
						}
						default:
							std::cout << "netlink: ignoring unknown metrics attr " << rtax_attr.type() << std::endl;
							if(rtax_attr.type() > RTAX_MAX) {
								sendError(this, hdr, EINVAL);
								return;
							}
							break;
					}
				}

				if(rtax_mtu) {
					route.mtu = *rtax_mtu;
					route_changed = true;
				}

				break;
			}
			default:
				std::cout << "netlink: ignoring unknown attr " << attr.type() << std::endl;
				if(attr.type() > RTA_MAX) {
					sendError(this, hdr, EINVAL);
					return;
				}
				break;
		}
	}

	if(msg->rtm_protocol)
		route.protocol = msg->rtm_protocol;
	if(msg->rtm_type)
		route.type = msg->rtm_type;
	if(msg->rtm_scope)
		route.scope = msg->rtm_scope;
	if(msg->rtm_flags)
		route.flags = msg->rtm_flags;
	if(msg->rtm_family)
		route.family = msg->rtm_family;

	if(route_changed) {
		// Like Linux, a route is identified by its destination and metric.
		auto sameRoute = [&] (const Ip4Router::Route &r) {
			return r.type != RTN_LOCAL && r.network.ip == route.network.ip
				&& r.network.prefix == route.network.prefix && r.metric == route.metric;
		};
		bool exists = std::ranges::any_of(ip4Router().getRoutes(), sameRoute);

		if(exists && (hdr->nlmsg_flags & NLM_F_EXCL)) {
			sendError(this, hdr, EEXIST);
			return;
		}
		// Prefer replacing an equivalent route so that addRoute() cannot fail after a deletion.
		if(exists && (hdr->nlmsg_flags & NLM_F_REPLACE)
				&& !ip4Router().deleteEquivalentRoute(route))
			ip4Router().deleteRoute(sameRoute);

		if(!ip4Router().addRoute(std::move(route))) {
			sendError(this, hdr, EEXIST);
			return;
		}
	}

	if(hdr->nlmsg_flags & NLM_F_ACK)
		sendAck(this, hdr);
}

void NetlinkSocket::getRoute(struct nlmsghdr *hdr) {
	// TODO: support single-route lookups (requests without NLM_F_DUMP).
	if(!(hdr->nlmsg_flags & NLM_F_DUMP)) {
		sendError(this, hdr, EOPNOTSUPP);
		return;
	}

	const struct rtgenmsg *payload;

	if(auto opt = netlinkMessage<struct rtgenmsg>(hdr, hdr->nlmsg_len))
		payload = *opt;
	else {
		sendError(this, hdr, EINVAL);
		return;
	}

	// We have no IPv6 routes; like Linux, other unknown families fall back to dumping all families.
	// TODO: also return ipv6 routes.
	if(payload->rtgen_family != AF_INET6) {
		auto ipv4_router = ip4Router();

		for(auto route : ipv4_router.getRoutes()) {
			sendRoutePacket(hdr, route);
		}
	}

	sendDone(this, hdr);
}

void NetlinkSocket::deleteRoute(struct nlmsghdr *hdr) {
	const struct rtmsg *msg;

	if(auto opt = netlinkMessage<struct rtmsg>(hdr, hdr->nlmsg_len))
		msg = *opt;
	else {
		sendError(this, hdr, EINVAL);
		return;
	}

	auto attrs = netlinkAttr(hdr, core::netlink::nl::packets::rt{});

	if(!attrs.has_value()) {
		sendError(this, hdr, EINVAL);
		return;
	}

	uint32_t table = msg->rtm_table;
	uint32_t dst = 0;
	uint32_t gateway = 0;
	uint32_t prefsrc = 0;
	int oif = 0;
	unsigned int metric = 0;

	for(auto attr : *attrs) {
		switch(attr.type()) {
			case RTA_TABLE: {
				table = attr.data<uint32_t>().value_or(0);
				break;
			}
			case RTA_DST: {
				dst = ntohl(attr.data<uint32_t>().value_or(0));
				break;
			}
			case RTA_GATEWAY: {
				gateway = ntohl(attr.data<uint32_t>().value_or(0));
				break;
			}
			case RTA_PREFSRC: {
				prefsrc = ntohl(attr.data<uint32_t>().value_or(0));
				break;
			}
			case RTA_OIF: {
				oif = attr.data<int>().value_or(0);
				break;
			}
			case RTA_PRIORITY: {
				metric = attr.data<unsigned int>().value_or(0);
				break;
			}
			default: {
				std::cout << "netlink: ignoring unknown attr " << attr.type() << " in RTM_DELROUTE request" << std::endl;
				if(attr.type() > RTA_MAX) {
					sendError(this, hdr, EINVAL);
					return;
				}
				break;
			}
		}
	}

	if(table == RT_TABLE_UNSPEC)
		table = RT_TABLE_MAIN;

	// Mirror Linux' fib_table_delete(): only the attributes that are non-zero have to match.
	auto matches = [&] (const Ip4Router::Route &r) {
		uint32_t routeTable = (r.type == RTN_LOCAL) ? RT_TABLE_LOCAL : RT_TABLE_MAIN;
		uint8_t routeType = r.type ? r.type : RTN_UNICAST;
		if(routeTable != table)
			return false;
		if(r.network.ip != dst || r.network.prefix != msg->rtm_dst_len)
			return false;
		if(msg->rtm_type && routeType != msg->rtm_type)
			return false;
		if(msg->rtm_scope != RT_SCOPE_NOWHERE && r.scope != msg->rtm_scope)
			return false;
		if(msg->rtm_protocol && r.protocol != msg->rtm_protocol)
			return false;
		if(prefsrc && r.source != prefsrc)
			return false;
		if(gateway && r.gateway != gateway)
			return false;
		if(metric && r.metric != metric)
			return false;
		if(oif) {
			auto link = r.link.lock();
			if(!link || link->index() != oif)
				return false;
		}
		return true;
	};

	if(!ip4Router().deleteRoute(matches)) {
		sendError(this, hdr, ESRCH);
		return;
	}

	if(hdr->nlmsg_flags & NLM_F_ACK)
		sendAck(this, hdr);
}

void NetlinkSocket::newAddr(struct nlmsghdr *hdr) {
	const struct ifaddrmsg *msg;

	if(auto opt = netlinkMessage<struct ifaddrmsg>(hdr, hdr->nlmsg_len))
		msg = *opt;
	else {
		sendError(this, hdr, EINVAL);
		return;
	}

	auto attrs = netlinkAttr(hdr, core::netlink::nl::packets::ifaddr{});

	if(!attrs.has_value()) {
		sendError(this, hdr, EINVAL);
		return;
	}

	uint32_t addr = 0;
	uint8_t prefix = msg->ifa_prefixlen;
	auto nic = nic::Link::byIndex(msg->ifa_index);

	if(!nic) {
		sendError(this, hdr, ENODEV);
		return;
	}

	for(auto &attr : *attrs) {
		switch(attr.type()) {
			case IFA_ADDRESS: {
				addr = ntohl(attr.data<uint32_t>().value_or(0));
				break;
			}
			case IFA_LOCAL: {
				addr = ntohl(attr.data<uint32_t>().value_or(0));
				break;
			}
			default: {
				std::cout << "netserver: ignoring unknown rtattr type " << attr.type() << " in RTM_NEWADDR request" << std::endl;
				if(attr.type() > RTA_MAX) {
					sendError(this, hdr, EINVAL);
					return;
				}
				break;
			}
		}
	}

	if(addr) {
		ip4().setLink({addr, prefix}, nic);

		Ip4Router::Route localRoute{{addr, 32}, nic};
		localRoute.type = RTN_LOCAL;
		localRoute.protocol = RTPROT_KERNEL;
		localRoute.scope = RT_SCOPE_HOST;
		localRoute.source = addr;
		ip4Router().addRoute(localRoute);
	}

	if(hdr->nlmsg_flags & NLM_F_ACK)
		sendAck(this, hdr);

	if(addr)
		broadcast(buildAddrPacket(RTM_NEWADDR, {addr, prefix}, nic, 0, RTNLGRP_IPV4_IFADDR,
				0, hdr->nlmsg_seq, hdr->nlmsg_pid));

	return;
}

void NetlinkSocket::getAddr(struct nlmsghdr *hdr) {
	// Like Linux, RTM_GETADDR only supports dumps.
	if(!(hdr->nlmsg_flags & NLM_F_DUMP)) {
		sendError(this, hdr, EOPNOTSUPP);
		return;
	}

	const struct ifaddrmsg *msg;

	if(auto opt = netlinkMessage<struct ifaddrmsg>(hdr, hdr->nlmsg_len))
		msg = *opt;
	else {
		auto rtgm = netlinkMessage<struct rtgenmsg>(hdr, hdr->nlmsg_len);

		if(rtgm) {
			msg = new ifaddrmsg{.ifa_family = (*rtgm)->rtgen_family};
		} else {
			sendError(this, hdr, EINVAL);
			return;
		}
	}

	// We have no IPv6 addresses; like Linux, other unknown families fall back to dumping all families.
	if(msg->ifa_family != AF_INET6) {
		auto links = nic::Link::getLinks();
		for(auto m = links.begin(); m != links.end(); m++) {
			if(!msg || msg->ifa_index == 0) {
				sendAddrPackets(hdr, msg, m->second);
			} else if(static_cast<uint32_t>(m->second->index()) == msg->ifa_index) {
				sendAddrPackets(hdr, msg, m->second);
				break;
			}
		}
	}

	sendDone(this, hdr);

	return;
}

void NetlinkSocket::deleteAddr(struct nlmsghdr *hdr) {
	const struct ifaddrmsg *msg;

	if(auto opt = netlinkMessage<struct ifaddrmsg>(hdr, hdr->nlmsg_len))
		msg = *opt;
	else {
		sendError(this, hdr, EINVAL);
		return;
	}

	auto attrs = netlinkAttr(hdr, core::netlink::nl::packets::ifaddr{});

	if(!attrs.has_value()) {
		sendError(this, hdr, EINVAL);
		return;
	}

	std::optional<uint32_t> local;
	std::optional<uint32_t> address;
	auto nic = nic::Link::byIndex(msg->ifa_index);

	if(!nic) {
		sendError(this, hdr, ENODEV);
		return;
	}

	for(auto attr : *attrs) {
		switch(attr.type()) {
			case IFA_LOCAL: {
				local = ntohl(attr.data<uint32_t>().value_or(0));
				break;
			}
			case IFA_ADDRESS: {
				address = ntohl(attr.data<uint32_t>().value_or(0));
				break;
			}
			default: {
				std::cout << "netserver: ignoring unknown rtattr type " << attr.type() << " in RTM_DELADDR request" << std::endl;
				if(attr.type() > RTA_MAX) {
					sendError(this, hdr, EINVAL);
					return;
				}
				break;
			}
		}
	}

	// Like Linux, delete the first address of the link that matches all of the given attributes.
	std::optional<CidrAddress> cidr;
	for(auto &[entry, link] : ip4().getAddresses()) {
		if(link.lock() != nic)
			continue;
		if(local && entry.ip != *local)
			continue;
		if(address && (entry.prefix != msg->ifa_prefixlen || !entry.sameNet(*address)))
			continue;
		cidr = entry;
		break;
	}

	if(!cidr) {
		sendError(this, hdr, EADDRNOTAVAIL);
		return;
	}

	ip4().deleteLink(*cidr);

	// Like Linux, drop the local route and all other routes that use the address as their source.
	if(!ip4().hasIp(cidr->ip)) {
		ip4Router().deleteRoutes([&] (const Ip4Router::Route &r) {
			return r.source == cidr->ip;
		});
	}

	if(hdr->nlmsg_flags & NLM_F_ACK)
		sendAck(this, hdr);

	broadcast(buildAddrPacket(RTM_DELADDR, *cidr, nic, 0, RTNLGRP_IPV4_IFADDR,
			0, hdr->nlmsg_seq, hdr->nlmsg_pid));

	return;
}

void NetlinkSocket::getNeighbor(struct nlmsghdr *hdr) {
	auto &table = neigh4().getTable();

	for(auto it = table.begin(); it != table.end(); it++) {
		sendNeighPacket(hdr, it->first, it->second);
	}

	if(hdr->nlmsg_flags & NLM_F_DUMP)
		sendDone(this, hdr);

	return;
}

} // namespace nl
