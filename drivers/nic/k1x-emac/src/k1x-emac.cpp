#include <print>

#include <arpa/inet.h>

#include <arch/barrier.hpp>
#include <arch/mem_space.hpp>
#include <arch/variable.hpp>
#include <async/mutex.hpp>
#include <async/recurring-event.hpp>
#include <helix/memory.hpp>
#include <helix/timer.hpp>
#include <netserver/phy.hpp>
#include <nic/k1x-emac/k1x-emac.hpp>
#include <protocols/hw/client.hpp>

namespace {

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-const-variable"

namespace dma {
constexpr arch::bit_register<uint32_t> configuration{0x00};
constexpr arch::bit_register<uint32_t> control{0x04};
constexpr arch::bit_register<uint32_t> statusIrq{0x08};
constexpr arch::bit_register<uint32_t> interruptEnable{0x0c};
constexpr arch::scalar_register<uint32_t> transmitAutoPollCounter{0x10};
constexpr arch::scalar_register<uint32_t> transmitPollDemand{0x14};
constexpr arch::scalar_register<uint32_t> transmitBaseAddress{0x1c};
constexpr arch::scalar_register<uint32_t> receiveBaseAddress{0x20};
} // namespace dma

namespace mac {
constexpr arch::bit_register<uint32_t> globalControl{0x100};
constexpr arch::bit_register<uint32_t> transmitControl{0x104};
constexpr arch::bit_register<uint32_t> receiveControl{0x108};
constexpr arch::bit_register<uint32_t> macAddressControl{0x118};

constexpr arch::scalar_register<uint32_t> macAddressHigh{0x120};
constexpr arch::scalar_register<uint32_t> macAddressMedium{0x124};
constexpr arch::scalar_register<uint32_t> macAddressLow{0x128};

constexpr arch::scalar_register<uint32_t> multicastHashTable1{0x150};
constexpr arch::scalar_register<uint32_t> multicastHashTable2{0x154};
constexpr arch::scalar_register<uint32_t> multicastHashTable3{0x158};
constexpr arch::scalar_register<uint32_t> multicastHashTable4{0x15c};

constexpr arch::bit_register<uint32_t> mdioControl{0x1a0};
constexpr arch::scalar_register<uint32_t> mdioData{0x1a4};

constexpr arch::scalar_register<uint32_t> transmitFifoAlmostFull{0x1c0};
constexpr arch::scalar_register<uint32_t> transmitPacketStartThreshold{0x1c4};
constexpr arch::scalar_register<uint32_t> receivePacketStartThreshold{0x1c8};
constexpr arch::scalar_register<uint32_t> interruptEnable{0x1e4};
} // namespace mac

namespace regs {
namespace dma::configuration {
constexpr arch::field<uint32_t, bool> softwareReset{0, 1};
constexpr arch::field<uint32_t, uint8_t> burstLength{1, 7};
constexpr arch::field<uint32_t, bool> waitForDone{16, 1};
constexpr arch::field<uint32_t, bool> strictBurst{17, 1};
constexpr arch::field<uint32_t, bool> dma64Bit{18, 1};
} // namespace dma::configuration

namespace dma::control {
constexpr arch::field<uint32_t, bool> startStopTransmitDma{0, 1};
constexpr arch::field<uint32_t, bool> startStopReceiveDma{1, 1};
} // namespace dma::control

namespace dma::interruptEnable {
constexpr arch::field<uint32_t, bool> txTransferDoneIntrEnable{0, 1};
constexpr arch::field<uint32_t, bool> txDescUnavailableIntrEnable{1, 1};
constexpr arch::field<uint32_t, bool> txDmaStoppedIntrEnable{2, 1};
constexpr arch::field<uint32_t, bool> rxTransferDoneIntrEnable{4, 1};
constexpr arch::field<uint32_t, bool> rxDescUnavailableIntrEnable{5, 1};
constexpr arch::field<uint32_t, bool> rxDmaStoppedIntrEnable{6, 1};
constexpr arch::field<uint32_t, bool> rxMissedFrameIntrEnable{7, 1};
} // namespace dma::interruptEnable

namespace dma::status {
constexpr arch::field<uint32_t, uint8_t> receiveDmaState{20, 4};
constexpr arch::field<uint32_t, uint8_t> transmitDmaState{16, 3};

constexpr arch::field<uint32_t, bool> ptpIrq{9, 1};
constexpr arch::field<uint32_t, bool> macIrq{8, 1};
constexpr arch::field<uint32_t, bool> rxMissedFrameIrq{7, 1};

constexpr arch::field<uint32_t, bool> rxDmaStoppedIrq{6, 1};
constexpr arch::field<uint32_t, bool> rxDescUnavailableIrq{5, 1};
constexpr arch::field<uint32_t, bool> rxDoneIrq{4, 1};

constexpr arch::field<uint32_t, bool> txDmaStoppedIrq{2, 1};
constexpr arch::field<uint32_t, bool> txDescUnavailableIrq{1, 1};
constexpr arch::field<uint32_t, bool> txDoneIrq{0, 1};
} // namespace dma::status

namespace mac::globalControl {
constexpr arch::field<uint32_t, bool> speed100{0, 1};
constexpr arch::field<uint32_t, bool> speed1000{1, 1};
constexpr arch::field<uint32_t, bool> fullDuplex{2, 1};
constexpr arch::field<uint32_t, bool> resetRxStatCounters{3, 1};
constexpr arch::field<uint32_t, bool> resetTxStatCounters{4, 1};
} // namespace mac::globalControl

namespace mac::transmitControl {
constexpr arch::field<uint32_t, bool> enable{0, 1};
constexpr arch::field<uint32_t, bool> autoRetry{3, 1};
} // namespace mac::transmitControl

namespace mac::receiveControl {
constexpr arch::field<uint32_t, bool> enable{0, 1};
constexpr arch::field<uint32_t, bool> storeForward{3, 1};
} // namespace mac::receiveControl

namespace mac::addressControl {
constexpr arch::field<uint32_t, bool> macAddress1Enable{0, 1};
constexpr arch::field<uint32_t, bool> macAddress2Enable{1, 1};
constexpr arch::field<uint32_t, bool> macAddress3Enable{2, 1};
constexpr arch::field<uint32_t, bool> macAddress4Enable{3, 1};
constexpr arch::field<uint32_t, bool> inverseMacAddress1Enable{4, 1};
constexpr arch::field<uint32_t, bool> inverseMacAddress2Enable{5, 1};
constexpr arch::field<uint32_t, bool> inverseMacAddress3Enable{6, 1};
constexpr arch::field<uint32_t, bool> inverseMacAddress4Enable{7, 1};
constexpr arch::field<uint32_t, bool> promiscuousModeEnable{8, 1};
} // namespace mac::addressControl

namespace mac::mdioControl {
constexpr arch::field<uint32_t, uint8_t> phyAddress{0, 5};
constexpr arch::field<uint32_t, uint8_t> registerAddress{5, 5};
constexpr arch::field<uint32_t, bool> readOperation{10, 1};
constexpr arch::field<uint32_t, bool> startTransaction{15, 1};
} // namespace mac::mdioControl

namespace rxStatus {
constexpr uint32_t runt = 1 << 1;
constexpr uint32_t checksumError = 1 << 6;
constexpr uint32_t maxLengthError = 1 << 7;
constexpr uint32_t jabberError = 1 << 8;
constexpr uint32_t lengthError = 1 << 9;
} // namespace rxStatus

namespace rxDesc1 {
constexpr arch::field<uint32_t, uint32_t> framePacketLength{0, 14};
constexpr arch::field<uint32_t, uint32_t> applicationStatus{14, 15};
constexpr arch::field<uint32_t, bool> lastDescriptor{29, 1};
constexpr arch::field<uint32_t, bool> firstDescriptor{30, 1};
constexpr arch::field<uint32_t, bool> own{31, 1};
} // namespace rxDesc1

namespace rxDesc2 {
constexpr arch::field<uint32_t, uint32_t> bufferSize1{0, 12};
constexpr arch::field<uint32_t, uint32_t> bufferSize2{12, 12};
constexpr arch::field<uint32_t, bool> secondAddressChained{25, 1};
constexpr arch::field<uint32_t, bool> endOfRing{26, 1};
constexpr arch::field<uint32_t, bool> rxTimestamp{30, 1};
constexpr arch::field<uint32_t, bool> ptpPacket{31, 1};
} // namespace rxDesc2

namespace txDesc1 {
constexpr arch::field<uint32_t, uint32_t> framePacketStatus{0, 30};
constexpr arch::field<uint32_t, bool> txTimestamp{30, 1};
constexpr arch::field<uint32_t, bool> own{31, 1};
} // namespace txDesc1

namespace txDesc2 {
constexpr arch::field<uint32_t, uint32_t> bufferSize1{0, 12};
constexpr arch::field<uint32_t, uint32_t> bufferSize2{12, 12};
constexpr arch::field<uint32_t, bool> forceEopError{24, 1};
constexpr arch::field<uint32_t, bool> secondAddressChained{25, 1};
constexpr arch::field<uint32_t, bool> endOfRing{26, 1};
constexpr arch::field<uint32_t, bool> disablePadding{27, 1};
constexpr arch::field<uint32_t, bool> addCrcDisable{28, 1};
constexpr arch::field<uint32_t, bool> firstSegment{29, 1};
constexpr arch::field<uint32_t, bool> lastSegment{30, 1};
constexpr arch::field<uint32_t, bool> interruptOnCompletion{31, 1};
} // namespace txDesc2
} // namespace regs

#pragma GCC diagnostic pop // -Wunused-const-variable

constexpr bool debugIrqs = false;
constexpr bool debugRxTx = false;

constexpr size_t mtuSize = 1500;
constexpr size_t ethernetHeaderSize = 14;
constexpr size_t fcsLength = 4;

struct EmacDescriptor {
	arch::bit_variable<uint32_t> data1;
	arch::bit_variable<uint32_t> data2;
	arch::scalar_variable<uint32_t> bufferAddr1;
	arch::scalar_variable<uint32_t> bufferAddr2;
};

constexpr size_t bufferSize = mtuSize + ethernetHeaderSize + fcsLength;
constexpr size_t dmaBufferSize = (bufferSize + 0x3f) & ~UINT64_C(0x3f);

constexpr size_t dcacheLineSize = 64;
constexpr size_t descriptorsPerCacheLine = dcacheLineSize / sizeof(EmacDescriptor);

struct EmacDescriptorRing {
	EmacDescriptorRing() = default;

