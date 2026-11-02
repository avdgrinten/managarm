#include <algorithm>
#include <arch/bit.hpp>
#include <asm/ioctl.h>
#include <format>
#include <linux/nvme_ioctl.h>
#include <iostream>
#include <print>

#include "namespace.hpp"
#include "controller.hpp"

Namespace::Namespace(Controller *controller, unsigned int nsid, int lbaShift, size_t lbaCount)
	: BlockDevice{(size_t)1 << lbaShift, -1, &controller->memoryPool()}, controller_(controller), nsid_(nsid),
	  lbaShift_(lbaShift), lbaCount_{lbaCount} {
	diskNamePrefix = "nvme";
	diskNameSuffix = std::format("n{}", nsid);
	partNameSuffix = std::format("n{}p", nsid);
}

async::detached Namespace::run() {
	mbus_ng::Properties descriptor{
		{"class", mbus_ng::StringItem{"nvme-namespace"}},
		{"nvme.nsid", mbus_ng::StringItem{std::to_string(nsid_)}},
		{"drvcore.mbus-parent", mbus_ng::StringItem{std::to_string(controller_->getMbusId())}},
	};

	mbusEntity_ = std::make_unique<mbus_ng::EntityManager>((co_await mbus_ng::Instance::global().createEntity(
		"nvme-namespace", descriptor)).unwrap());
	parentId = mbusEntity_->id();

	// The number of blocks per command is a 16-bit field.
	size_t maxSectors = 0x10000;
	if (auto maxBytes = controller_->maxTransferSize())
		maxSectors = std::min(maxSectors, *maxBytes >> sectorShift);
	if (!maxSectors) {
		std::println(std::cout, "block/nvme: Blocks of namespace {} exceed the maximum transfer size", nsid_);
		co_return;
	}
	limits.maxSectorsPerRequest = maxSectors;
	// All namespaces share the I/O queue, so this is an upper bound that Queue enforces.
	limits.queueCapacity = controller_->ioQueueDepth();
	blockfs::runDevice(this);

	co_return;
}

async::result<std::expected<void, blockfs::Error>> Namespace::issueRead(unsigned int, uint64_t sector, arch::dma_buffer_view view) {
	using arch::convert_endian;
	using arch::endian;

	auto cmd = std::make_unique<Command>();
	auto &cmdBuf = cmd->getCommandBuffer().readWrite;

	auto numSectors = (view.size() + sectorSize - 1) >> sectorShift;

	cmdBuf.opcode = spec::kRead;
	cmdBuf.nsid = convert_endian<endian::little, endian::native>(nsid_);
	cmdBuf.startLba = convert_endian<endian::little, endian::native>(sector);
	cmdBuf.length = convert_endian<endian::little, endian::native>(numSectors - 1);
	co_await cmd->setupBuffer(controller_, view, controller_->dataTransferPolicy());

	auto result = co_await controller_->submitIoCommand(std::move(cmd));
	if (!result.first.successful()) {
		std::println(std::cout, "block/nvme: Read failed, status {:#x}", result.first.status);
		co_return std::unexpected{blockfs::Error::ioError};
	}
	co_return {};
}

async::result<std::expected<void, blockfs::Error>> Namespace::issueWrite(unsigned int, uint64_t sector, arch::dma_buffer_view view) {
	using arch::convert_endian;
	using arch::endian;

	auto cmd = std::make_unique<Command>();
	auto &cmdBuf = cmd->getCommandBuffer().readWrite;

	auto numSectors = (view.size() + sectorSize - 1) >> sectorShift;

	cmdBuf.opcode = spec::kWrite;
	cmdBuf.nsid = convert_endian<endian::little, endian::native>(nsid_);
	cmdBuf.startLba = convert_endian<endian::little, endian::native>(sector);
	cmdBuf.length = convert_endian<endian::little, endian::native>(numSectors - 1);
	co_await cmd->setupBuffer(controller_, view, controller_->dataTransferPolicy());

	auto result = co_await controller_->submitIoCommand(std::move(cmd));
	if (!result.first.successful()) {
		std::println(std::cout, "block/nvme: Write failed, status {:#x}", result.first.status);
		co_return std::unexpected{blockfs::Error::ioError};
	}
	co_return {};
}

async::result<std::expected<void, blockfs::Error>> Namespace::flush() {
	auto cmd = std::make_unique<Command>();
	auto &cmdBuf = cmd->getCommandBuffer().common;
	cmdBuf = {};
	cmdBuf.opcode = spec::kFlush;
	cmdBuf.namespaceId = arch::convert_endian<arch::endian::little, arch::endian::native>(nsid_);

	// PCIe uses zero data pointers; fabrics needs the empty transport SGL.
	co_await cmd->setupBuffer(controller_, {}, controller_->dataTransferPolicy());
	auto result = co_await controller_->submitIoCommand(std::move(cmd));
	if (!result.first.successful()) {
		std::println(std::cout, "block/nvme: Flush failed, status {:#x}", result.first.status);
		co_return std::unexpected{blockfs::Error::ioError};
	}
	co_return {};
}

async::result<size_t> Namespace::getSize() {
	co_return lbaCount_ << lbaShift_;
}

async::result<void> Namespace::handleIoctl(managarm::fs::GenericIoctlRequest &req, helix::BorrowedDescriptor conversation) {
	if(req.command() == NVME_IOCTL_ID) {
		managarm::fs::GenericIoctlReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_result(nsid_);

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
	} else if(req.command() == NVME_IOCTL_ADMIN_CMD) {
		nvme_admin_cmd param;

		auto [recv_buffer] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::recvBuffer(&param, sizeof(param))
		);
		HEL_CHECK(recv_buffer.error());

		auto cmd = std::make_unique<Command>();
		auto &cmdBuf = cmd->getCommandBuffer().common;

		cmdBuf.opcode = param.opcode;
		cmdBuf.flags = param.flags;
		cmdBuf.namespaceId = param.nsid;
		cmdBuf.cdw2[0] = param.cdw2;
		cmdBuf.cdw2[1] = param.cdw3;
		cmdBuf.cdw10 = param.cdw10;
		cmdBuf.cdw11 = param.cdw11;
		cmdBuf.cdw12 = param.cdw12;
		cmdBuf.cdw13 = param.cdw13;
		cmdBuf.cdw14 = param.cdw14;
		cmdBuf.cdw15 = param.cdw15;

		size_t data_size = param.data_len;
		arch::dma_buffer data_buf{&controller_->memoryPool(), data_size};

		auto [recv_data] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::recvBuffer(data_buf.data(), param.data_len)
		);
		HEL_CHECK(recv_data.error());

		co_await cmd->setupBuffer(controller_, data_buf, controller_->dataTransferPolicy());

		auto res = co_await controller_->submitAdminCommand(std::move(cmd));

		managarm::fs::GenericIoctlReply resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_status(res.first.status);
		resp.set_result(res.second.u64);

		auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
			helix_ng::sendBuffer(data_buf.data(), data_size)
		);
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_data.error());
	} else {
		std::cout << "\e[31m" "libblockfs: Unknown ioctl() message with ID "
				<< req.command() << "\e[39m" << std::endl;

		auto [dismiss] = co_await helix_ng::exchangeMsgs(
			conversation, helix_ng::dismiss());
		HEL_CHECK(dismiss.error());
	}
}
