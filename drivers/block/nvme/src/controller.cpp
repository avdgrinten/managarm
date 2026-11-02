#include <arch/bit.hpp>
#include <format>
#include <helix/timer.hpp>
#include <protocols/mbus/client.hpp>

#include "controller.hpp"

namespace regs {
	constexpr arch::bit_register<uint64_t> cap{0x0};
	constexpr arch::scalar_register<uint32_t> vs{0x8};
	constexpr arch::scalar_register<uint32_t> intms{0xc};
	constexpr arch::scalar_register<uint32_t> intmc{0x10};
	constexpr arch::bit_register<uint32_t> cc{0x14};
	constexpr arch::scalar_register<uint32_t> csts{0x1c};
	constexpr arch::scalar_register<uint32_t> aqa{0x24};
	constexpr arch::scalar_register<uint64_t> asq{0x28};
	constexpr arch::scalar_register<uint64_t> acq{0x30};
} // namespace regs

namespace flags {
	namespace cap {
		constexpr arch::field<uint64_t, uint16_t> mqes{0, 16};
		constexpr arch::field<uint64_t, uint8_t> dstrd{32, 4};
	} // namespace cap

	namespace vs {
		static inline uint32_t version(uint16_t major, uint8_t minor, uint8_t tertiary) {
			return ((uint32_t)major << 16) | ((uint32_t)minor << 8) | tertiary;
		}
	} // namespace vs

	namespace cc {
		constexpr arch::field<uint32_t, uint8_t> iocqes{20, 4};
		constexpr arch::field<uint32_t, uint8_t> iosqes{16, 4};
		constexpr arch::field<uint32_t, bool> enable{0, 1};
	} // namespace cc

	namespace csts {
		constexpr int ready = 1 << 0;
	} // namespace csts
} // namespace flags

PciExpressController::PciExpressController(
    int64_t parentId,
    protocols::hw::Device hwDevice,
    std::string location,
    helix::Mapping regsMapping,
	helix::UniqueDescriptor ioSpace,
	bool iommuActive
)
: Controller(parentId, location, ControllerType::PciExpress),
  hwDevice_{std::move(hwDevice)},
  regsMapping_{std::move(regsMapping)},
  regs_{regsMapping_.get()},
  ioSpace_{std::move(ioSpace)},
  dmaSpace_{dmaRealm_.attachDmaSpace(ioSpace_, iommuActive)} {}

async::detached PciExpressController::run(mbus_ng::EntityId subsystem) {
	co_await reset();
	co_await scanNamespaces();

	mbus_ng::Properties descriptor{
		{"class", mbus_ng::StringItem{"nvme-controller"}},
		{"nvme.subsystem", mbus_ng::StringItem{std::to_string(subsystem)}},
		{"nvme.address", mbus_ng::StringItem{location_}},
		{"nvme.transport", mbus_ng::StringItem{"pcie"}},
		{"nvme.serial", mbus_ng::StringItem{serial}},
		{"nvme.model", mbus_ng::StringItem{model}},
		{"nvme.fw-rev", mbus_ng::StringItem{fw_rev}},
		{"drvcore.mbus-parent", mbus_ng::StringItem{std::to_string(parentId_)}},
	};

	mbusEntity_ = std::make_unique<mbus_ng::EntityManager>((co_await mbus_ng::Instance::global().createEntity(
		"nvme-controller", descriptor)).unwrap());

	for (auto &ns : activeNamespaces_)
		ns->run();
}

async::detached PciExpressController::handleIrqs(helix::UniqueDescriptor irq) {
	irqSequence_ = 0;

	while (true) {
		auto awaitResult = co_await helix_ng::awaitEvent(irq, irqSequence_);

		regs_.store(regs::intms, 1);

		HEL_CHECK(awaitResult.error());
		irqSequence_ = awaitResult.sequence();

		int found = 0;
		for (auto &q : activeQueues_) {
			found |= static_cast<PciExpressQueue *>(q.get())->handleIrq();
		}

		regs_.store(regs::intmc, 1);

		if (found) {
			HEL_CHECK(helAcknowledgeIrq(irq.getHandle(), kHelAckAcknowledge, irqSequence_));
		} else {
			HEL_CHECK(helAcknowledgeIrq(irq.getHandle(), kHelAckNack, irqSequence_));
		}
	}
}

