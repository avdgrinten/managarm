#include <print>
#include <arpa/inet.h>
#include <arch/barrier.hpp>
#include <arch/mem_space.hpp>
#include <arch/variable.hpp>
#include <async/mutex.hpp>
#include <async/recurring-event.hpp>
#include <helix/memory.hpp>
#include <helix/timer.hpp>
#include <protocols/hw/client.hpp>
#include <netserver/phy.hpp>

#include <nic/bcmgenet/bcmgenet.hpp>

#include "main.hpp"
#include "reg.hpp"

namespace {

inline constexpr bool debugIrq = false;
inline constexpr bool traceMdio = false;
inline constexpr bool promiscMode = false;

} // namespace anonymous

namespace nic::bcmgenet {

async::result<std::expected<uint16_t, nic::PhyError>>
BcmGenetMii::read(uint8_t phyAddress, uint8_t registerNum) {
	if constexpr (traceMdio) {
		std::println("{} MDIO: Read {:02x}.{:02x}",
				parent_, phyAddress, registerNum);
	}

	parent_->space_.store(reg::mdio::cmd,
			mdio::cmd::startBusy(true)
			| mdio::cmd::read(true)
			| mdio::cmd::pmd(phyAddress)
			| mdio::cmd::reg(registerNum));

	// Wait for the transaction to complete.
	// TODO(qookie): We could use interrupts for this.
	auto result = co_await helix::kindaBusyWait(1'000'000'000, [&] {
		return !(parent_->space_.load(reg::mdio::cmd) & mdio::cmd::startBusy);
	});

	if (!result) {
		if constexpr (traceMdio) {
			std::println("{} MDIO: Read {:02x}.{:02x} = timeout",
					parent_, phyAddress, registerNum);
		}
		co_return std::unexpected{nic::PhyError::timeout};
	}

	auto value = parent_->space_.load(reg::mdio::cmd) & mdio::cmd::data;

	if constexpr (traceMdio) {
		std::println("{} MDIO: Read {:02x}.{:02x} = {:04x}",
				parent_, phyAddress, registerNum, value);
	}

	co_return value;
}

async::result<std::expected<void, nic::PhyError>>
BcmGenetMii::write(uint8_t phyAddress, uint8_t registerNum, uint16_t value) {
	if constexpr (traceMdio) {
		std::println("{} MDIO: Write {:02x}.{:02x} = {:04x}",
				parent_, phyAddress, registerNum, value);
	}

	parent_->space_.store(reg::mdio::cmd,
			mdio::cmd::startBusy(true)
			| mdio::cmd::write(true)
			| mdio::cmd::pmd(phyAddress)
			| mdio::cmd::reg(registerNum)
			| mdio::cmd::data(value));

	// Wait for the transaction to complete.
	// TODO(qookie): We could use interrupts for this.
	auto result = co_await helix::kindaBusyWait(1'000'000'000, [&] {
		return !(parent_->space_.load(reg::mdio::cmd) & mdio::cmd::startBusy);
	});

	if (!result) {
		if constexpr (traceMdio) {
			std::println("{} MDIO: Write {:02x}.{:02x} = {:04x} timeout",
					parent_, phyAddress, registerNum, value);
		}

		co_return std::unexpected{nic::PhyError::timeout};
	}

	if constexpr (traceMdio) {
		std::println("{} MDIO: Write {:02x}.{:02x} = {:04x} complete",
				parent_, phyAddress, registerNum, value);
	}

	co_return {};
}