	EmacDescriptorRing(arch::dma_array<EmacDescriptor> descriptors)
	: descriptors(std::move(descriptors)),
	  head(0),
	  tail(0) {}

	arch::dma_array<EmacDescriptor> descriptors;
	size_t head{0};
	size_t tail{0};
};

struct K1xEmacMii : nic::Mdio {
	K1xEmacMii(void *mmioMapping) : _mmioMapping{mmioMapping}, _mmio{mmioMapping} {}

	async::result<std::expected<uint16_t, nic::PhyError>>
	read(uint8_t phyAddress, uint8_t registerNum) override;

	async::result<std::expected<void, nic::PhyError>>
	write(uint8_t phyAddress, uint8_t registerNum, uint16_t value) override;

private:
	void *_mmioMapping;
	arch::mem_space _mmio;
	arch::dma_barrier _barrier{false};
};

struct K1xEmacNic : nic::Link {
	K1xEmacNic(protocols::hw::Device device, helix_ng::Mapping mapping, helix::UniqueDescriptor irq)
	: nic::Link{mtuSize, &_dmaPool},
	  _device{std::move(device)},
	  _mmioMapping{std::move(mapping)},
	  _irq{std::move(irq)},
	  _mmio{_mmioMapping.get()},
	  _mii{std::make_shared<K1xEmacMii>(_mmioMapping.get())} {}

