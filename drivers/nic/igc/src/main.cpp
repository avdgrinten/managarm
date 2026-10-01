#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <helix/ipc.hpp>
#include <print>

#include "main.hpp"
#include "regs.hpp"

namespace nic::igc {

IgcNic::IgcNic(protocols::hw::Device device, helix::UniqueDescriptor dmaSpace, bool iommuActive)
: nic::Link(mtuSize, &dmaPool_),
  dmaPool_{&dmaRealm_, {.addressBits = 64, .allocateContigous = !iommuActive}},
  dmaSpaceHandle_{std::move(dmaSpace)},
  dmaSpace_{dmaRealm_.attachDmaSpace(dmaSpaceHandle_, iommuActive)},
  device_{std::move(device)} {}

async::result<void> IgcNic::reset() {
	space_.store(reg::imc, ~uint32_t{0});
	space_.load(reg::icr);

	space_.store(reg::rctl, arch::bit_value<uint32_t>{0});
	space_.store(reg::tctl, arch::bit_value<uint32_t>{0} / tctl::psp(true));
	wrfl();
	co_await helix::sleepFor(10'000'000);

	space_.store(reg::ctrl, space_.load(reg::ctrl) / ctrl::gioMasterDisable(true));
	if (!co_await helix::kindaBusyWait(800'000'000, [this] {
		    return !(space_.load(reg::status) & status::gioMasterEnable);
	    }))
		std::println("igc: PCIe master disable timed out, resetting anyway");

	space_.store(reg::ctrl, space_.load(reg::ctrl) / ctrl::rst(true));
	co_await helix::sleepFor(1'000'000);

	if (!co_await helix::kindaBusyWait(20'000'000, [this] {
		    return bool(space_.load(reg::eecd) & eecd::autoRd);
	    }))
		std::println("igc: NVM auto-read did not complete after reset");

	space_.store(reg::imc, ~uint32_t{0});
	space_.load(reg::icr);
}

void IgcNic::readMac(std::array<uint8_t, 6> &out) {
	uint32_t ral = space_.load(reg::ral0);
	uint32_t rah = space_.load(reg::rah0);
	out[0] = ral;
	out[1] = ral >> 8;
	out[2] = ral >> 16;
	out[3] = ral >> 24;
	out[4] = rah;
	out[5] = rah >> 8;
}

void IgcNic::initRxAddrs(const std::array<uint8_t, 6> &mac) {
	uint32_t ralVal = uint32_t(mac[0]) | (uint32_t(mac[1]) << 8) | (uint32_t(mac[2]) << 16)
	                  | (uint32_t(mac[3]) << 24);
	auto rahVal = rah::mac4(mac[4]) / rah::mac5(mac[5]) / rah::av(true);

	space_.store(reg::ral0, ralVal);
	wrfl();
	space_.store(reg::rah0, static_cast<uint32_t>(rahVal));
	wrfl();

	for (size_t n = 1; n < reg::rarCount; n++) {
		space_.store(reg::ral(n), 0);
		wrfl();
		space_.store(reg::rah(n), 0);
		wrfl();
	}

	for (size_t i = 0; i < reg::mtaCount; i++)
		space_.store(reg::mta(i), 0);
	wrfl();
}

async::result<bool> IgcNic::getHwSemaphore() {
	auto smbiFree = [this] { return !(space_.load(reg::swsm) & swsm::smbi); };
	if (!co_await helix::kindaBusyWait(100'000'000, smbiFree)) {
		std::println("igc: SMBI stuck, force-releasing hardware semaphore");
		putHwSemaphore();
		if (!co_await helix::kindaBusyWait(100'000'000, smbiFree))
			co_return false;
	}

	bool ok = helix::busyWaitUntil(100'000'000, [this] {
		space_.store(reg::swsm, space_.load(reg::swsm) / swsm::swesmbi(true));
		return bool(space_.load(reg::swsm) & swsm::swesmbi);
	});
	if (!ok) {
		putHwSemaphore();
		co_return false;
	}
	co_return true;
}

void IgcNic::putHwSemaphore() {
	space_.store(reg::swsm, space_.load(reg::swsm) / swsm::smbi(false) / swsm::swesmbi(false));
}

async::result<bool> IgcNic::acquireSwfwSync(uint32_t mask) {
	uint32_t fwmask = mask << 16;
	for (int i = 0; i < 200; i++) {
		if (!co_await getHwSemaphore())
			co_return false;

		uint32_t swfwSync = space_.load(reg::swFwSync);
		if (!(swfwSync & (mask | fwmask))) {
			space_.store(reg::swFwSync, swfwSync | mask);
			putHwSemaphore();
			co_return true;
		}

		putHwSemaphore();
		co_await helix::sleepFor(5'000'000);
	}
	co_return false;
}

async::result<void> IgcNic::releaseSwfwSync(uint32_t mask) {
	if (!co_await getHwSemaphore()) {
		std::println("igc: Failed to take semaphore for SW_FW_SYNC release");
		co_return;
	}
	uint32_t swfwSync = space_.load(reg::swFwSync);
	space_.store(reg::swFwSync, swfwSync & ~mask);
	putHwSemaphore();
}

async::result<std::expected<uint16_t, int>> IgcNic::mdicRead(uint8_t phyReg) {
	space_.store(reg::mdic, mdic::regAdd(phyReg) / mdic::op(mdic::opRead));
	co_return co_await mdicWait();
}

async::result<std::expected<void, int>> IgcNic::mdicWrite(uint8_t phyReg, uint16_t value) {
	space_.store(reg::mdic, mdic::data(value) / mdic::regAdd(phyReg) / mdic::op(mdic::opWrite));
	auto res = co_await mdicWait();
	if (!res)
		co_return std::unexpected(res.error());
	co_return {};
}

async::result<std::expected<uint16_t, int>> IgcNic::mdicWait() {
	arch::bit_value<uint32_t> last{0};
	bool ready = helix::busyWaitUntil(100'000'000, [this, &last] {
		last = space_.load(reg::mdic);
		return bool(last & mdic::ready);
	});
	if (!ready)
		co_return std::unexpected(ETIMEDOUT);
	if (last & mdic::error)
		co_return std::unexpected(EIO);
	co_return uint16_t(last & mdic::data);
}

async::result<void> IgcNic::phyPowerUpAutoneg() {
	if (!co_await acquireSwfwSync(swfwPhy0Sm)) {
		std::println("igc: PHY restart failed: could not acquire SWFW sync");
		co_return;
	}

	auto bmcr = co_await mdicRead(0);
	if (bmcr) {
		if (*bmcr & mii::crPowerDown)
			std::println("igc: PHY was powered down, powering up");
		auto res = co_await mdicWrite(
		    0, (*bmcr & ~mii::crPowerDown) | mii::crAutoNegEn | mii::crRestartAutoNeg
		);
		if (!res)
			std::println("igc: PHY restart failed");
	} else {
		std::println("igc: PHY restart failed");
	}

	co_await releaseSwfwSync(swfwPhy0Sm);
}

void IgcNic::setupLink() {
	space_.store(
	    reg::ctrl,
	    space_.load(reg::ctrl) / ctrl::slu(true) / ctrl::frcspd(false) / ctrl::frcdpx(false)
	);
}

void IgcNic::linkStatus(bool &up, uint32_t &speed, bool &fullDuplex) {
	auto s = space_.load(reg::status);
	switch (s & status::speed) {
		case 0:
			speed = 10;
			break;
		case 1:
			speed = 100;
			break;
		default:
			speed = (s & status::speed2500) ? 2500 : 1000;
			break;
	}
	up = bool(s & status::lu);
	fullDuplex = bool(s & status::fd);
}

async::result<void> IgcNic::init() {
	auto info = co_await device_.getPciInfo();

	uint32_t off = 0x100;
	for (int i = 0; i < 64; i++) {
		uint32_t hdr = co_await device_.loadPciSpace(off, 4);
		if (hdr == 0 || hdr == ~uint32_t{0})
			break;
		if ((hdr & 0xFFFF) == 0x001E) {
			uint32_t ctl1 = co_await device_.loadPciSpace(off + 8, 4);
			if (ctl1 & 0x5)
				co_await device_.storePciSpace(off + 8, 4, ctl1 & ~uint32_t{0x5});
			break;
		}
		off = hdr >> 20;
		if (off < 0x100 || (off & 3))
			break;
	}

	auto &barInfo = info.barInfo[0];
	assert(barInfo.ioType == protocols::hw::IoType::kIoTypeMemory);
	assert(barInfo.length >= 0x1'0000);
	auto bar0 = co_await device_.accessBar(0);

	mmioMapping_ = {bar0, barInfo.offset, barInfo.length};
	space_ = arch::mem_space{mmioMapping_.get()};

	co_await reset();

	co_await device_.enableMsi();
	irq_ = co_await device_.installMsi(0);

	std::array<uint8_t, 6> macBytes;
	readMac(macBytes);
	if (macBytes == std::array<uint8_t, 6>{0, 0, 0, 0, 0, 0}
	    || macBytes == std::array<uint8_t, 6>{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}) {
		std::println("igc: No valid MAC address in NVM");
		co_return;
	}

	for (size_t i = 0; i < 6; i++)
		mac_[i] = macBytes[i];
	std::println("igc: MAC address {}", mac_);

	initRxAddrs(macBytes);

	co_await phyPowerUpAutoneg();
	setupLink();

	rxDescs_ = arch::dma_array<RxDescriptor>(&dmaPool_, ringSize);
	rxBufs_ = arch::dma_array<Buffer>(&dmaPool_, ringSize);
	co_await dmaSpace_.ensure_mapped(rxDescs_.view_buffer());
	co_await dmaSpace_.ensure_mapped(rxBufs_.view_buffer());
	for (size_t i = 0; i < ringSize; i++)
		rxBufIova_.push_back(dmaSpace_.iova_of(rxBufs_.object_view(i)));
	for (size_t i = 0; i < ringSize; i++)
		armRx(i);

	uint64_t rxPhys = dmaSpace_.iova_of(rxDescs_.view_buffer());
	space_.store(reg::rxdctl0, arch::bit_value<uint32_t>{0});
	space_.store(reg::rdbal0, uint32_t(rxPhys));
	space_.store(reg::rdbah0, uint32_t(rxPhys >> 32));
	space_.store(reg::rdlen0, ringSize * sizeof(RxDescriptor));
	space_.store(reg::rdh0, 0);
	space_.store(reg::rdt0, 0);
	space_.store(
	    reg::srrctl0,
	    arch::bit_value<uint32_t>{0} / srrctl0::bsizepkt(bufferSize >> 10)
	        / srrctl0::bsizehdr(256 >> 6) / srrctl0::desctype(srrctl0::desctypeAdvOnebuf)
	);
	space_.store(
	    reg::rxdctl0,
	    arch::bit_value<uint32_t>{0} / rxdctl0::pthresh(8) / rxdctl0::hthresh(8)
	        / rxdctl0::wthresh(4) / rxdctl0::queueEnable(true)
	);

	if (!helix::busyWaitUntil(10'000'000, [this] {
		    return bool(space_.load(reg::rxdctl0) & rxdctl0::queueEnable);
	    }))
		std::println("igc: RX queue 0 did not enable");

	space_.store(
	    reg::rctl,
	    arch::bit_value<uint32_t>{0} / rctl::en(true) / rctl::bam(true) / rctl::secrc(true)
	);
	wrfl();
	__sync_synchronize();
	space_.store(reg::rdt0, ringSize - 1);

	txDescs_ = arch::dma_array<TxDescriptor>(&dmaPool_, ringSize);
	txBufs_ = arch::dma_array<Buffer>(&dmaPool_, ringSize);
	co_await dmaSpace_.ensure_mapped(txDescs_.view_buffer());
	co_await dmaSpace_.ensure_mapped(txBufs_.view_buffer());
	for (size_t i = 0; i < ringSize; i++)
		txBufIova_.push_back(dmaSpace_.iova_of(txBufs_.object_view(i)));

	uint64_t txPhys = dmaSpace_.iova_of(txDescs_.view_buffer());
	space_.store(reg::txdctl0, arch::bit_value<uint32_t>{0});
	wrfl();
	space_.store(reg::tdbal0, uint32_t(txPhys));
	space_.store(reg::tdbah0, uint32_t(txPhys >> 32));
	space_.store(reg::tdlen0, ringSize * sizeof(TxDescriptor));
	space_.store(reg::tdh0, 0);
	space_.store(reg::tdt0, 0);
	space_.store(
	    reg::tctl,
	    arch::bit_value<uint32_t>{0} / tctl::en(true) / tctl::psp(true)
	        / tctl::ct(tctl::collisionThreshold) / tctl::rtlc(true)
	);
	space_.store(
	    reg::txdctl0,
	    arch::bit_value<uint32_t>{0} / txdctl0::pthresh(8) / txdctl0::hthresh(1)
	        / txdctl0::wthresh(0) / txdctl0::queueEnable(true)
	);
	if (!helix::busyWaitUntil(10'000'000, [this] {
		    return bool(space_.load(reg::txdctl0) & txdctl0::queueEnable);
	    }))
		std::println("igc: TX queue 0 did not enable");

	space_.store(
	    reg::gpie,
	    arch::bit_value<uint32_t>{0} / gpie::nsicr(true) / gpie::msixMode(true) / gpie::eiame(true)
	        / gpie::pba(true)
	);
	space_.store(
	    reg::ivar0, arch::bit_value<uint32_t>{0} / ivar0::rxQ0(ivarValid) / ivar0::txQ0(ivarValid)
	);
	space_.store(reg::ivarMisc, arch::bit_value<uint32_t>{0} / ivarMisc::other(ivarValid));
	space_.store(reg::eitr0, startItr);

	space_.load(reg::icr);
	space_.store(reg::eiac, 1);
	space_.store(reg::eiam, 1);
	space_.store(reg::eims, 1);
	space_.store(reg::ims, arch::bit_value<uint32_t>{0} / ims::lsc(true));
	wrfl();

	processIrqs();
}

async::detached IgcNic::processIrqs() {
	uint64_t sequence = 0;
	while (true) {
		auto await = co_await helix_ng::awaitEvent(irq_, sequence);
		HEL_CHECK(await.error());
		sequence = await.sequence();

		auto icrVal = space_.load(reg::icr);
		HEL_CHECK(helAcknowledgeIrq(irq_.getHandle(), kHelAckAcknowledge, sequence));

		if (icrVal & icr::lsc) {
			bool up;
			uint32_t speed;
			bool fullDuplex;
			linkStatus(up, speed, fullDuplex);
			if (up)
				std::println(
				    "igc: Link up, {} Mb/s, {} duplex", speed, fullDuplex ? "full" : "half"
				);
			else
				std::println("igc: Link down");
		}

		space_.store(reg::eims, 1);

		rxEvent_.raise();
	}
}

async::result<size_t> IgcNic::receive(arch::dma_buffer_view frame) {
	while (true) {
		size_t i = rxNtc_;
		__sync_synchronize();
		uint32_t status = rxDescs_[i].statusError;
		if (status & desc::rxdStatDd) {
			uint16_t len = rxDescs_[i].length;
			size_t n = std::min({size_t(len), frame.size(), bufferSize});
			memcpy(frame.data(), rxBufs_[i].data, n);

			armRx(i);
			__sync_synchronize();
			space_.store(reg::rdt0, i);
			rxNtc_ = (i + 1) % ringSize;

			if (!(status & desc::rxdStatEop)) {
				std::println("igc: Dropping RX frame without EOP");
				continue;
			}
			co_return n;
		}
		co_await rxEvent_.async_wait();
	}
}

async::result<void> IgcNic::send(const arch::dma_buffer_view frame) {
	if (frame.size() == 0 || frame.size() > maxFrameLen)
		co_return;

	reapTx();
	if (txOutstanding_ == ringSize - 1) {
		if (!co_await helix::kindaBusyWait(10'000'000, [this] {
			    reapTx();
			    return txOutstanding_ < ringSize - 1;
		    }))
			co_return;
	}

	size_t i = txNtu_;
	memcpy(txBufs_[i].data, frame.data(), frame.size());
	txDescs_[i].addr = txBufIova_[i];
	txDescs_[i].cmdTypeLen = uint32_t(frame.size()) | desc::advtxdDtypData | desc::advtxdDcmdDext
	                         | desc::advtxdDcmdIfcs | desc::advtxdDcmdEop | desc::advtxdDcmdRs;
	txDescs_[i].olinfoStatus = uint32_t(frame.size()) << desc::advtxdPaylenShift;

	txNtu_ = (i + 1) % ringSize;
	++txOutstanding_;
	__sync_synchronize();
	space_.store(reg::tdt0, txNtu_);
	co_return;
}

async::result<std::shared_ptr<nic::Link>> makeShared(protocols::hw::Device device) {
	co_await device.enableBusmaster();

	co_await device.enableDma(false);
	auto [iommuActive, dmaSpace, _] = co_await device.getDmaSpace();

	auto nic = std::make_shared<IgcNic>(std::move(device), std::move(dmaSpace), iommuActive);
	co_await nic->init();
	co_return std::move(nic);
}

} // namespace nic::igc