async::result<void> BcmGenetNic::initialize() {
	auto rev_ = space_.load(reg::rev);
	auto major = rev_ & rev::major;
	auto minor = rev_ & rev::minor;

	if (!major) major = 1;
	if (major == 5 || major == 6) major--;

	std::println("{} GENETv{}.{}", this, major, minor);
	std::println("{} MAC address: {}", this, mac_);

	co_await reset_();

	setupBufs_();

	// TODO(qookie): The PHY address is found in the DT, but I'm assuming
	// it's static for the RPi4.
	phy_ = co_await nic::makeEthernetPhy(mii_, 1, phyMode_);

	if (!phy_) {
		std::println("{} No PHY found", this);
		co_return;
	}

	co_await phy_->configure();
	co_await phy_->startup();

	if (!phy_->linkStatus()) {
		std::println("{} Link is down", this);
		// TODO(qookie): This is not fatal.
		co_return;
	}
	updateLink_();

	space_.store(reg::portCtrl, portCtrl::extGphy);

	setupRings_();
	writeMacAddr_();
	setupRxFilter_();

	{
		auto umacCmd = space_.load(reg::umac::cmd);
		space_.store(reg::umac::cmd, umacCmd
				/ umac::cmd::rxEnable(true)
				/ umac::cmd::txEnable(true));
	}

	// Clear & mask all prior pending interrupts.
	space_.store(reg::intr::clear, arch::bit_value<uint32_t>{0xFFFFFFFF});
	space_.store(reg::intr::setMask, arch::bit_value<uint32_t>{0xFFFFFFFF});
	// Enable RX and TX interrupts.
	space_.store(reg::intr::clearMask, intr::txDmaDone(true) | intr::rxDmaDone(true));

	processIrqs_();
	processRx_();
	co_await device_.enableBusIrq();

	// Kick IRQs since they're already pending in the interrupt controller from earlier.
	HEL_CHECK(helAcknowledgeIrq(irq_.getHandle(), kHelAckKick, 0));

	std::println("{} NIC initialized", this);
}