	async::result<void> send(const arch::dma_buffer_view) override;

	async::result<bool> initialize();

private:
	// Clean up RX descriptors starting from the given index.
	void cleanRxDescriptors(size_t index);

	async::detached processIrqs();
	async::detached processRx();

	// Returns the frame of a released RX descriptor, or nothing if it has to be dropped.
	std::optional<arch::dma_buffer_view> checkRxDescriptor(EmacDescriptor &rxDesc,
			arch::dma_buffer_view rxBuffer);

private:
	protocols::hw::Device _device;
	helix_ng::Mapping _mmioMapping;
	helix::UniqueDescriptor _irq;
	arch::dma_barrier _barrier{false};
	arch::mem_space _mmio;
	arch::dma_realm _dmaRealm;
	arch::contiguous_pool _dmaPool{&_dmaRealm};
	EmacDescriptorRing _rxRing;
	EmacDescriptorRing _txRing;
	arch::dma_buffer _rxBuffer;
	arch::dma_buffer _txBuffer;
	async::recurring_event _rxEvent;
	async::recurring_event _txEvent;
	async::mutex _txMutex;
	std::shared_ptr<K1xEmacMii> _mii;
	std::shared_ptr<nic::EthernetPhy> _phy;
};

async::result<std::expected<uint16_t, nic::PhyError>>
K1xEmacMii::read(uint8_t phyAddress, uint8_t registerNum) {
	_mmio.store(
	    mac::mdioControl,
	    regs::mac::mdioControl::phyAddress(phyAddress)
	        | regs::mac::mdioControl::registerAddress(registerNum)
	        | regs::mac::mdioControl::readOperation(true)
	        | regs::mac::mdioControl::startTransaction(true)
	);

	// Wait for the transaction to complete.
	auto result = co_await helix::kindaBusyWait(1'000'000'000, [&] {
		return !(_mmio.load(mac::mdioControl) & regs::mac::mdioControl::startTransaction);
	});

