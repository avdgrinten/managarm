#include <format>
#include <helix/timer.hpp>
#include <protocols/fs/client.hpp>
#include <sys/epoll.h>

#include "../controller.hpp"
#include "tcp.hpp"

namespace nvme::fabric {

TcpQueue::TcpQueue(Tcp *controller, uint16_t cid, unsigned int index, unsigned int depth, in_addr addr, in_port_t port, helix::BorrowedLane lane, std::span<uint8_t, 16> uuid)
: Queue(controller, index, depth), addr_{addr}, port_{port}, lane_{std::move(lane)}, controllerId_{cid}, uuid_{uuid} {
}

async::result<protocols::fs::Error> TcpQueue::connect() {
	spec::tcp::ICReq connect_req {
		.ch{
			.pduType = spec::tcp::PduType::ICReq,
			.flags = 0,
			.headerLength = sizeof(spec::tcp::ICReq),
			.pduDataOffset = 0,
			.pduLength = sizeof(spec::tcp::ICReq),
		},
		.pduFormatVersion = 0,
		.hostPduDataAlignment = 0,
		.digest = 0,
		.maxr2t = 0,
	};

	size_t sent = 0;
	while(sent < sizeof(connect_req)) {
		auto send_err = co_await file_->sendto(reinterpret_cast<std::byte *>(&connect_req) + sent, sizeof(connect_req) - sent, 0, nullptr, 0);
		if(!send_err)
			co_return send_err.error();
		sent += send_err.value();
	}

	spec::tcp::ICResp resp{};
	size_t read = 0;
	while(read < sizeof(resp)) {
		auto recv_err = co_await file_->recvfrom(reinterpret_cast<std::byte *>(&resp) + read, sizeof(resp) - read, 0, nullptr, 0);
		if(!recv_err)
			co_return recv_err.error();
		read += recv_err.value();
	}

	if(resp.ch.pduType != spec::tcp::PduType::ICResp)
		co_return protocols::fs::Error::addressNotAvailable;

	connectedEvent_.raise();

	co_return protocols::fs::Error::none;
}

async::result<void> TcpQueue::init() {
	auto sock_err = co_await protocols::fs::File::createSocket(lane_, AF_INET, SOCK_STREAM, 0, 0);
	if(!sock_err) {
		std::cout << "block/nvme: failed to create socket for queue " << qid_ << std::endl;
		co_return;
	}

	file_ = std::make_unique<protocols::fs::File>(std::move(sock_err.value()));

	sockaddr_in sockaddr{};
	sockaddr.sin_family = AF_INET;
	sockaddr.sin_port = htons(port_);
	sockaddr.sin_addr.s_addr = addr_.s_addr;

	auto connect_err = co_await file_->connect(reinterpret_cast<struct sockaddr *>(&sockaddr), sizeof(sockaddr));
	if(connect_err != protocols::fs::Error::none) {
		std::cout << "block/nvme: failed to TCP connect for queue " << qid_ << std::endl;
		co_return;
	}

	if(co_await connect() != protocols::fs::Error::none) {
		std::cout << "block/nvme: failed to init queue " << qid_ << std::endl;
		co_return;
	}

	auto cmd = std::make_unique<Command>();
	auto &connectCmd = cmd->getCommandBuffer().fabricConnect;
	connectCmd.opcode = static_cast<uint8_t>(spec::AdminOpcode::Fabrics);
	connectCmd.flags = 0x40;
	connectCmd.fabricsCommandType = static_cast<uint16_t>(spec::FabricsCommand::Connect);
	connectCmd.recordFormat = 0;
	connectCmd.queueId = qid_;
	connectCmd.sqSize = depth_ - 1;
	connectCmd.connectAttrs = 0;
	connectCmd.keepAliveTimeout = keepAliveTimeout_;

	spec::fabric::ConnectCommandData connectData({
		.controllerId = controllerId_,
		.subsystemNqn = "nqn.2024-12.org.managarm:nvme:managarm-boot",
	});

	memcpy(connectData.hostIdentifier, uuid_.data(), sizeof(connectData.hostIdentifier));
	auto nqn = std::format("nqn.2014-08.org.nvmexpress:uuid:{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
		uuid_[0], uuid_[1], uuid_[2], uuid_[3], uuid_[4], uuid_[5], uuid_[6], uuid_[7],
		uuid_[8], uuid_[9], uuid_[10], uuid_[11], uuid_[12], uuid_[13], uuid_[14], uuid_[15]);
	strncpy(connectData.hostNqn, nqn.c_str(), sizeof(connectData.hostNqn));

	co_await cmd->setupBuffer(controller_, arch::dma_buffer_view{nullptr, &connectData, sizeof(connectData)}, spec::DataTransfer::SGL);
	auto res = co_await submitCommand(std::move(cmd));
	if(!res.first.successful()) {
		std::cout << "block/nvme: failed to set up queue " << qid_ << std::endl;
		co_return;
	}

	controllerId_ = res.second.u16;
}

async::detached TcpQueue::keepAlive() {
	// sane keepalive timeout values are between 5 sec and 10 min
	assert(keepAliveTimeout_ > 5000);
	assert(keepAliveTimeout_ < 10 * 60 * 1000);

	while(true) {
		// Like Linux, send at half the timeout; the next sleep only starts once the previous
		// keep-alive has completed, so a fixed margin is eaten up by any delay.
		co_await helix::sleepFor(keepAliveTimeout_ / 2 * 1'000'000);

		auto cmd = std::make_unique<Command>();
		auto &packet = cmd->getCommandBuffer().common;
		packet.opcode = static_cast<uint8_t>(spec::AdminOpcode::KeepAlive);
		co_await cmd->setupBuffer(controller_, arch::dma_buffer_view{}, spec::DataTransfer::SGL);
		co_await submitCommand(std::move(cmd));
	}
}

async::detached TcpQueue::run() {
	co_await connectedEvent_.wait();

	submitPendingLoop();

	if(qid_ == 0)
		keepAlive();

	auto recvbuf = std::vector<std::byte>(65536);

	while(true) {
		size_t received = 0;

		while(received < sizeof(spec::tcp::PduCommonHeader)) {
			auto recv_err = co_await file_->recvfrom(recvbuf.data() + received, sizeof(spec::tcp::PduCommonHeader) - received, 0, nullptr, 0);
			if(!recv_err) {
				std::cout << "block/nvme: error on receive for queue " << qid_ << std::endl;
				co_return;
			}
			received += recv_err.value();
		}

		auto ch = reinterpret_cast<spec::tcp::PduCommonHeader *>(recvbuf.data());

		if(ch->pduLength > recvbuf.size())
			recvbuf.resize(ch->pduLength);

		while(received < ch->pduLength) {
			auto recv_err = co_await file_->recvfrom(recvbuf.data() + received, ch->pduLength - received, 0, nullptr, 0);
			if(!recv_err) {
				std::cout << "block/nvme: error on receive for queue " << qid_ << std::endl;
				co_return;
			}
			received += recv_err.value();
		}

		ch = reinterpret_cast<spec::tcp::PduCommonHeader *>(recvbuf.data());

		switch(ch->pduType) {
			case spec::tcp::PduType::CapsuleResp: {
				auto capsuleResp = reinterpret_cast<spec::tcp::CapsuleResp *>(recvbuf.data());
				auto slot = capsuleResp->responseCqe.commandId;
				if(slot < queuedCmds_.size() && queuedCmds_[slot]) {
					auto cmd = std::move(queuedCmds_[slot]);
					cmd->complete(spec::CompletionStatus{capsuleResp->responseCqe.status}, capsuleResp->responseCqe.result);
					commandsInFlight_--;
					freeSlotDoorbell_.raise();
				}
				break;
			}
			case spec::tcp::PduType::C2HData: {
				auto resp = reinterpret_cast<spec::tcp::C2HData *>(recvbuf.data());

				if(resp->ch.pduDataOffset + resp->dataLength > resp->ch.pduLength) {
					std::cout << std::format("block/nvme: NVMe-oF packet requests out-of-bound read, dropping") << std::endl;
					continue;
				}

				auto &cmd = queuedCmds_.at(resp->commandCapsuleId);
				if(cmd->view().byte_data() && cmd->view().size() >= resp->dataOffset + resp->dataLength) {
					memcpy(cmd->view().byte_data() + resp->dataOffset, recvbuf.data() + resp->ch.pduDataOffset, resp->dataLength);
				} else {
					std::cout << std::format("block/nvme: NVMe-oF packet requests out-of-bound read, dropping") << std::endl;
					continue;
				}
				break;
			}
			default: {
				std::cout << std::format("block/nvme: unhandled NVMe-oF PDU type {:#x}", static_cast<uint8_t>(ch->pduType)) << std::endl;
				co_return;
			}
		}
	}
}

async::detached TcpQueue::submitPendingLoop() {
	while (true) {
		auto cmd = co_await pendingCmdQueue_.async_get();
		assert(cmd);
		co_await submitCommandToDevice(std::move(cmd.value()));
	}
}

async::result<void> TcpQueue::submitCommandToDevice(std::unique_ptr<Command> cmd) {
	auto slot = co_await findFreeSlot();

	// we can safely reuse the buffer as we are (implicitly) serialized by `submitPendingLoop`
	new (buf_.data()) spec::tcp::CapsuleCmd({
		.ch = {
			.pduType = spec::tcp::PduType::CapsuleCmd,
			.flags = 0,
			.headerLength = sizeof(spec::tcp::CapsuleCmd) + sizeof(cmd->getCommandBuffer()),
			.pduDataOffset = 0,
			.pduLength = sizeof(spec::tcp::CapsuleCmd),
		}
	});

	auto capsuleCmd = reinterpret_cast<spec::tcp::CapsuleCmd *>(buf_.data());
	auto data_len = cmd->view().size();
	capsuleCmd->ch.pduLength += sizeof(cmd->getCommandBuffer());

	memcpy(&buf_[sizeof(spec::tcp::CapsuleCmd)], &cmd->getCommandBuffer(), sizeof(cmd->getCommandBuffer()));

	auto genericCommand = reinterpret_cast<spec::Command *>(&buf_[sizeof(spec::tcp::CapsuleCmd)]);
	genericCommand->common.commandId = slot;
	auto opcode = cmd->getCommandBuffer().common.opcode;

	if(data_len && opcode & 1) {
		if(data_len > capsuleCmd->ch.pduDataOffset + buf_.size())
			buf_.resize(capsuleCmd->ch.pduDataOffset + buf_.size());

		capsuleCmd->ch.pduDataOffset = sizeof(spec::tcp::CapsuleCmd) + sizeof(cmd->getCommandBuffer());
		memcpy(&buf_[capsuleCmd->ch.pduDataOffset], reinterpret_cast<void *>(cmd->view().byte_data()), data_len);
		capsuleCmd->ch.pduLength += data_len;
	}

	queuedCmds_[slot] = std::move(cmd);
	commandsInFlight_++;

	size_t sent = 0;

	while(sent < capsuleCmd->ch.pduLength) {
		auto send_err = co_await file_->sendto(buf_.data() + sent, capsuleCmd->ch.pduLength - sent, 0, nullptr, 0);
		if(!send_err) {
			std::cout << "block/nvme: error on send for queue " << qid_ << std::endl;
			co_return;
		}
		sent += send_err.value();
	}
}

async::result<Command::Result> TcpQueue::submitCommand(std::unique_ptr<Command> cmd) {
	auto future = cmd->getFuture();
	pendingCmdQueue_.put(std::move(cmd));
	co_return *(co_await future.get());
}

Tcp::Tcp(mbus_ng::EntityId entity, in_addr addr, in_port_t port, std::string location, helix::UniqueLane netserver)
: Controller(entity, location, ControllerType::FabricsTcp), serverAddr_{addr}, serverPort_{port}, netserverLane_{std::move(netserver)} {
	preferredDataTransfer_ = spec::DataTransfer::SGL;
}

async::detached Tcp::run(mbus_ng::EntityId subsystem) {
	uint8_t uuid[16];
	size_t n = 0;
	while(n < 16) {
		size_t chunk;
		HEL_CHECK(helGetRandomBytes(uuid + n, 16 - n, &chunk));
		n += chunk;
	}

	uuid[6] = (uuid[6] & 0x0F) | 0x40;
	uuid[8] = (uuid[8] & 0x3F) | 0x80;

	auto adminq = std::make_unique<TcpQueue>(this, 0xFFFF, 0, 32, serverAddr_, serverPort_, netserverLane_, std::span<uint8_t, 16>{uuid});
	adminq->run();
	co_await adminq->init();
	auto cid = adminq->controllerId();
	activeQueues_.push_back(std::move(adminq));

	std::cout << std::format("block/nvme: TCP socket connected to controller") << std::endl;

	mbus_ng::Properties descriptor{
		{"class", mbus_ng::StringItem{"nvme-controller"}},
		{"nvme.subsystem", mbus_ng::StringItem{std::to_string(subsystem)}},
		{"nvme.address", mbus_ng::StringItem{location_}},
		{"nvme.transport", mbus_ng::StringItem{"tcp"}},
		{"nvme.serial", mbus_ng::StringItem{serial}},
		{"nvme.model", mbus_ng::StringItem{model}},
		{"nvme.fw-rev", mbus_ng::StringItem{fw_rev}},
		{"drvcore.mbus-parent", mbus_ng::StringItem{"-1"}},
	};

	mbusEntity_ = std::make_unique<mbus_ng::EntityManager>((co_await mbus_ng::Instance::global().createEntity(
		"nvme-controller", descriptor)).unwrap());

	auto set_prop_err = co_await fabricSetProperty(0x14, 0x00460060, 4);
	if(!set_prop_err) {
		std::cout << "block/name: failed to configure Controller parameters" << std::endl;
		co_return;
	}

	set_prop_err = co_await fabricSetProperty(0x14, 0x00460061, 4);
	if(!set_prop_err) {
		std::cout << "block/name: failed to enable Controller" << std::endl;
		co_return;
	}

	auto cmd = std::make_unique<Command>();
	auto &setFeature = cmd->getCommandBuffer().setFeatures;
	setFeature.opcode = static_cast<uint8_t>(spec::AdminOpcode::SetFeatures);
	setFeature.nsid = 0;
	setFeature.data[0] = 0x07;
	setFeature.data[1] = 0;

	co_await cmd->setupBuffer(this, arch::dma_buffer_view{}, preferredDataTransfer_);
	co_await activeQueues_.front()->submitCommand(std::move(cmd));

	// // setup I/O queue
	auto ioq = std::make_unique<TcpQueue>(this, cid, 1, 128, serverAddr_, serverPort_, netserverLane_, std::span<uint8_t, 16>{uuid});
	ioq->run();
	co_await ioq->init();
	activeQueues_.push_back(std::move(ioq));

	co_await scanNamespaces();

	for (auto &ns : activeNamespaces_)
		ns->run();
}

async::result<Command::Result> Tcp::submitAdminCommand(std::unique_ptr<Command> cmd) {
	co_return co_await activeQueues_.at(0)->submitCommand(std::move(cmd));
}

async::result<Command::Result> Tcp::submitIoCommand(std::unique_ptr<Command> cmd) {
	co_return co_await activeQueues_.at(1)->submitCommand(std::move(cmd));
}

async::result<frg::expected<spec::CompletionStatus, uint64_t>> Tcp::fabricGetProperty(uint32_t propertyOffset, size_t size) {
	assert(size == 4 || size == 8);

	auto cmd = std::make_unique<Command>();
	auto &propCmd = cmd->getCommandBuffer().fabricPropertyGet;

	propCmd.opcode = static_cast<uint8_t>(spec::AdminOpcode::Fabrics);
	propCmd.flags = 0x40;
	propCmd.fabricsCommandType = static_cast<uint16_t>(spec::FabricsCommand::PropertyGet);
	propCmd.attributes = (size == 4) ? 0 : 1;
	propCmd.offset = propertyOffset;

	auto res = co_await activeQueues_.front()->submitCommand(std::move(cmd));
	if(res.first.successful())
		co_return {res.second.u64};

	co_return spec::CompletionStatus{res.first};
}

async::result<frg::expected<spec::CompletionStatus, uint64_t>> Tcp::fabricSetProperty(uint32_t propertyOffset, uint64_t value, size_t size) {
	assert(size == 4 || size == 8);

	auto cmd = std::make_unique<Command>();
	auto &propCmd = cmd->getCommandBuffer().fabricPropertySet;

	propCmd.opcode = static_cast<uint8_t>(spec::AdminOpcode::Fabrics);
	propCmd.flags = 0x40;
	propCmd.fabricsCommandType = static_cast<uint16_t>(spec::FabricsCommand::PropertySet);
	propCmd.attributes = (size == 4) ? 0 : 1;
	propCmd.offset = propertyOffset;
	propCmd.value = value;

	auto res = co_await activeQueues_.front()->submitCommand(std::move(cmd));
	if(res.first.successful())
		co_return {res.second.u64};

	co_return spec::CompletionStatus{res.first};
}

} // namespace nvme::fabric