async::detached PciExpressController::handleMsis(helix::UniqueDescriptor irq, size_t queueId, bool isMsiX) {
	irqSequence_ = 0;

	while (true) {
		auto awaitResult = co_await helix_ng::awaitEvent(irq, irqSequence_);

		auto q = std::ranges::find_if(activeQueues_, [queueId](auto &q) {
			return q->getQueueId() == queueId;
		});

		if(q == activeQueues_.end()) {
			std::cout << std::format("nvme: Queue ID {} not found, quitting", queueId) << std::endl;
			break;
		}

		if(!isMsiX)
			regs_.store(regs::intms, 1 << queueId);

		HEL_CHECK(awaitResult.error());
		irqSequence_ = awaitResult.sequence();

		static_cast<PciExpressQueue *>(q->get())->handleIrq();

		if(!isMsiX)
			regs_.store(regs::intmc, 1 << queueId);

		HEL_CHECK(helAcknowledgeIrq(irq.getHandle(), kHelAckAcknowledge, irqSequence_));
	}
}

async::result<void> PciExpressController::waitStatus(bool enabled) {
	auto readyBit = enabled ? flags::csts::ready : 0;

	co_await helix::kindaBusyWait(50'000'000,
								  [&] { return (regs_.load(regs::csts) & flags::csts::ready) == readyBit; });
}

async::result<void> PciExpressController::enable() {
	auto cc = regs_.load(regs::cc);
	auto new_val = cc / flags::cc::iosqes(6) / flags::cc::iocqes(4) / flags::cc::enable(true);
	regs_.store(regs::cc, new_val);

	co_await waitStatus(true);
}

async::result<void> PciExpressController::disable() {
	auto cc = regs_.load(regs::cc);
	auto new_val = cc / flags::cc::enable(false);
	regs_.store(regs::cc, new_val);

	co_await waitStatus(false);
}

async::result<void> PciExpressController::setupIOQueueInterrupts(size_t queueId, size_t vector) {
	if(irqMode_ == InterruptMode::Msi || irqMode_ == InterruptMode::MsiX) {
		auto irq = co_await hwDevice_.installMsi(vector);
		handleMsis(std::move(irq), queueId, irqMode_ == InterruptMode::MsiX);
	}
}

async::result<void> PciExpressController::reset() {
	auto cap = regs_.load(regs::cap);
	const auto doorbellsOffset = 0x1000;

	queueDepth_ = std::min((cap & flags::cap::mqes) + 1, IO_QUEUE_DEPTH);
	dbStride_ = 1 << (cap & flags::cap::dstrd);

	version_ = regs_.load(regs::vs);

	co_await disable();

	auto info = co_await hwDevice_.getPciInfo();

	if(info.numMsis) {
		irqMode_ = info.msiX ? InterruptMode::MsiX : InterruptMode::Msi;
		co_await hwDevice_.enableMsi();
		co_await setupIOQueueInterrupts(0, 0);
	} else {
		irqMode_ = InterruptMode::LegacyIrq;
		auto irq = co_await hwDevice_.accessIrq();
		co_await hwDevice_.enableBusIrq();
		handleIrqs(std::move(irq));
	}

	auto adminQ = std::make_unique<PciExpressQueue>(this, 0, 32, regs_.subspace(doorbellsOffset));
	co_await adminQ->init();

	uint32_t aqa = (31 << 16) | 31;
	regs_.store(regs::aqa, aqa);
	co_await dmaSpace_.ensure_mapped(adminQ->getSq());
	co_await dmaSpace_.ensure_mapped(adminQ->getCq());
	regs_.store(regs::asq, dmaSpace_.iova_of(adminQ->getSq()));
	regs_.store(regs::acq, dmaSpace_.iova_of(adminQ->getCq()));

	adminQ->run();
	activeQueues_.push_back(std::move(adminQ));

	co_await enable();

	co_await requestIoQueues(1, 1);

	co_await setupIOQueueInterrupts(1, 1);
	auto ioQ = std::make_unique<PciExpressQueue>(this, 1, queueDepth_, regs_.subspace(doorbellsOffset + 1 * 8 * dbStride_), 1);
	co_await ioQ->init();

	if (co_await setupIoQueue(ioQ.get())) {
		ioQ->run();
		activeQueues_.push_back(std::move(ioQ));
	}

	assert(activeQueues_.size() >= 2 && "At least need one IO queue");
}