	if (!result) {
		co_return std::unexpected{nic::PhyError::timeout};
	}

	_barrier.invalidate((uint8_t *)_mmioMapping + mac::mdioData.offset(), sizeof(uint32_t));

	co_return _mmio.load(mac::mdioData) & 0xffff;
}

async::result<std::expected<void, nic::PhyError>>
K1xEmacMii::write(uint8_t phyAddress, uint8_t registerNum, uint16_t value) {
	_mmio.store(mac::mdioData, value & 0xffff);
	_mmio.store(
	    mac::mdioControl,
	    regs::mac::mdioControl::phyAddress(phyAddress)
	        | regs::mac::mdioControl::registerAddress(registerNum)
	        | regs::mac::mdioControl::readOperation(false)
	        | regs::mac::mdioControl::startTransaction(true)
	);

	_barrier.writeback((uint8_t *)_mmioMapping + mac::mdioData.offset(), sizeof(uint32_t));

	// Wait for the transaction to complete.
	auto result = co_await helix::kindaBusyWait(1'000'000'000, [&] {
		return !(_mmio.load(mac::mdioControl) & regs::mac::mdioControl::startTransaction);
	});

	if (!result) {
		co_return std::unexpected{nic::PhyError::timeout};
	}

	co_return {};
}

async::detached K1xEmacNic::processRx() {
	while (true) {
		auto index = _rxRing.head;

		auto &rxDesc = _rxRing.descriptors[index];
		_barrier.invalidate(&rxDesc, sizeof(rxDesc));

		if (rxDesc.data1.load() & regs::rxDesc1::own) {
			// The descriptor is still owned by the DMA, wait for it to be released.
			co_await _rxEvent.async_wait();
			continue;
		}

		// Advance the RX head.
		_rxRing.head = (_rxRing.head + 1) % _rxRing.descriptors.size();

		// Get the RX buffer for the current descriptor.
		auto rxBuffer = _rxBuffer.subview(index * dmaBufferSize, bufferSize);

		auto frame = checkRxDescriptor(rxDesc, rxBuffer);
		if (frame) {
			_barrier.invalidate(rxBuffer.data(), rxBuffer.size());
			feedPacket(*frame);
		}

		// Dropped frames have to return their descriptors as well.
		if ((index + 1) % descriptorsPerCacheLine == 0) {
			cleanRxDescriptors(index + 1 - descriptorsPerCacheLine);
		}
	}
}

std::optional<arch::dma_buffer_view>
K1xEmacNic::checkRxDescriptor(EmacDescriptor &rxDesc, arch::dma_buffer_view rxBuffer) {
	// Perform sanity checks on the RX descriptor.
	if (!(rxDesc.data1.load() & regs::rxDesc1::lastDescriptor)) {
		std::println("k1x-emac: rx descriptor last descriptor bit not set, discarding");
		return std::nullopt;
	}

	auto status = rxDesc.data1.load() & regs::rxDesc1::applicationStatus;
	auto length = rxDesc.data1.load() & regs::rxDesc1::framePacketLength;
	if (status & regs::rxStatus::runt) {
		std::println("k1x-emac: received rx frame is less than 64 bytes, discarding");
		return std::nullopt;
	}
	if (status & regs::rxStatus::checksumError) {
		std::println("k1x-emac: received rx frame has CRC errors, discarding");
		return std::nullopt;
	}
	if (status & regs::rxStatus::maxLengthError) {
		std::println("k1x-emac: received rx frame exceeds maximum length, discarding");
		return std::nullopt;
	}
	if (status & regs::rxStatus::jabberError) {
		std::println("k1x-emac: received rx frame has been truncated, discarding");
		return std::nullopt;
	}
	if (length <= fcsLength || length > rxBuffer.size()) {
		std::println("k1x-emac: received rx frame is too short or too long, discarding");
		return std::nullopt;
	}

	return rxBuffer.subview(0, length - fcsLength);
}

