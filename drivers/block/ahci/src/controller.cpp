#include <bit> // For std::popcount

#include <inttypes.h>

#include <helix/timer.hpp>

#include "controller.hpp"

namespace regs {
	constexpr arch::scalar_register<uint32_t> cap{0x0};
	constexpr arch::scalar_register<uint32_t> ghc{0x4};
	constexpr arch::scalar_register<uint32_t> interruptStatus{0x8};
	constexpr arch::scalar_register<uint32_t> portsImpl{0xC};
	constexpr arch::scalar_register<uint32_t> version{0x10};
	constexpr arch::scalar_register<uint32_t> cap2{0x24};
	constexpr arch::scalar_register<uint32_t> biosHandoff{0x28};
}

namespace flags {
	namespace ghc {
		constexpr int ahciEnable          = 1 << 31;
		constexpr int revertSingleMessage = 1 << 2;
		constexpr int interruptEnable     = 1 << 1;
		constexpr int hbaReset            = 1;
	}

	namespace bohc {
		constexpr int biosBusy       = 1 << 4;
		constexpr int osOwnership    = 1 << 1;
		constexpr int biosOwnership  = 1;
	}

	namespace cap {
		constexpr int supports64Bit   = 1 << 31;
		constexpr int staggeredSpinup = 1 << 27;
	}

	namespace cap2 {
		constexpr int supportsHandoff = 1;
	}
}

Controller::Controller(
    int64_t parentId,
    protocols::hw::Device hwDevice,
    helix::Mapping hbaRegs,
    helix::UniqueDescriptor irq,
    bool useMsis,
    helix::UniqueDescriptor dmaSpace,
    bool iommuActive
)
: hwDevice_{std::move(hwDevice)},
  regsMapping_{std::move(hbaRegs)},
  regs_{regsMapping_.get()},
  irq_{std::move(irq)},
  dmaSpaceHandle_{std::move(dmaSpace)},
  dmaSpace_{dmaRealm_.attachDmaSpace(dmaSpaceHandle_, iommuActive)},
  parentId_{parentId},
  useMsis_{useMsis} {}

async::detached Controller::run() {
	// Enable AHCI
	auto ghc = regs_.load(regs::ghc);
	regs_.store(regs::ghc, ghc | flags::ghc::ahciEnable);

	// Perform BIOS -> OS handoff (10.6.3)
	auto version = regs_.load(regs::version);
	auto cap2 = regs_.load(regs::cap2);
	if (version >= 0x10200 && (cap2 & flags::cap2::supportsHandoff)) {
		auto biosHandoff = regs_.load(regs::biosHandoff);
		regs_.store(regs::biosHandoff, biosHandoff | flags::bohc::osOwnership);

		// Spec is slightly unclear what to do here: first, wait on BOS = 0 for 25ms.
		auto success = co_await helix::kindaBusyWait(25'000'000,
				[&]{ return !(regs_.load(regs::biosHandoff) & flags::bohc::biosOwnership); });

		if (success) {
			std::cout << "block/ahci: BIOS handoff successful\n";
		} else {
			// If BB is now set, we wait on BOS = 0 for 2 seconds.
			if (regs_.load(regs::biosHandoff) & flags::bohc::biosBusy) {
				std::cout << "block/ahci: BIOS handoff timed out once, retrying...\n";
				success = co_await helix::kindaBusyWait(2'000'000'000,
					[&]{ return !(regs_.load(regs::biosHandoff) & flags::bohc::biosOwnership); });
				assert(success && "block/ahci: BIOS handoff timed out twice");
			} else {
				std::cout << "block/ahci: BIOS handoff timed out once, assuming control\n";
			}
		}
	}

	// Reset the controller (10.4.3)
	ghc = regs_.load(regs::ghc);
	regs_.store(regs::ghc, ghc | flags::ghc::hbaReset);

	// Wait until the reset is complete (HR = 0), with a timeout of 1s
	auto success = co_await helix::kindaBusyWait(1'000'000'000,
		[&]{ return !(regs_.load(regs::ghc) & flags::ghc::hbaReset); });
	assert(success && "block/ahci: HBA timed out after reset");

	ghc = regs_.load(regs::ghc);
	regs_.store(regs::ghc, ghc | flags::ghc::ahciEnable);

	auto cap = regs_.load(regs::cap);
	maxPorts_ = (cap & 0x1F) + 1;
	assert(maxPorts_ <= 32);

	portsImpl_ = regs_.load(regs::portsImpl);
	assert(portsImpl_ != 0 && std::popcount(portsImpl_) <= maxPorts_);

	// auto numCommandSlots = 1;
	auto numCommandSlots = ((cap >> 8) & 0x1F) + 1;
	auto iss = (cap >> 20) & 0xF;
	bool ss = cap & flags::cap::staggeredSpinup;
	bool revertSingleMessage = regs_.load(regs::ghc) & flags::ghc::revertSingleMessage;
	bool s64a = cap & flags::cap::supports64Bit;
	assert(s64a); // TODO: We aren't allowed to read some fields if no 64-bit support

	printf("block/ahci: Initialised controller: version %x, %d active ports, "
			"%d slots, Gen %d, SS %s, 64-bit %s, MSI %s%s\n", version, std::popcount(portsImpl_),
			numCommandSlots, iss, ss ? "yes" : "no", s64a ? "yes" : "no", useMsis_ ? "yes" : "no",
			revertSingleMessage ? "/reverted to single" : "");

	if (!(co_await initPorts_(numCommandSlots, ss))) {
		std::cout << "\e[31mblock/ahci: No ports found, exiting\e[39m\n";
		co_return;
	}

	// Enable interrupts
	co_await hwDevice_.enableBusIrq();
	ghc = regs_.load(regs::ghc);
	regs_.store(regs::ghc, ghc | flags::ghc::interruptEnable);

	handleIrqs_();

	for (auto& port : activePorts_) {
		if (!(co_await port->run())) {
			dumpState_();
		}
	}
}