async::result<Command::Result> PciExpressController::requestIoQueues(uint16_t sqs, uint16_t cqs) {
	auto &adminQ = activeQueues_.front();
	auto cmd = std::make_unique<Command>();
	auto &setFeat = cmd->getCommandBuffer().setFeatures;

	setFeat.opcode = static_cast<uint8_t>(spec::AdminOpcode::SetFeatures);
	setFeat.data[0] = 0x07;
	setFeat.data[1] = ((cqs - 1) << 16) | (sqs - 1);

	return adminQ->submitCommand(std::move(cmd));
}

async::result<bool> PciExpressController::setupIoQueue(PciExpressQueue *q) {
	auto cqRes = co_await createCQ(q);
	if (!cqRes.first.successful())
		co_return false;

	auto sqRes = co_await createSQ(q);
	if (!sqRes.first.successful())
		co_return false;

	co_return true;
}

async::result<Command::Result> PciExpressController::createCQ(PciExpressQueue *q) {
	using arch::convert_endian;
	using arch::endian;

	auto &adminQ = activeQueues_.front();
	auto cmd = std::make_unique<Command>();
	auto &cmdBuf = cmd->getCommandBuffer().createCQ;

	uint16_t flags = spec::kQueuePhysContig | spec::kCQIrqEnabled;

	co_await dmaSpace_.ensure_mapped(q->getCq());

	cmdBuf.opcode = static_cast<uint8_t>(spec::AdminOpcode::CreateCQ);
	cmdBuf.prp1 = convert_endian<endian::little, endian::native>(dmaSpace_.iova_of(q->getCq()));
	cmdBuf.cqid = convert_endian<endian::little, endian::native>((uint16_t)q->getQueueId());
	cmdBuf.qSize = convert_endian<endian::little, endian::native>((uint16_t)q->getQueueDepth() - 1);
	cmdBuf.cqFlags = convert_endian<endian::little, endian::native>((uint16_t)flags);
	cmdBuf.irqVector = q->interruptVector();

	co_return co_await adminQ->submitCommand(std::move(cmd));
}

async::result<Command::Result> PciExpressController::createSQ(PciExpressQueue *q) {
	using arch::convert_endian;
	using arch::endian;

	auto &adminQ = activeQueues_.front();
	auto cmd = std::make_unique<Command>();
	auto &cmdBuf = cmd->getCommandBuffer().createSQ;

	uint16_t flags = spec::kQueuePhysContig;

	co_await dmaSpace_.ensure_mapped(q->getSq());

	cmdBuf.opcode = static_cast<uint8_t>(spec::AdminOpcode::CreateSQ);
	cmdBuf.prp1 = convert_endian<endian::little, endian::native>(dmaSpace_.iova_of(q->getSq()));
	cmdBuf.sqid = convert_endian<endian::little, endian::native>((uint16_t)q->getQueueId());
	cmdBuf.qSize = convert_endian<endian::little, endian::native>((uint16_t)q->getQueueDepth() - 1);
	cmdBuf.sqFlags = convert_endian<endian::little, endian::native>((uint16_t)flags);
	cmdBuf.cqid = convert_endian<endian::little, endian::native>((uint16_t)q->getQueueId());

	co_return co_await adminQ->submitCommand(std::move(cmd));
}

async::result<Command::Result> Controller::identifyController(arch::dma_object_view<spec::IdentifyController> id) {
	auto &adminQ = activeQueues_.front();
	auto cmd = std::make_unique<Command>();
	auto &cmdBuf = cmd->getCommandBuffer().identify;

	cmdBuf.opcode = static_cast<uint8_t>(spec::AdminOpcode::Identify);
	cmdBuf.cns = spec::kIdentifyController;
	co_await cmd->setupBuffer(this, id.view_buffer(), preferredDataTransfer_);

	co_return co_await adminQ->submitCommand(std::move(cmd));
}

async::result<Command::Result> Controller::identifyNamespaceList(unsigned int nsid, arch::dma_buffer_view list) {
	using arch::convert_endian;
	using arch::endian;

	auto &adminQ = activeQueues_.front();
	auto cmd = std::make_unique<Command>();
	auto &cmdBuf = cmd->getCommandBuffer().identify;

	cmdBuf.opcode = static_cast<uint8_t>(spec::AdminOpcode::Identify);
	cmdBuf.cns = spec::kIdentifyActiveList;
	cmdBuf.nsid = convert_endian<endian::little, endian::native>(nsid);
	co_await cmd->setupBuffer(this, list, preferredDataTransfer_);

	co_return co_await adminQ->submitCommand(std::move(cmd));
}