async::result<void> K1xEmacNic::send(const arch::dma_buffer_view frame) {
	co_await _txMutex.async_lock();
	frg::unique_lock lock{frg::adopt_lock, _txMutex};

	auto buffer = _txBuffer.subview(_txRing.head * dmaBufferSize, bufferSize);
	memcpy(buffer.data(), frame.data(), frame.size());
	_barrier.writeback(buffer);

	auto &txDesc = _txRing.descriptors[_txRing.head];
	memset(&txDesc, 0, sizeof(txDesc));

	txDesc.data2.store(
	    regs::txDesc2::bufferSize1(buffer.size()) | regs::txDesc2::firstSegment(true)
	    | regs::txDesc2::lastSegment(true) | regs::txDesc2::interruptOnCompletion(true)
	    | regs::txDesc2::endOfRing(_txRing.head == _txRing.descriptors.size() - 1)
	);
	txDesc.bufferAddr1.store(helix::ptrToPhysical(buffer.data()));

	_txRing.head = (_txRing.head + 1) % _txRing.descriptors.size();

#if defined(__riscv) && __riscv_xlen == 64
	asm volatile("fence iorw, iorw" ::: "memory");
#endif

	txDesc.data1.store(regs::txDesc1::own(true));

	_barrier.writeback(&txDesc, sizeof(txDesc));

	_mmio.store(dma::transmitPollDemand, 0xff);

	while (txDesc.data1.load() & regs::txDesc1::own) {
		co_await _txEvent.async_wait();

		_barrier.invalidate(&txDesc, sizeof(txDesc));
	}
}