void Controller::dumpState_() {
	printf("block/ahci: Dumping controller state:\n");
	printf("  CAP: %#x\n", regs_.load(regs::cap));
	printf("  CAP2: %#x\n", regs_.load(regs::cap2));
	printf("  GHC: %#x\n", regs_.load(regs::ghc));
	printf("  IS: %#x\n", regs_.load(regs::interruptStatus));
	printf("  PI: %#x\n", regs_.load(regs::portsImpl));
	printf("  VS: %#x\n", regs_.load(regs::version));
	printf("  BOHC: %#x\n", regs_.load(regs::biosHandoff));
}

async::detached Controller::handleIrqs_() {
	irqSequence_ = 0;

	while (true) {
		if (logCommands) {
			printf("block/ahci: Awaiting IRQ, seq %" PRIu64 ", status %x\n",
				irqSequence_, regs_.load(regs::interruptStatus));
		}

		auto await = co_await helix_ng::awaitEvent(irq_, irqSequence_);
		HEL_CHECK(await.error());
		irqSequence_ = await.sequence();

		if (logCommands) {
			printf("block/ahci: Received IRQ, seq %" PRIu64 ", status %x\n",
				irqSequence_, regs_.load(regs::interruptStatus));
		}

		auto intStatus = regs_.load(regs::interruptStatus) & portsImpl_;
		if (intStatus) {
			for (auto &port : activePorts_) {
				if (intStatus & (1 << port->getIndex())) {
					port->handleIrq();
				}
			}

			regs_.store(regs::interruptStatus, intStatus);
			HEL_CHECK(helAcknowledgeIrq(irq_.getHandle(), kHelAckAcknowledge, irqSequence_));
		} else {
			if (useMsis_) {
				// In theory, we shouldn't get here, but refer to xhci handleMsis() comment for details.
				HEL_CHECK(helAcknowledgeIrq(irq_.getHandle(), kHelAckAcknowledge, irqSequence_));
			} else {
				for (auto &port : activePorts_) {
					std::cout << "block/ahci: NACK, dumping state on port " << port->getIndex() << ":\n";
					port->dumpState();
					port->checkErrors();
					std::cout << "\tglobal IS: " << regs_.load(regs::interruptStatus) << ", masked GIS " << (regs_.load(regs::interruptStatus) & portsImpl_) << ", read GIS " << intStatus << "\n";
				}

				HEL_CHECK(helAcknowledgeIrq(irq_.getHandle(), kHelAckNack, irqSequence_));
			}
		}
	}
}

async::result<bool> Controller::initPorts_(size_t numCommandSlots, bool ss) {
	for (int i = 0; i < maxPorts_; i++) {
		if (portsImpl_ & (1 << i)) {
			auto offset = 0x100 + i * 0x80;
			auto port = std::make_unique<Port>(this, parentId_, i, numCommandSlots, ss, regs_.subspace(offset));

			if (co_await port->init())
				activePorts_.push_back(std::move(port));
		}
	}

	co_return activePorts_.size() > 0;
}