async::result<void> BcmGenetNic::reset_() {
	auto rbufCtrl_ = space_.load(reg::rbufCtrl);

	space_.store(reg::rbufCtrl, rbufCtrl_ / rbufCtrl::reset(true));
	co_await helix::sleepFor(10'000);

	space_.store(reg::rbufCtrl, rbufCtrl_ / rbufCtrl::reset(false));
	co_await helix::sleepFor(10'000);

	space_.store(reg::rbufCtrl, arch::bit_value<uint32_t>{0});
	co_await helix::sleepFor(10'000);

	space_.store(reg::umac::cmd, arch::bit_value<uint32_t>{0});
	space_.store(reg::umac::cmd, umac::cmd::localLoopback(true)
			| umac::cmd::swReset(true));
	co_await helix::sleepFor(10'000);
	space_.store(reg::umac::cmd, arch::bit_value<uint32_t>{0});

	space_.store(reg::umac::mibCtrl, umac::mibCtrl::resetRunt(true)
			| umac::mibCtrl::resetRx(true) | umac::mibCtrl::resetTx(true));
	space_.store(reg::umac::mibCtrl, arch::bit_value<uint32_t>{0});

	space_.store(reg::umac::maxFrameLen, 1536);

	rbufCtrl_ = space_.load(reg::rbufCtrl);
	space_.store(reg::rbufCtrl, rbufCtrl_ / rbufCtrl::align2b(true));

	space_.store(reg::bufSize, 1);

	// Disable TX and RX DMAs.
	auto txDmaCtrl = space_.load(reg::txDma::ctrl);
	arch::field<uint32_t, bool> ctrlRingEn{defaultRing + 1, 1};
	space_.store(reg::txDma::ctrl, txDmaCtrl
			/ ring::enable(false) / ctrlRingEn(false));

	auto rxDmaCtrl = space_.load(reg::rxDma::ctrl);
	space_.store(reg::rxDma::ctrl, rxDmaCtrl
			/ ring::enable(false) / ctrlRingEn(false));

	// Flush TX and RX queues.
	space_.store(reg::umac::txFlush, 1);
	co_await helix::sleepFor(10'000);
	space_.store(reg::umac::txFlush, 0);

	auto rbufFlush_ = space_.load(reg::rbufFlush);
	space_.store(reg::rbufFlush, rbufFlush_ | 1);
	co_await helix::sleepFor(10'000);
	space_.store(reg::rbufFlush, rbufFlush_);
	co_await helix::sleepFor(10'000);
}

void BcmGenetNic::updateLink_() {
	bool needsId =
		phyMode_ == nic::PhyMode::rgmiiRxid
		|| phyMode_ == nic::PhyMode::rgmiiTxid
		|| phyMode_ == nic::PhyMode::rgmiiId;

	auto oobCtrl = space_.load(reg::extRgmiiOob);
	space_.store(reg::extRgmiiOob, oobCtrl
			/ extRgmiiOob::oobDisable(false)
			/ extRgmiiOob::rgmiiLink(true)
			/ extRgmiiOob::rgmiiMode(true)
			/ extRgmiiOob::rgmiiIdDisable(!needsId));

	int speed;
	if (phy_->speed() == LinkSpeed::speed10) {
		speed = 0;
	} else if (phy_->speed() == LinkSpeed::speed100) {
		speed = 1;
	} else if (phy_->speed() == LinkSpeed::speed1000) {
		speed = 2;
	} else {
		std::println("{} Unexpected link speed >1Gbps, assuming 1Gbps ({})", this, (int)phy_->speed());
		speed = 2;
	}

	auto umacCmd = space_.load(reg::umac::cmd);
	space_.store(reg::umac::cmd, umacCmd
			/ umac::cmd::speed(speed));
}

void BcmGenetNic::setupBufs_() {
	for (size_t i = 0; i < descriptorCount; i++) {
		txBufs_.push_back(arch::dma_buffer{&dmaPool_, bufferSize});
	}
	for (size_t i = 0; i < descriptorCount; i++) {
		rxBufs_.push_back(arch::dma_buffer{&dmaPool_, bufferSize});
		barrier_.writeback(rxBufs_[i]);
		writeRxdesc_(i);
	}
}

void BcmGenetNic::writeRxdesc_(size_t idx) {
	auto spc = reg::desc::rxSubspace(space_, idx);
	auto &buf = rxBufs_[idx];
	auto ptr = helix::ptrToPhysical(buf.data());

	spc.store(reg::desc::addrLo, ptr);
	spc.store(reg::desc::addrHi, ptr >> 32);
}

void BcmGenetNic::writeTxdesc_(size_t idx, const arch::dma_buffer_view buf, arch::bit_value<uint32_t> flags) {
	auto spc = reg::desc::txSubspace(space_, idx);
	auto ptr = helix::ptrToPhysical(buf.data());

	spc.store(reg::desc::addrLo, ptr);
	spc.store(reg::desc::addrHi, ptr >> 32);
	spc.store(reg::desc::status, flags | desc::buflen(buf.size()));
}


void BcmGenetNic::setupRings_() {
	auto txRing = reg::txDma::subspace(space_, defaultRing);

	space_.store(reg::txDma::scbBurstSize, 0x08);

	txRing.store(reg::txDma::readPtrLo, 0);
	txRing.store(reg::txDma::readPtrHi, 0);
	txRing.store(reg::txDma::consIndex, 0);
	txRing.store(reg::txDma::prodIndex, 0);
	txRing.store(reg::txDma::ringBufSize,
			ring::bufLength(bufferSize) | ring::descCount(descriptorCount));
	txRing.store(reg::txDma::startAddrLo, 0);
	txRing.store(reg::txDma::startAddrHi, 0);
	txRing.store(reg::txDma::endAddrLo, descriptorCount * reg::descSize / 4 - 1);
	txRing.store(reg::txDma::endAddrHi, 0);
	txRing.store(reg::txDma::flowPeriod, 0);
	txRing.store(reg::txDma::writePtrLo, 0);
	txRing.store(reg::txDma::writePtrHi, 0);

	// IRQ after 10 packets sent or ring empty.
	txRing.store(reg::txDma::mbufDoneThres, 10);

	// Enable the default TX ring.
	arch::field<uint32_t, bool> ringCfgEn{defaultRing, 1};
	space_.store(reg::txDma::ringCfg, ringCfgEn(true));

	auto txDmaCtrl = space_.load(reg::txDma::ctrl);
	arch::field<uint32_t, bool> ctrlRingEn{defaultRing + 1, 1};
	space_.store(reg::txDma::ctrl, txDmaCtrl
			/ ring::enable(true) / ctrlRingEn(true));

	auto rxRing = reg::rxDma::subspace(space_, defaultRing);

	space_.store(reg::rxDma::scbBurstSize, 0x08);

	rxRing.store(reg::rxDma::writePtrLo, 0);
	rxRing.store(reg::rxDma::writePtrHi, 0);
	rxRing.store(reg::rxDma::consIndex, 0);
	rxRing.store(reg::rxDma::prodIndex, 0);
	rxRing.store(reg::rxDma::ringBufSize,
			ring::bufLength(bufferSize) | ring::descCount(descriptorCount));
	rxRing.store(reg::rxDma::startAddrLo, 0);
	rxRing.store(reg::rxDma::startAddrHi, 0);
	rxRing.store(reg::rxDma::endAddrLo, descriptorCount * reg::descSize / 4 - 1);
	rxRing.store(reg::rxDma::endAddrHi, 0);
	rxRing.store(reg::rxDma::xonXoffThres,
			ring::xonXoffThresHi(descriptorCount >> 4) | ring::xonXoffThresLo(5));
	rxRing.store(reg::rxDma::readPtrLo, 0);
	rxRing.store(reg::rxDma::readPtrHi, 0);

	// Configure timeout.
	rxRing.store(reg::rxDma::mbufDoneThres, 10);
	auto rxTimeoutReg = reg::rxDma::ringTimeout(defaultRing);
	space_.store(rxTimeoutReg, space_.load(rxTimeoutReg) / ring::ringTimeout(7));

	// Enable the default RX ring
	space_.store(reg::rxDma::ringCfg, ringCfgEn(true));

	auto rxDmaCtrl = space_.load(reg::rxDma::ctrl);
	space_.store(reg::rxDma::ctrl, rxDmaCtrl
			/ ring::enable(true) / ctrlRingEn(true));
}

void BcmGenetNic::writeMacAddr_() {
	uint32_t m0 = 0, m1 = 0;

	m1 |= uint32_t(mac_[5]) << 0;
	m1 |= uint32_t(mac_[4]) << 8;
	m0 |= uint32_t(mac_[3]) << 0;
	m0 |= uint32_t(mac_[2]) << 8;
	m0 |= uint32_t(mac_[1]) << 16;
	m0 |= uint32_t(mac_[0]) << 24;

	space_.store(reg::umac::mac0, m0);
	space_.store(reg::umac::mac1, m1);
}

void BcmGenetNic::setupRxFilter_() {
	arch::bit_value<uint32_t> mdfEnables{0};

	auto writeFilter = [&] (const nic::MacAddress &addr, int idx) {
		uint32_t m0 = 0, m1 = 0;

		m1 |= uint32_t(addr[5]) << 0;
		m1 |= uint32_t(addr[4]) << 8;
		m1 |= uint32_t(addr[3]) << 16;
		m1 |= uint32_t(addr[2]) << 24;
		m0 |= uint32_t(addr[0]) << 8;
		m0 |= uint32_t(addr[1]) << 0;

		space_.store(reg::umac::mdfAddrLo(idx), m0);
		space_.store(reg::umac::mdfAddrHi(idx), m1);

		arch::field<uint32_t, bool> enable{16 - idx, 1};
		mdfEnables |= enable(true);
	};

	auto cmd = space_.load(reg::umac::cmd);

	if constexpr (promiscMode) {
		cmd /= umac::cmd::promisc(true);
	} else {
		cmd /= umac::cmd::promisc(false);

		nic::MacAddress bcast{{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
		writeFilter(bcast, 0);
		writeFilter(mac_, 1);
	}

	space_.store(reg::umac::cmd, cmd);
	space_.store(reg::umac::mdf, mdfEnables);
}

async::detached BcmGenetNic::processIrqs_() {
	uint64_t sequence = 0;

	while(1) {
		auto await = co_await helix_ng::awaitEvent(irq_, sequence);
		HEL_CHECK(await.error());
		sequence = await.sequence();

		auto stat = space_.load(reg::intr::stat);
		space_.store(reg::intr::clear, stat);

		bool tx = stat & intr::txDmaDone;
		bool rx = stat & intr::rxDmaDone;
		bool mdioDone = stat & intr::mdioDone;
		bool mdioError = stat & intr::mdioError;

		if constexpr (debugIrq) {
			std::println("{} IRQ RX? {}, TX? {}, MDIO done? {}, MDIO error? {}",
					this, rx, tx, mdioDone, mdioError);
		}

		if (!rx && !tx && !mdioDone && !mdioError) {
			if constexpr (debugIrq) {
				std::println("{} IRQ NACKed with status {:08x}",
						this, (uint32_t)stat);
			}

			HEL_CHECK(helAcknowledgeIrq(irq_.getHandle(), kHelAckNack, sequence));
			continue;
		}

		if (tx) {
			processTxRing_();
		}

		if (rx) {
			processRxRing_();
		}

		if (mdioDone || mdioError) {
			std::println("{} Unexpected MDIO IRQ", this);
		}

		HEL_CHECK(helAcknowledgeIrq(irq_.getHandle(), kHelAckAcknowledge, sequence));
	}
}

void BcmGenetNic::processRxRing_() {
	auto rxRing = reg::rxDma::subspace(space_, defaultRing);
	rxPidx_ = rxRing.load(reg::rxDma::prodIndex) & 0xFFFF;
	rxEvent_.raise();
}

void BcmGenetNic::processTxRing_() {
	auto txRing = reg::txDma::subspace(space_, defaultRing);
	txCidx_ = txRing.load(reg::txDma::consIndex) & 0xFFFF;
	txEvent_.raise();
}

async::detached BcmGenetNic::processRx_() {
	auto rxRing = reg::rxDma::subspace(space_, defaultRing);

	while (true) {
		while (rxInFlight_() == 0) {
			co_await rxEvent_.async_wait();
		}

		auto idx = rxCidx_++ % descriptorCount;
		auto descSpc = reg::desc::rxSubspace(space_, idx);
		auto status = descSpc.load(reg::desc::status);

		auto retireDesc = [&] {
			writeRxdesc_(idx);
			rxRing.store(reg::rxDma::consIndex, rxCidx_);
		};

		if (status & desc::rx::allErrors) {
			std::println("{} Received packet with errors:{}{}{}{}{}",
					this,
					status & desc::rx::overrunErr ? " overrun" : "",
					status & desc::rx::crcErr ? " CRC" : "",
					status & desc::rx::rxErr ? " receive" : "",
					status & desc::rx::frameErr ? " frame" : "",
					status & desc::rx::lenErr ? " length" : "");

			retireDesc();
			continue;
		}

		auto len = status & desc::buflen;
		// Two alignment bytes + Ethernet header size
		if (len < 16) {
			std::println("{} Received packet that is too short ({} < 16)",
					this, len);
			// Discard if too short.
			retireDesc();
			continue;
		}

		len -= 2;

		auto &rxBuf = rxBufs_[idx];

		if (len > rxBuf.size() - 2) {
			std::println("{} Received packet larger than its buffer ({} > {})",
					this, len, rxBuf.size() - 2);
			retireDesc();
			continue;
		}

		barrier_.invalidate(rxBuf);

		// Skip first 2 alignment bytes prepended due to rbufCtrl::align2b.
		feedPacket(rxBuf.subview(2, len));

		retireDesc();
	}
}

async::result<void> BcmGenetNic::send(const arch::dma_buffer_view buf) {
	auto txRing = reg::txDma::subspace(space_, defaultRing);

	while (txInFlight_() == descriptorCount) {
		co_await txEvent_.async_wait();
	}

	auto idx = txPidx_++ % descriptorCount;

	auto &txBuf = txBufs_[idx];

	memcpy(txBuf.data(), buf.data(), buf.size());
	barrier_.writeback(txBuf);

	writeTxdesc_(idx, txBuf.subview(0, buf.size()),
			desc::sop(true)
			| desc::eop(true)
			| desc::tx::crc(true)
			| desc::tx::qtag(0b111111));

	txRing.store(reg::txDma::prodIndex, txPidx_);
}


async::result<std::shared_ptr<nic::Link>> makeShared(mbus_ng::EntityId entity) {
	auto mbusEntity = co_await mbus_ng::Instance::global().getEntity(entity);
	auto device = protocols::hw::Device((co_await mbusEntity.getRemoteLane()).unwrap());

	auto dtInfo = co_await device.getDtInfo();

	auto reg = co_await device.accessDtRegister(0);
	auto irq = co_await device.installDtIrq(0);

	auto phyModeProp = co_await device.getDtProperty("phy-mode");
	if (!phyModeProp) {
		std::println("bcmgenet: DT node is missing \"phy-mode\" property!");
		co_return nullptr;
	}
	auto phyModeStr = *phyModeProp->asString();

	nic::PhyMode phyMode;
	if (phyModeStr == "rgmii") {
		phyMode = nic::PhyMode::rgmii;
	} else if (phyModeStr == "rgmii-rxid") {
		phyMode = nic::PhyMode::rgmiiRxid;
	} else if (phyModeStr == "rgmii-txid") {
		phyMode = nic::PhyMode::rgmiiTxid;
	} else if (phyModeStr == "rgmii-id") {
		phyMode = nic::PhyMode::rgmiiId;
	} else {
		std::println("bcmgenet: DT node has unsupported \"phy-mode\" value: \"{}\"!",
				std::string_view{phyModeStr.data(), phyModeStr.size()});
		co_return nullptr;
	}

	auto macAddrProp = co_await device.getDtProperty("mac-address");
	if (!macAddrProp) {
		macAddrProp = co_await device.getDtProperty("local-mac-address");
		if (!macAddrProp) {
			macAddrProp = co_await device.getDtProperty("address");
			if (!macAddrProp) {
				std::println("bcmgenet: DT node is missing \"mac-address\", \"local-mac-address\", or \"address\" property!");
				co_return nullptr;
			} else {
				std::println("bcmgenet: Using \"address\" property");
			}
		} else {
			std::println("bcmgenet: Using \"local-mac-address\" property");
		}
	} else {
		std::println("bcmgenet: Using \"mac-address\" property");
	}

	nic::MacAddress macAddr;
	{
		if (macAddrProp->size() != 6) {
			std::println("bcmgenet: MAC address property is too short ({} != 6)!",
					macAddrProp->size());
			co_return nullptr;
		}

		std::array<uint8_t, 6> macBytes;
		std::ranges::copy(macAddrProp->data(), macBytes.begin());
		macAddr = nic::MacAddress{macBytes};
	}

	auto mapping = helix::Mapping{std::move(reg),
		dtInfo.regs[0].offset, dtInfo.regs[0].length};
	auto nic = std::make_shared<BcmGenetNic>(dtInfo.regs[0].address,
			std::move(device), std::move(mapping),
			std::move(irq), phyMode, macAddr);

	co_await nic->initialize();

	co_return nic;
}

} // namespace nic::k1x_emac