async::result<bool> K1xEmacNic::initialize() {
	auto findApmuDevice = [](uint64_t baseAddress)
	    -> async::result<
	        std::optional<std::pair<helix::UniqueDescriptor, protocols::hw::DtRegister>>> {
		// TODO: This should look up the clock DT node properly instead of looking them
		// all up and hoping one contains the address we are looking for.

		auto filter =
		    mbus_ng::Conjunction{{mbus_ng::EqualsFilter{"dt.compatible=spacemit,k1x-clock", ""}}};

		auto enumerator = mbus_ng::Instance::global().enumerate(filter);
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		if (events.empty()) {
			std::println("k1x-emac: No clock controller found on mbus");
			co_return std::nullopt;
		}

		for (const auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created) {
				continue;
			}

			auto mbusEntity = co_await mbus_ng::Instance::global().getEntity(events[0].id);
			auto device = protocols::hw::Device((co_await mbusEntity.getRemoteLane()).unwrap());
			auto dtInfo = co_await device.getDtInfo();

			for (size_t i = 0; i < dtInfo.regs.size(); ++i) {
				auto reg = dtInfo.regs[i];

				if (reg.address != baseAddress) {
					continue;
				}

				auto handle = co_await device.accessDtRegister(i);

				co_return std::make_pair(std::move(handle), reg);
			}
		}

		std::println("k1x-emac: No APMU device with matching base address found");

		co_return std::nullopt;
	};

	auto apmuBaseProperty = co_await _device.getDtProperty("k1x,apmu-base-reg");
	auto ctrlRegProperty = co_await _device.getDtProperty("ctrl-reg");

	uint64_t apmuBase;
	uint32_t ctrlReg;

	if (apmuBaseProperty) {
		auto access = apmuBaseProperty->access();
		if (!access.readCells(apmuBase, 2) && !access.readCells(apmuBase, 1)) {
			std::println("k1x-emac: Failed to read k1x,apmu-base-reg property");
			co_return false;
		}
	} else {
		std::println("k1x-emac: No APMU base address found");
		co_return false;
	}

	if (ctrlRegProperty) {
		auto access = ctrlRegProperty->access();
		if (!access.readCells(ctrlReg, 1)) {
			std::println("k1x-emac: Failed to read ctrl-reg property");
			co_return false;
		}
	} else {
		std::println("k1x-emac: No ctrl-reg property found");
		co_return false;
	}

	auto apmuReg = co_await findApmuDevice(apmuBase);
	if (!apmuReg) {
		std::println("k1x-emac: No APMU device found");
		co_return false;
	}

	auto &[reg, dtReg] = *apmuReg;

	auto apmuMapping = helix::Mapping{std::move(reg), dtReg.offset, dtReg.length};
	auto apmuSpace = arch::mem_space{apmuMapping.get()};

	if (!(arch::scalar_load<uint32_t>(apmuSpace, ctrlReg) & (1 << 0))) {
		std::println("k1x-emac: AXI bus clock is not enabled");
		co_return false;
	}

	// TODO(qookie): Read phy-mode from the DT.
	_phy = co_await nic::makeEthernetPhy(_mii, 0, nic::PhyMode::rgmii);

	if (!_phy) {
		std::println("k1x-emac: No PHY found");
		co_return false;
	}

	co_await _phy->configure();
	co_await _phy->startup();

	if (!_phy->linkStatus()) {
		std::println("k1x-emac: Link is down");
		co_return true;
	}

	_mmio.store(
	    mac::globalControl,
	    regs::mac::globalControl::speed100(_phy->speed() == nic::LinkSpeed::speed100)
	        | regs::mac::globalControl::speed1000(_phy->speed() == nic::LinkSpeed::speed1000)
	        | regs::mac::globalControl::fullDuplex(_phy->duplex() == nic::LinkDuplex::full)
	        | regs::mac::globalControl::resetRxStatCounters(true)
	        | regs::mac::globalControl::resetTxStatCounters(true)
	);

	auto rxThresholdProperty = co_await _device.getDtProperty("rx-threshold");
	auto txThresholdProperty = co_await _device.getDtProperty("tx-threshold");
	auto rxRingNumProperty = co_await _device.getDtProperty("rx-ring-num");
	auto txRingNumProperty = co_await _device.getDtProperty("tx-ring-num");
	auto dmaBurstLengthProperty = co_await _device.getDtProperty("dma-burst-len");

	uint32_t rxThreshold = 14;
	uint32_t txThreshold = 192;
	uint32_t rxRingNum = 128;
	uint32_t txRingNum = 128;
	uint32_t dmaBurstLength = 1;

	if (rxThresholdProperty) {
		auto access = rxThresholdProperty->access();
		if (!access.readCells(rxThreshold, 1)) {
			std::println("k1x-emac: Failed to read rx-threshold property");
			co_return false;
		}
	}

	if (txThresholdProperty) {
		auto access = txThresholdProperty->access();
		if (!access.readCells(txThreshold, 1)) {
			std::println("k1x-emac: Failed to read tx-threshold property");
			co_return false;
		}
	}

	if (rxRingNumProperty) {
		auto access = rxRingNumProperty->access();
		if (!access.readCells(rxRingNum, 1)) {
			std::println("k1x-emac: Failed to read rx-ring-num property");
			co_return false;
		}
	}

	if (txRingNumProperty) {
		auto access = txRingNumProperty->access();
		if (!access.readCells(txRingNum, 1)) {
			std::println("k1x-emac: Failed to read tx-ring-num property");
			co_return false;
		}
	}

	if (dmaBurstLengthProperty) {
		auto access = dmaBurstLengthProperty->access();
		if (!access.readCells(dmaBurstLength, 1)) {
			std::println("k1x-emac: Failed to read dma-burst-len property");
			co_return false;
		}
	}

	std::println(
	    "k1x-emac: tx-threshold={}, rx-threshold={}, tx-ring-num={}, rx-ring-num={}",
	    txThreshold,
	    rxThreshold,
	    txRingNum,
	    rxRingNum
	);

	_rxRing = {arch::dma_array<EmacDescriptor>{&_dmaPool, rxRingNum}};
	_txRing = {arch::dma_array<EmacDescriptor>{&_dmaPool, txRingNum}};

	_rxBuffer = arch::dma_buffer{&_dmaPool, dmaBufferSize * rxRingNum};
	_txBuffer = arch::dma_buffer{&_dmaPool, dmaBufferSize * txRingNum};

	assert(!((uintptr_t)_rxRing.descriptors.data() & 0x3f));
	assert(!((uintptr_t)_txRing.descriptors.data() & 0x3f));
	assert(!((uintptr_t)_rxBuffer.data() & 0x3f));
	assert(!((uintptr_t)_txBuffer.data() & 0x3f));

	memset(_txRing.descriptors.data(), 0, txRingNum * sizeof(EmacDescriptor));
	_barrier.writeback(_txRing.descriptors.view_buffer());

	for (size_t i = 0; i < _rxRing.descriptors.size(); i += descriptorsPerCacheLine) {
		cleanRxDescriptors(i);
	}

	_mmio.store(mac::interruptEnable, 0);
	_mmio.store(dma::interruptEnable, arch::bit_value<uint32_t>{0});
	_mmio.store(dma::control, arch::bit_value<uint32_t>{0});

	// Disable receive and trasmit units.
	_mmio.store(mac::receiveControl, arch::bit_value<uint32_t>{0});
	_mmio.store(mac::transmitControl, arch::bit_value<uint32_t>{0});

	// Enable MAC address filtering.
	_mmio.store(mac::macAddressControl, regs::mac::addressControl::macAddress1Enable(true));

	// Zero out the multicast hash table.
	_mmio.store(mac::multicastHashTable1, 0);
	_mmio.store(mac::multicastHashTable2, 0);
	_mmio.store(mac::multicastHashTable3, 0);
	_mmio.store(mac::multicastHashTable4, 0);

	// Set the transmit FIFO almost full threshold.
	// This value comes from the Linux driver, but I'm not sure how it was determined.
	_mmio.store(mac::transmitFifoAlmostFull, 0x1f8);

	// Set up receive and transmit thresholds.
	_mmio.store(mac::transmitPacketStartThreshold, txThreshold);
	_mmio.store(mac::receivePacketStartThreshold, rxThreshold);

	// Reset DMA.
	_mmio.store(dma::control, arch::bit_value<uint32_t>{0});

	_mmio.store(dma::configuration, regs::dma::configuration::softwareReset(true));
	co_await helix::sleepFor(10'000'000);

	_mmio.store(dma::configuration, regs::dma::configuration::softwareReset(false));
	co_await helix::sleepFor(10'000'000);

	_mmio.store(
	    dma::configuration,
	    regs::dma::configuration::burstLength(1 << (dmaBurstLength - 1))
	        | regs::dma::configuration::waitForDone(true)
	        | regs::dma::configuration::strictBurst(true) | regs::dma::configuration::dma64Bit(true)
	);

	// Configure DMA.
	auto rxDescsPhysical = helix::ptrToPhysical(_rxRing.descriptors.data());
	auto txDescsPhysical = helix::ptrToPhysical(_txRing.descriptors.data());
	assert(!(rxDescsPhysical & ~UINT64_C(0xFFFFFFFF)));
	assert(!(txDescsPhysical & ~UINT64_C(0xFFFFFFFF)));

	_mmio.store(dma::receiveBaseAddress, rxDescsPhysical);
	_mmio.store(dma::transmitBaseAddress, txDescsPhysical);

	// Enable the receive and transmit units.
	_mmio.store(
	    mac::receiveControl,
	    regs::mac::receiveControl::enable(true) | regs::mac::receiveControl::storeForward(true)
	);
	_mmio.store(
	    mac::transmitControl,
	    regs::mac::transmitControl::enable(true) | regs::mac::transmitControl::autoRetry(true)
	);

	_mmio.store(dma::transmitAutoPollCounter, 0);

	// Enable DMA.
	_mmio.store(
	    dma::control,
	    _mmio.load(dma::control) | regs::dma::control::startStopReceiveDma(true)
	        | regs::dma::control::startStopTransmitDma(true)
	);

	// Enable interrupts.
	_mmio.store(mac::interruptEnable, 0);
	_mmio.store(
	    dma::interruptEnable,
	    regs::dma::interruptEnable::txTransferDoneIntrEnable(true)
	        | regs::dma::interruptEnable::txDmaStoppedIntrEnable(true)
	        | regs::dma::interruptEnable::rxTransferDoneIntrEnable(true)
	        | regs::dma::interruptEnable::rxDmaStoppedIntrEnable(true)
	        | regs::dma::interruptEnable::rxMissedFrameIntrEnable(true)
	);

