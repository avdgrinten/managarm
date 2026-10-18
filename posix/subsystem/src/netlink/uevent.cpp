#include "uevent.hpp"

namespace netlink {

async::result<protocols::fs::Error> uevent::sendMsg(nl_socket::OpenFile *, core::netlink::Packet packet, struct sockaddr_nl *sa) {
	// Carbon-copy to the message to a group.
	if(packet.group)
		nl_socket::deliverToGroup(NETLINK_KOBJECT_UEVENT, packet.group, packet);

	// Netlink delivers the message per unicast.
	// This is done even if the target address includes multicast groups.
	if(sa->nl_pid) {
		// Deliver to a user-mode socket.
		auto file = nl_socket::findPort(sa->nl_pid);
		if(!file)
			co_return protocols::fs::Error::connectionRefused;

		static_cast<nl_socket::OpenFile *>(file.get())->deliver(std::move(packet));
	}else{
		// TODO: Deliver the message a listener function.
	}

	co_return protocols::fs::Error::none;
}

} // namespace netlink