async::result<Command::Result> Controller::identifyNamespace(unsigned int nsid, arch::dma_object_view<spec::IdentifyNamespace> id) {
	using arch::convert_endian;
	using arch::endian;

	auto &adminQ = activeQueues_.front();
	auto cmd = std::make_unique<Command>();
	auto &cmdBuf = cmd->getCommandBuffer().identify;

	cmdBuf.opcode = static_cast<uint8_t>(spec::AdminOpcode::Identify);
	cmdBuf.cns = spec::kIdentifyNamespace;
	cmdBuf.nsid = convert_endian<endian::little, endian::native>(nsid);
	co_await cmd->setupBuffer(this, id.view_buffer(), preferredDataTransfer_);

	co_return co_await adminQ->submitCommand(std::move(cmd));
}

async::result<void> Controller::scanNamespaces() {
	using arch::convert_endian;
	using arch::endian;

	arch::dma_object<spec::IdentifyController> idCtrl{&pool_};
	int nn;

	if (!(co_await identifyController({idCtrl})).first.successful())
		co_return;

	auto type = idCtrl->cntrltype;
	if (version_ >= flags::vs::version(1, 4, 0)) {
		// error out on reserved controller type
		if(type == 0) {
			std::cout << std::format("block/nvme: invalid controller type {} reported in identify controller request", type) << std::endl;
			co_return;
		}

		// TODO: support non-I/O controllers
		if(type != 1) {
			std::cout << std::format("block/nvme: unsupported controller type {} reported in identify controller request", type) << std::endl;
			co_return;
		}
	} else if(type != 0 && type != 1) {
		std::cout << std::format("block/nvme: invalid controller type {} reported in identify controller request", type) << std::endl;
		co_return;
	}

	nn = convert_endian<endian::little>(idCtrl->nn);

	model = std::string{idCtrl->mn, sizeof(idCtrl->mn)};
	serial = std::string{idCtrl->sn, sizeof(idCtrl->sn)};
	fw_rev = std::string{idCtrl->fr, sizeof(idCtrl->fr)};
	ioccsz_ = convert_endian<endian::little>(idCtrl->ioccsz);
	icdoff_ = convert_endian<endian::little>(idCtrl->icdoff);
	// MDTS is in units of CAP.MPSMIN; assuming the smallest (4 KiB) can only underestimate.
	if (idCtrl->mdts)
		maxTransferSize_ = size_t{0x1000} << idCtrl->mdts;

	if (version_ >= flags::vs::version(1, 1, 0)) {
		auto nsList = arch::dma_array<uint32_t>{&pool_, 1024};
		int numLists = (nn + 1023) >> 10;
		unsigned int prev = 0;
		for (auto i = 0; i < numLists; i++) {
			if (!(co_await identifyNamespaceList(prev, nsList.view_buffer())).first.successful())
				co_return;

			int j;
			for (j = 0; j < std::min((int)nn, 1024); j++) {
				auto nsid = convert_endian<endian::little>(nsList[j]);
				if (!nsid)
					co_return;

				co_await createNamespace(nsid);

				prev = nsid;
			}

			nn -= j;
		}

		co_return;
	}

	for (int i = 1; i <= nn; i++) {
		co_await createNamespace(i);
	}
}

async::result<void> Controller::createNamespace(unsigned int nsid) {
	arch::dma_object<spec::IdentifyNamespace> id{&pool_};

	if (!(co_await identifyNamespace(nsid, id)).first.successful())
		co_return;

	if (!id->ncap)
		co_return;

	auto lbaShift = id->lbaf[id->flbas & 0xf].ds;
	if (!lbaShift)
		lbaShift = 9;

	auto ns = std::make_unique<Namespace>(this, nsid, lbaShift, id->nsze);
	activeNamespaces_.push_back(std::move(ns));
}

async::result<Command::Result> PciExpressController::submitAdminCommand(std::unique_ptr<Command> cmd) {
	auto &q = activeQueues_.front();

	return q->submitCommand(std::move(cmd));
}

async::result<Command::Result> PciExpressController::submitIoCommand(std::unique_ptr<Command> cmd) {
	auto &ioQ = activeQueues_.back();

	return ioQ->submitCommand(std::move(cmd));
}

async::result<void> PciExpressController::ensureMapped(arch::dma_buffer_view view) {
	return dmaSpace_.ensure_mapped(view);
}

std::optional<uintptr_t> PciExpressController::prpAddressOf(arch::dma_buffer_view view) {
	return dmaSpace_.iova_of(view);
}