#if defined(__riscv) && __riscv_xlen == 64
	asm volatile("fence iorw, iorw" ::: "memory");
#endif

	auto macHigh = _mmio.load(mac::macAddressHigh);
	auto macMedium = _mmio.load(mac::macAddressMedium);
	auto macLow = _mmio.load(mac::macAddressLow);

	std::println(
	    "k1x-emac: MAC address: {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
	    macHigh & 0xff,
	    (macHigh >> 8) & 0xff,
	    macMedium & 0xff,
	    (macMedium >> 8) & 0xff,
	    macLow & 0xff,
	    (macLow >> 8) & 0xff
	);

	mac_[0] = macHigh & 0xff;
	mac_[1] = (macHigh >> 8) & 0xff;
	mac_[2] = macMedium & 0xff;
	mac_[3] = (macMedium >> 8) & 0xff;
	mac_[4] = macLow & 0xff;
	mac_[5] = (macLow >> 8) & 0xff;

	// Kick off the IRQ processing coroutine.
	processIrqs();
	processRx();

	co_return true;
}

void K1xEmacNic::cleanRxDescriptors(size_t index) {
	assert(index % descriptorsPerCacheLine == 0);
	assert(index + descriptorsPerCacheLine <= _rxRing.descriptors.size());

	for (size_t i = index; i < index + descriptorsPerCacheLine; ++i) {
		auto &rxDesc = _rxRing.descriptors[i];
		memset(&rxDesc, 0, sizeof(rxDesc));

		auto buffer = _rxBuffer.subview(i * dmaBufferSize, bufferSize);
		auto physical = helix::ptrToPhysical(buffer.data());
		assert(!(physical & ~UINT64_C(0xFFFFFFFF)));

		rxDesc.bufferAddr1.store(physical);
		rxDesc.data2.store(
		    regs::rxDesc2::bufferSize1(buffer.size())
		    | regs::rxDesc2::endOfRing(i == _rxRing.descriptors.size() - 1)
		);

#if defined(__riscv) && __riscv_xlen == 64
		asm volatile("fence iorw, iorw" ::: "memory");
#endif

		rxDesc.data1.store(regs::rxDesc1::own(true));

		_barrier.writeback(&rxDesc, sizeof(rxDesc));
	}
}

