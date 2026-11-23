#pragma once

#include <async/result.hpp>
#include <async/recurring-event.hpp>
#include <protocols/fs/server.hpp>
#include <netserver/nic.hpp>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include "core/netlink.hpp"
#include "ip/ip4.hpp"
#include "ip/arp.hpp"

#include <deque>
#include <vector>

namespace nl {

void initialize();

void broadcast(core::netlink::Packet packet);

core::netlink::Packet buildLinkPacket(std::shared_ptr<nic::Link> nic, uint32_t group,
		uint16_t flags, uint32_t seq, uint32_t pid);

void broadcastNewLink(std::shared_ptr<nic::Link> nic);

core::netlink::Packet buildAddrPacket(uint16_t type, CidrAddress addr, std::shared_ptr<nic::Link> nic,
		uint8_t ifaFlags, uint32_t group, uint16_t flags, uint32_t seq, uint32_t pid);

class NetlinkSocket final : core::netlink::NetlinkFile {
public:
	NetlinkSocket(int flags, int protocol);

	void handleClose();

	static async::result<protocols::fs::RecvResult> recvMsg(void *obj,
			helix_ng::CredentialsView creds, uint32_t flags, void *data,
			size_t len, void *addr_buf, size_t addr_size, size_t max_ctrl_len);

	static async::result<frg::expected<protocols::fs::Error, size_t>> sendMsg(void *obj,
			helix_ng::CredentialsView creds, uint32_t flags, void *data, size_t len,
			void *addr_ptr, size_t addr_size, std::vector<uint32_t> fds, struct ucred ucreds);


	static async::result<protocols::fs::Error> bind(void *obj, helix_ng::CredentialsView creds,
			const void *addr_ptr, size_t addr_length);

	static async::result<frg::expected<protocols::fs::Error, size_t>> sockname(void *, void *, size_t);

	static async::result<frg::expected<protocols::fs::Error, protocols::fs::PollWaitResult>>
	pollWait(void *object, uint64_t sequence, int mask, async::cancellation_token cancellation);

	static async::result<frg::expected<protocols::fs::Error, protocols::fs::PollStatusResult>>
	pollStatus(void *object);

	static async::result<frg::expected<protocols::fs::Error>>
	setSocketOption(void *object, int layer, int number, std::vector<char> optbuf);

	static async::result<frg::expected<protocols::fs::Error>>
	getSocketOption(void *object, helix_ng::CredentialsView creds, int layer, int number, std::vector<char> &optbuf);

	static async::result<void> setFileFlags(void *object, int flags);
	static async::result<int> getFileFlags(void *object);

	constexpr static protocols::fs::FileOperations ops {
		.pollWait = &pollWait,
		.pollStatus = &pollStatus,
		.bind = &bind,
		.sockname = &sockname,
		.getFileFlags = &getFileFlags,
		.setFileFlags = &setFileFlags,
		.recvMsg = &recvMsg,
		.sendMsg = &sendMsg,
		.setSocketOption = &setSocketOption,
		.getSocketOption = &getSocketOption,
	};

	void deliver(core::netlink::Packet packet) override;

	const int protocol;
private:
	void getRoute(struct nlmsghdr *hdr);
	void newRoute(struct nlmsghdr *hdr);
	void deleteRoute(struct nlmsghdr *hdr);

	void getLink(struct nlmsghdr *hdr);

	void newAddr(struct nlmsghdr *hdr);
	void getAddr(struct nlmsghdr *hdr);
	void deleteAddr(struct nlmsghdr *hdr);

	void getNeighbor(struct nlmsghdr *hdr);

	void sendLinkPacket(std::shared_ptr<nic::Link> nic, void *h, uint16_t flags);
	void sendAddrPackets(const struct nlmsghdr *hdr, const struct ifaddrmsg *msg, std::shared_ptr<nic::Link> nic);
	void sendRoutePacket(const struct nlmsghdr *hdr, Ip4Router::Route &route);
	void sendNeighPacket(const struct nlmsghdr *hdr, uint32_t addr, Neighbours::Entry &entry);

	int flags;

	// Status management for poll()
	async::recurring_event _statusBell;
	bool _isClosed = false;
	uint64_t _currentSeq = 0;
	uint64_t _inSeq = 0;
	bool _passCreds = false;
	bool _nonBlock = false;
	bool pktinfo_ = false;

	struct GroupBitmap {
		void set(size_t i, bool set = true) {
			size_t chunk = i / CHAR_BIT;
			size_t offset = i % CHAR_BIT;

			// fill missing chunks with zero
			if(data_.size() <= chunk) {
				for(size_t c = data_.size(); c <= chunk; c++) {
					data_.push_back(0);
				}
			}

			if(set)
				data_.at(chunk) |= (1 << offset);
			else
				data_.at(chunk) &= ~(1 << offset);
		}

		bool get(size_t i) const {
			size_t chunk = i / CHAR_BIT;
			size_t offset = i % CHAR_BIT;

			if(chunk < data_.size()) {
				return data_.at(chunk) & (1 << offset);
			}

			return false;
		}

		size_t writeList(std::span<uint32_t> span) const {
			size_t written = 0;

			for(size_t i = 0; i < data_.size(); i++) {
				for(size_t bit = 0; bit < CHAR_BIT; bit++) {
					if(data_.at(i) & (1 << bit)) {
						span[written++] = ((i * CHAR_BIT) + bit);
						if(written >= span.size())
							return written;
					}
				}
			}

			return written;
		}
	private:
		std::vector<uint8_t> data_;
	};

	GroupBitmap groupMemberships_;

	std::deque<core::netlink::Packet> _recvQueue;
};

} // namespace nl