async::detached K1xEmacNic::processIrqs() {
	uint64_t sequence = 0;
	while (true) {
		if constexpr (debugIrqs) {
			std::println("k1x-emac: Waiting for IRQ... sequence={}", sequence);
		}

		auto await = co_await helix_ng::awaitEvent(_irq, sequence);
		HEL_CHECK(await.error());
		sequence = await.sequence();

		if constexpr (debugIrqs) {
			std::println("k1x-emac: IRQ received, sequence={}", sequence);
		}

		auto status = _mmio.load(dma::statusIrq);
		auto clearBits = arch::bit_value<uint32_t>{0};

		if (status & regs::dma::status::txDoneIrq) {
			clearBits |= regs::dma::status::txDoneIrq(true);
			if constexpr (debugIrqs || debugRxTx) {
				std::println("k1x-emac: TX done IRQ received");
			}
		}

		if (status & regs::dma::status::txDescUnavailableIrq) {
			clearBits |= regs::dma::status::txDescUnavailableIrq(true);
			if constexpr (debugIrqs) {
				std::println("k1x-emac: TX descriptor unavailable IRQ received");
			}
		}

		if (status & regs::dma::status::txDmaStoppedIrq) {
			clearBits |= regs::dma::status::txDmaStoppedIrq(true);
			if constexpr (debugIrqs) {
				std::println("k1x-emac: TX DMA stopped IRQ received");
			}
		}

		if (status & regs::dma::status::rxDoneIrq) {
			clearBits |= regs::dma::status::rxDoneIrq(true);
			if constexpr (debugIrqs || debugRxTx) {
				std::println("k1x-emac: RX done IRQ received");
			}
		}

		if (status & regs::dma::status::rxDescUnavailableIrq) {
			clearBits |= regs::dma::status::rxDescUnavailableIrq(true);
			if constexpr (debugIrqs) {
				std::println("k1x-emac: RX descriptor unavailable IRQ received");
			}
		}

		if (status & regs::dma::status::rxDmaStoppedIrq) {
			clearBits |= regs::dma::status::rxDmaStoppedIrq(true);
			if constexpr (debugIrqs) {
				std::println("k1x-emac: RX DMA stopped IRQ received");
			}
		}

		if (status & regs::dma::status::rxMissedFrameIrq) {
			clearBits |= regs::dma::status::rxMissedFrameIrq(true);
			if constexpr (debugIrqs) {
				std::println("k1x-emac: RX missed frame IRQ received");
			}
		}

		_mmio.store(dma::statusIrq, clearBits);

		HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckAcknowledge, sequence));

		if (status & regs::dma::status::txDoneIrq) {
			_txEvent.raise();
		}

		if (status & regs::dma::status::rxDoneIrq) {
			_rxEvent.raise();
		}
	}
}

} // namespace

namespace nic::k1x_emac {

async::result<std::shared_ptr<nic::Link>> makeShared(mbus_ng::EntityId entity) {
	auto mbusEntity = co_await mbus_ng::Instance::global().getEntity(entity);
	auto device = protocols::hw::Device((co_await mbusEntity.getRemoteLane()).unwrap());

	auto dtInfo = co_await device.getDtInfo();

	co_await device.enableBusIrq();

	auto reg = co_await device.accessDtRegister(0);
	auto irq = co_await device.installDtIrq(0);

	std::println(
	    "k1x-emac: MMIO register at address 0x{:x}, length 0x{:x}",
	    dtInfo.regs[0].address,
	    dtInfo.regs[0].length
	);

	auto mapping = helix::Mapping{std::move(reg), dtInfo.regs[0].offset, dtInfo.regs[0].length};
	auto nic = std::make_shared<K1xEmacNic>(std::move(device), std::move(mapping), std::move(irq));

	if (co_await nic->initialize()) {
		std::println("k1x-emac: NIC initialized");

		co_return nic;
	}

	co_return nullptr;
}

} // namespace nic::k1x_emac
