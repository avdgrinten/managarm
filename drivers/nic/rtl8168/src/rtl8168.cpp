#include <async/basic.hpp>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <nic/rtl8168/common.hpp>
#include <nic/rtl8168/rtl8168.hpp>
#include <nic/rtl8168/regs.hpp>
#include <nic/rtl8168/debug_options.hpp>
#include <frg/logging.hpp>
#include <helix/timer.hpp>
#include <memory>
#include <unistd.h>

static const std::unordered_map<RealtekNic::MacRevision, std::string> rtl_chip_infos = {
	// PCI Devices
	{RealtekNic::MacRevision::MacVer02, "RTL8169s"},
	{RealtekNic::MacRevision::MacVer03, "RTL8110s"},
	{RealtekNic::MacRevision::MacVer04, "RTL8169sb/8110sb"},
	{RealtekNic::MacRevision::MacVer05, "RTL8169sc/8110sc"},
	{RealtekNic::MacRevision::MacVer06, "RTL8169sc/8110sc"},
	// PCIe Devices
	{RealtekNic::MacRevision::MacVer07, "RTL8102e"},
	{RealtekNic::MacRevision::MacVer08, "RTL8102e"},
	{RealtekNic::MacRevision::MacVer09, "RTL8102e/RTL8103e"},
	{RealtekNic::MacRevision::MacVer10, "RTL8101e/RTL8100e"},
	{RealtekNic::MacRevision::MacVer11, "RTL8168b/8111b"},
	{RealtekNic::MacRevision::MacVer14, "RTL8401"},
	{RealtekNic::MacRevision::MacVer17, "RTL8168b/8111b"},
	{RealtekNic::MacRevision::MacVer18, "RTL8168cp/8111cp"},
	{RealtekNic::MacRevision::MacVer19, "RTL8168c/8111c"},
	{RealtekNic::MacRevision::MacVer20, "RTL8168c/8111c"},
	{RealtekNic::MacRevision::MacVer21, "RTL8168c/8111c"},
	{RealtekNic::MacRevision::MacVer22, "RTL8168c/8111c"},
	{RealtekNic::MacRevision::MacVer23, "RTL8168cp/8111cp"},
	{RealtekNic::MacRevision::MacVer24, "RTL8168cp/8111cp"},
	{RealtekNic::MacRevision::MacVer25, "RTL8168d/8111d"},
	{RealtekNic::MacRevision::MacVer26, "RTL8168d/8111d"},
	{RealtekNic::MacRevision::MacVer28, "RTL8168dp/8111dp"},
	{RealtekNic::MacRevision::MacVer29, "RTL8105e"},
	{RealtekNic::MacRevision::MacVer30, "RTL8105e"},
	{RealtekNic::MacRevision::MacVer31, "RTL8168dp/8111dp"},
	{RealtekNic::MacRevision::MacVer32, "RTL8168e/8111e"},
	{RealtekNic::MacRevision::MacVer33, "RTL8168e/8111e"},
	{RealtekNic::MacRevision::MacVer34, "RTL8168evl/8111evl"},
	{RealtekNic::MacRevision::MacVer35, "RTL8168f/8111f"},
	{RealtekNic::MacRevision::MacVer36, "RTL8168f/8111f"},
	{RealtekNic::MacRevision::MacVer37, "RTL8402"},
	{RealtekNic::MacRevision::MacVer38, "RTL8411"},
	{RealtekNic::MacRevision::MacVer39, "RTL8106e"},
	{RealtekNic::MacRevision::MacVer40, "RTL8168g/8111g"},
	{RealtekNic::MacRevision::MacVer42, "RTL8168gu/8111gu"},
	{RealtekNic::MacRevision::MacVer43, "RTL8106eus"},
	{RealtekNic::MacRevision::MacVer44, "RTL8411b"},
	{RealtekNic::MacRevision::MacVer46, "RTL8168h/8111h"},
	{RealtekNic::MacRevision::MacVer48, "RTL8107e"},
	{RealtekNic::MacRevision::MacVer51, "RTL8168ep/8111ep"},
	{RealtekNic::MacRevision::MacVer52, "RTL8168fp/RTL8117"},
	{RealtekNic::MacRevision::MacVer53, "RTL8168fp/RTL8117"},
	{RealtekNic::MacRevision::MacVer61, "RTL8125A"},
	{RealtekNic::MacRevision::MacVer63, "RTL8125B"},
	{RealtekNic::MacRevision::MacVer70, "RTL8126A"},
	{RealtekNic::MacRevision::MacVer80, "RTL8127A"},
};

RealtekNic::RealtekNic(
    protocols::hw::Device device, helix::UniqueDescriptor dmaSpace, bool iommuActive
)
: nic::Link(1500, &_dmaPool),
  dmaSpaceHandle_{std::move(dmaSpace)},
  dmaSpace_{_dmaRealm.attachDmaSpace(dmaSpaceHandle_, iommuActive)},
  _device{std::move(device)} {}

async::result<void> RealtekNic::getMmio() {
	auto info = co_await _device.getPciInfo();
	size_t bar_index = 0;

	// Select the first MMIO BAR
	while(true) {
		assert(bar_index < 6 && "drivers/rtl8168: unable to locate MMIO BAR!");

		if(info.barInfo[bar_index].ioType == protocols::hw::IoType::kIoTypeMemory) {
			break;
		}

		bar_index++;
	}

	if(logDriverStart) {
		std::cout << "drivers/rtl8168: selected BAR " << bar_index << std::endl;
	}

	auto &barInfo = info.barInfo[bar_index];
	assert(barInfo.ioType == protocols::hw::IoType::kIoTypeMemory);
	auto bar = co_await _device.accessBar(bar_index);

	_mmio_mapping = {bar, barInfo.offset, barInfo.length};
	_mmio = _mmio_mapping.get();
}

void RealtekNic::determineMacRevision() {
	struct rtl_mac_info {
		uint16_t mask;
		uint16_t val;
		MacRevision ver;
	};

	// This structure was adapted from the linux kernel driver. It seems to be the only sane way to detect
	// exactly which card we have: it compares various bits from the TxConfig register and applies some masks
	// over the identification registers.
	static const auto mac_info = std::to_array<struct rtl_mac_info>({
		// 8127A family.
		{ 0x7cf, 0x6c9,	MacRevision::MacVer80 },

		// 8126A family.
		{ 0x7cf, 0x64a,	MacRevision::MacVer70 },
		{ 0x7cf, 0x649,	MacRevision::MacVer70 },

		// 8125B family.
		{ 0x7cf, 0x641,	MacRevision::MacVer63 },

		// 8125A family.
		{ 0x7cf, 0x609,	MacRevision::MacVer61 },

		// RTL8117
		{ 0x7cf, 0x54b,	MacRevision::MacVer53 },
		{ 0x7cf, 0x54a,	MacRevision::MacVer52 },

		// 8168EP family.
		{ 0x7cf, 0x502,	MacRevision::MacVer51 },
		// 8168H family.
		{ 0x7cf, 0x541,	MacRevision::MacVer46 },
		// 8168G family.
		{ 0x7cf, 0x5c8,	MacRevision::MacVer44 },
		{ 0x7cf, 0x509,	MacRevision::MacVer42 },
		{ 0x7cf, 0x4c0,	MacRevision::MacVer40 },

		// 8168F family.
		{ 0x7c8, 0x488,	MacRevision::MacVer38 },
		{ 0x7cf, 0x481,	MacRevision::MacVer36 },
		{ 0x7cf, 0x480,	MacRevision::MacVer35 },

		// 8168E family.
		{ 0x7c8, 0x2c8,	MacRevision::MacVer34 },
		{ 0x7cf, 0x2c1,	MacRevision::MacVer32 },
		{ 0x7c8, 0x2c0,	MacRevision::MacVer33 },

		// 8168D family.
		{ 0x7cf, 0x281,	MacRevision::MacVer25 },
		{ 0x7c8, 0x280,	MacRevision::MacVer26 },

		// 8168DP family.
		{ 0x7cf, 0x28a,	MacRevision::MacVer28 },
		{ 0x7cf, 0x28b,	MacRevision::MacVer31 },

		// 8168C family.
		{ 0x7cf, 0x3c9,	MacRevision::MacVer23 },
		{ 0x7cf, 0x3c8,	MacRevision::MacVer18 },
		{ 0x7c8, 0x3c8,	MacRevision::MacVer24 },
		{ 0x7cf, 0x3c0,	MacRevision::MacVer19 },
		{ 0x7cf, 0x3c2,	MacRevision::MacVer20 },
		{ 0x7cf, 0x3c3,	MacRevision::MacVer21 },
		{ 0x7c8, 0x3c0,	MacRevision::MacVer22 },

		// 8168B family.
		{ 0x7c8, 0x380,	MacRevision::MacVer17 },
		{ 0x7c8, 0x300,	MacRevision::MacVer11 },

		// 8101 family.
		{ 0x7c8, 0x448,	MacRevision::MacVer39 },
		{ 0x7c8, 0x440,	MacRevision::MacVer37 },
		{ 0x7cf, 0x409,	MacRevision::MacVer29 },
		{ 0x7c8, 0x408,	MacRevision::MacVer30 },
		{ 0x7cf, 0x349,	MacRevision::MacVer08 },
		{ 0x7cf, 0x249,	MacRevision::MacVer08 },
		{ 0x7cf, 0x348,	MacRevision::MacVer07 },
		{ 0x7cf, 0x248,	MacRevision::MacVer07 },
		{ 0x7cf, 0x240,	MacRevision::MacVer14 },
		{ 0x7c8, 0x348,	MacRevision::MacVer09 },
		{ 0x7c8, 0x248,	MacRevision::MacVer09 },
		{ 0x7c8, 0x340,	MacRevision::MacVer10 },

		// 8110 family.
		{ 0xfc8, 0x980,	MacRevision::MacVer06 },
		{ 0xfc8, 0x180,	MacRevision::MacVer05 },
		{ 0xfc8, 0x100,	MacRevision::MacVer04 },
		{ 0xfc8, 0x040,	MacRevision::MacVer03 },
		{ 0xfc8, 0x008,	MacRevision::MacVer02 },

		// Catch-all
		// If a card fails to be detected properly,
		// it will be assigned this mac revision
		{ 0x000, 0x000,	MacRevision::MacVerNone }
	});
	uint16_t xid = (_mmio.load(regs::transmit_config) & flags::transmit_config::detect_bits) & 0xFCF;

	for(auto &v : mac_info) {
		if((xid & v.mask) == v.val) {
			_revision = v.ver;
			break;
		}
	}

	// Some cards need to be handled differently if they do not support gigabit.
	if (_revision != MacVerNone && !_has_gmii) {
		if (_revision == MacRevision::MacVer42)
			_revision = MacRevision::MacVer43;
		else if (_revision == MacRevision::MacVer46)
			_revision = MacRevision::MacVer48;
	}

	assert(_revision != MacRevision::MacVerNone);
	frg::to(std::cout) << frg::fmt("drivers/rtl8168: MAC Revision: MacVer{:02}", uint16_t(_revision)) << frg::endlog;
	std::cout << "drivers/rtl8168: Card name: " << rtl_chip_infos.at(_revision) << std::endl;
}

void RealtekNic::determineDashType() {
	switch(_revision) {
		case MacRevision::MacVer28:
		case MacRevision::MacVer31: {
			// DASH_DP
			assert(!"Not Implemented");
			break;
		}
		case MacRevision::MacVer51 ... MacRevision::MacVer53: {
			// DASH_EP
			assert(!"Not Implemented");
			break;
		}
		default:
			_dash_type = DashType::DashNone;
	}
}

async::result<void> RealtekNic::configure8168EEEMac() {
	if(_revision != MacRevision::MacVer38) {
		_mmio.store(regs::eee_led, _mmio.load(regs::eee_led) & ~0x07);
	}

	co_await writeERIRegister(0x1B0, 0b1111, co_await readERIRegister(0x1B0) | 3);
}

void RealtekNic::disablePCIeL2L3State() {
	_mmio.store(regs::config3, _mmio.load(regs::config3) / flags::config3::enable_l2l3(false));
}

async::result<void> RealtekNic::initializeHardware() {
	switch(_revision) {
		case MacRevision::MacVer51 ... MacRevision::MacVer53: {
			assert(!"Not implemented; linux calls \"rtl8168ep_stop_cmac\" here");
			[[fallthrough]];
		}
		case MacRevision::MacVer40 ... MacRevision::MacVer48: {
			// RTL8168g; linux equivalent: rtl_hw_init_8168g
			co_await enableRXDVGate();
			auto cmd = _mmio.load(regs::cmd);
			cmd /= flags::cmd::transmitter(false);
			cmd /= flags::cmd::receiver(false);
			_mmio.store(regs::cmd, cmd);
			co_await helix::sleepFor(1'000'000);

			_mmio.store(regs::mcu, _mmio.load(regs::mcu) / (flags::mcu::now_is_oob(false)));

			write8168MacOCPRegister(0xE8DE, read8168MacOCPRegister(0xE8DE) & ~(1 << 14));
			co_await RTL8168gWaitLLShareFifoReady();

			write8168MacOCPRegister(0xE8DE, 1 << 15);
			co_await RTL8168gWaitLLShareFifoReady();

			break;
		}
		case MacRevision::MacVer61 ... MacRevision::MacVer70: {
			co_await enableRXDVGate();
			auto cmd = _mmio.load(regs::cmd);
			cmd /= flags::cmd::transmitter(false);
			cmd /= flags::cmd::receiver(false);
			_mmio.store(regs::cmd, cmd);
			co_await helix::sleepFor(1'000'000);

			_mmio.store(regs::mcu, _mmio.load(regs::mcu) / (flags::mcu::now_is_oob(false)));

			write8168MacOCPRegister(0xE8DE, read8168MacOCPRegister(0xE8DE) & ~(1 << 14));
			co_await RTL8168gWaitLLShareFifoReady();

			write8168MacOCPRegister(0xC0AA, 0x07D0);
			write8168MacOCPRegister(0xC0A6, 0x0150);
			write8168MacOCPRegister(0xC01E, 0x5555);
			co_await RTL8168gWaitLLShareFifoReady();
			break;
		}
		default: {
			break;
		}
	}
}

async::result<bool> RealtekNic::up() {
	co_await cleanup();
	co_await startCard();
	co_return true;
}

async::result<bool> RealtekNic::down() {
	assert(!"Not implemented");
	co_return true;
}

async::result<bool> RealtekNic::cleanup() {
	maskIRQsAndAck();
	closeRX();
	switch(_revision) {
		case MacRevision::MacVer28:
		case MacRevision::MacVer31: {
			assert(!"Not Implemented");
			break;
		}
		case MacRevision::MacVer34 ... MacRevision::MacVer38: {
			assert(!"Not Implemented");
			break;
		}
		case MacRevision::MacVer40 ... MacRevision::MacVer70: {
			co_await enableRXDVGate();
			co_await helix::sleepFor(2'000'000);
			break;
		}
		default: {
			assert(!"Not Implemented");
			break;
		}
	}

	co_await issueHardwareReset();
	co_return true;
}

async::result<void> RealtekNic::issueHardwareReset() {
	_mmio.store(regs::cmd, flags::cmd::reset(true));

	while(_mmio.load(regs::cmd) & flags::cmd::reset)
		co_await helix::sleepFor(1'000); // 1000 ns = 1 µs
}

// These two functions write the high uint32_t first; this is intentional:
// some motherboards (at least some embedded ARM boards) have problems if you write the low int first
void RealtekNic::setupRxDescriptors() {
	_mmio.store(regs::rdsar_high, (_rxQueue->getBase() >> 32) & 0xFFFFFFFF);
	__sync_synchronize();
	_mmio.store(regs::rdsar_low, _rxQueue->getBase() & 0xFFFFFFFF);
}

void RealtekNic::setupTxDescriptors() {
	_mmio.store(regs::tnpds_high, (_txQueue->getBase() >> 32) & 0xFFFFFFFF);
	__sync_synchronize();
	_mmio.store(regs::tnpds_low, _txQueue->getBase() & 0xFFFFFFFF);
}

async::result<bool> RealtekNic::startCard() {
	unlockConfigRegisters();
	setHardwareASPMClockEnable(false);

	// TODO: CpCmd stuff
	//       it seems like they just mask it with CPCMD_MASK here

	if(_revision <= MacRevision::MacVer06) {
		assert(!"Not Implemented"); // rtl_hw_start_8169
	} else if(is8125Family()) {
		arch::bit_mask<uint8_t> int_cfg0_mask{0xFF};
		if (_revision == MacVer80)
			int_cfg0_mask = arch::bit_mask<uint8_t>{0xBF};
		else if (_revision != MacVer61)
			int_cfg0_mask = arch::bit_mask<uint8_t>{0xFE};

		_mmio.store(regs::int_cfg0_8125, _mmio.load(regs::int_cfg0_8125) & int_cfg0_mask);

		switch(_revision) {
			case MacVer61: {
				for(int i = 0xA00; i < 0xB00; i += 4) {
					_mmio.store(arch::scalar_register<uint32_t>{i}, 0);
				}
				break;
			}
			case MacVer63:
			case MacVer70: {
				for(int i = 0xA00; i < 0xA80; i += 4) {
					_mmio.store(arch::scalar_register<uint32_t>{i}, 0);
				}
				_mmio.store(regs::int_cfg1_8125, 0x00);
				break;
			}
			default:
				break;
		}

		co_await configure8125Hardware();
	} else {
		if (_revision >= MacRevision::MacVer34 &&
			_revision != MacRevision::MacVer37 &&
			_revision != MacRevision::MacVer39) {
			_mmio.store(regs::tx_max_size, 0x27);
		} else {
			_mmio.store(regs::tx_max_size, (8064 >> 7));
		}

		co_await configureHardware();

		_mmio.store(regs::interrupt_mitigate, arch::bit_value<uint16_t>(0));
	}

	co_await enableExitL1();
	setHardwareASPMClockEnable(true);
	if(logDriverStart) {
		std::cout << "drivers/rtl8168: setting up RX Descriptors" << std::endl;
	}
	setupRxDescriptors();
	if(logDriverStart) {
		std::cout << "drivers/rtl8168: setting up TX Descriptors" << std::endl;
	}
	setupTxDescriptors();
	lockConfigRegisters();

	// TODO: Jumbo packet configuration would probably go here

	forcePCICommit();
	_mmio.store(regs::cmd, flags::cmd::transmitter(true) | flags::cmd::receiver(true));

	setRxConfigRegisters();
	setTxConfigRegisters();
	// TODO: rtl_set_rx_config_features
	//       rtl_set_rx_mode
	co_return true;
}

async::result<void> RealtekNic::init() {
	auto pci_id = co_await _device.loadPciSpace(pci::Device, 2);

	// TODO: Some cards that are realtek chipsets use different PCI device IDs.
	//       It should be safe to treat them as rtl8168 cards, however we should ideally handle this.
	//       I could not find accurate information as to which card they exactly are, but seeing that no driver I could find handles them specially,
	//       they are likely pretty standard RTL chips which require no special cases anyway, so it is kind of irrelevant what we set here.
	switch(pci_id) {
		case uint16_t(PciModel::RTL8136): {
			// These cards are 10/100 only
			_has_gmii = false;
			_model = PciModel::RTL8136;
			break;
		}
		case uint16_t(PciModel::RTL8125): {
			_model = PciModel::RTL8125;
			break;
		}
		case uint16_t(PciModel::RTL8126): {
			_model = PciModel::RTL8126;
			break;
		}
		case uint16_t(PciModel::RTL8161): {
			_model = PciModel::RTL8161;
			break;
		}
		case uint16_t(PciModel::RTL8162): {
			_model = PciModel::RTL8162;
			break;
		}
		case uint16_t(PciModel::RTL8167): {
			_model = PciModel::RTL8167;
			break;
		}
		case uint16_t(PciModel::RTL8168): {
			_model = PciModel::RTL8168;
			break;
		}
		case uint16_t(PciModel::RTL8169): {
			_model = PciModel::RTL8169;
			break;
		}

		default: {
			std::cout << "drivers/rtl8168: unknown PCI device ID " << std::hex << pci_id << std::dec << std::endl;
			std::cout << "drivers/rtl8168: pretending to be a RTL8168." << std::endl;
			_model = PciModel::RTL8168;
		}
	}

	_rxQueue = co_await RxQueue::create(*this, NUM_RX_DESCRIPTORS);
	_txQueue = co_await TxQueue::create(*this, NUM_TX_DESCRIPTORS);

	_irq = co_await _device.accessIrq();

	co_await getMmio();

	determineMacRevision();

	// TODO: some ancient cards need some IRQ setup / only support legacy PCI IRQs.
	//       we do not support these, so assert if one is detected.
	if(_revision <= MacRevision::MacVer17) {
		assert(!"Not Implemented; requires legacy PCI IRQ support / IRQ setup");
	}

	auto readMac = [&] (arch::scalar_register<uint32_t> lowReg, arch::scalar_register<uint32_t> highReg) {
		auto mac_lower = _mmio.load(lowReg);
		auto mac_higher = _mmio.load(highReg);

		mac_[0] = (mac_lower >> 0) & 0xFF;
		mac_[1] = (mac_lower >> 8) & 0xFF;
		mac_[2] = (mac_lower >> 16) & 0xFF;
		mac_[3] = (mac_lower >> 24) & 0xFF;
		mac_[4] = (mac_higher >> 0) & 0xFF;
		mac_[5] = (mac_higher >> 8) & 0xFF;

		// Reject all-zero and multicast addresses, like Linux' is_valid_ether_addr().
		bool allZero = !(mac_lower | (mac_higher & 0xFFFF));
		return !allZero && !(mac_[0] & 1);
	};

	// Like Linux, prefer the backup copy on the RTL8125 family and fall back to IDR0.
	if(is8125Family() && readMac(regs::mac0_bkp_low, regs::mac0_bkp_high)) {
		// IDR0 is what the RX filter matches against (Linux: rtl_rar_set()).
		unlockConfigRegisters();
		_mmio.store(regs::idr4, mac_[4] | (mac_[5] << 8));
		forcePCICommit();
		_mmio.store(regs::idr0, mac_[0] | (mac_[1] << 8) | (mac_[2] << 16) | (uint32_t(mac_[3]) << 24));
		forcePCICommit();
		lockConfigRegisters();
	} else {
		readMac(regs::idr0, regs::idr4);
	}

	std::cout << "drivers/rtl8168: MAC " << mac_ << std::endl;

	maskIRQsAndAck();
	if(logDriverStart) {
		std::cout << "drivers/rtl8168: masked IRQs" << std::endl;
	}
	co_await initializeHardware();
	if(logDriverStart) {
		std::cout << "drivers/rtl8168: initialized hardware" << std::endl;
	}
	co_await issueHardwareReset();
	if(logDriverStart) {
		std::cout << "drivers/rtl8168: reset card" << std::endl;
	}
	co_await startCard();
	if(logDriverStart) {
		std::cout << "drivers/rtl8168: started card" << std::endl;
	}
	updateLinkState();

	// Enable IRQs
	if(is8125Family()) {
		_mmio.store(regs::rtl8125::interrupt_status_val, ~0);
		_mmio.store(regs::rtl8125::interrupt_mask_val, ~0);
	} else {
		_mmio.store(regs::interrupt_status_val, ~0);
		_mmio.store(regs::interrupt_mask_val, ~0);
	}
	forcePCICommit();

	if(logDriverStart) {
		std::cout << "drivers/rtl8168: entering interrupt loop" << std::endl;
	}

	if(logDriverStart)
		printRegisters();

	processIrqs();
}

// TODO: We really should not do this like this
//       What we should do is have a constant callback we always poll to, as this way of doing things will always be prone to race conditions
async::result<size_t> RealtekNic::receive(arch::dma_buffer_view frame) {
	co_return co_await _rxQueue->submitDescriptor(frame, *this);
}

async::result<void> RealtekNic::send(arch::dma_buffer_view payload) {
	co_await _txQueue->submitDescriptor(payload, *this);
}


async::detached RealtekNic::processIrqs() {
	co_await _device.enableBusIrq();

	// TODO: The kick here should not be required.
	HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckKick, 0));

	if(logIRQs) {
		std::cout << "drivers/rtl8168: entering processIrqs loop" << std::endl;
	}

	uint64_t sequence = 0;

	if(use_timer_interrupt_to_check_received_packets) {
		// Enable the timer IRQ
		_mmio.store(regs::timer_interrupt, 0x400);
	}
	while(true) {
		auto await = co_await helix_ng::awaitEvent(_irq, sequence);
		HEL_CHECK(await.error());
		sequence = await.sequence();

		arch::bit_value<uint32_t> status{0};
		if(is8125Family()) {
			status = _mmio.load(regs::rtl8125::interrupt_status);
		} else {
			status = arch::bit_value<uint32_t>{uint16_t(_mmio.load(regs::interrupt_status))};
		}

		if constexpr (logIRQs)
			std::println("drivers/rtl8168: IRQ received status 0x{:08x}", uint32_t(status));

		if(!uint32_t(status)) {
			HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckAcknowledge, sequence));
			continue;
		}

		if(is8125Family()) {
			_mmio.store(regs::rtl8125::interrupt_status, arch::bit_value<uint32_t>(status));
		} else {
			_mmio.store(regs::interrupt_status, arch::bit_value<uint16_t>(uint32_t(status)));
		}

		// Did the status of the network link change?
		if(status & flags::interrupt_status::link_change) {
			if(logIRQs) {
				puts("drivers/rtl8168: link change");
			}
			updateLinkState();
		}

		// Did we successfully transmit information?
		if(status & flags::interrupt_status::tx_ok) {
			if(logIRQs) {
				std::cout << "drivers/rtl8168: TX_OK" << std::endl;
			}
			_txQueue->handleTxOk();
		}

		// Did the NIC run out of descriptors to send?
		if(status & flags::interrupt_status::tx_desc_unavailable) {
			if(logIRQs) {
				std::cout << "drivers/rtl8168: TX_DESC_UNAVAILABLE" << std::endl;
			}

			// If the TX_OK bit is also set, and we still have data to send, then ring the doorbell again.
			// This bypasses bugs found in some cards.
			if(status & flags::interrupt_status::tx_ok && !_txQueue->bufferEmpty() && restart_transmitter_on_tx_ok_and_tx_desc_unavailable) {
				ringDoorbell();
			}
		}

		// Was there in error during transmit?
		if(status & flags::interrupt_status::tx_err) {
			std::cout << "drivers/rtl8168: got TX_ERR interrupt!" << std::endl;
		}

		// Did we receive something?
		if(status & flags::interrupt_status::rx_ok) {
			if(logIRQs) {
				std::cout << "drivers/rtl8168: RX_OK" << std::endl;
			}
		}

		// Did the NIC encounter an error doing receive?
		if(status & flags::interrupt_status::rx_err) {
			std::cout << "drivers/rtl8168: got RX_ERR interrupt!" << std::endl;
		}

		// Did the NIC run out of RX descriptors?
		if((status & flags::interrupt_status::rx_overflow) && !_loggedRxOverflow) {
			std::cout << "drivers/rtl8168: ran out of RX descriptors" << std::endl;
			_loggedRxOverflow = true;
		}

		if((status & flags::interrupt_status::rx_ok) || (status & flags::interrupt_status::rx_err)
				|| (status & flags::interrupt_status::rx_overflow)
				|| (status & flags::interrupt_status::rx_fifo_overflow))
			_rxQueue->handleRxOk();

		// Did we get a timer interrupt?
		if(status & flags::interrupt_status::pcs_timeout) {
			if(logIRQs) {
				std::cout << "drivers/rtl8168: PCS_TIMEOUT" << std::endl;
			}
			if(use_timer_interrupt_to_check_received_packets) {
				_rxQueue->handleRxOk();
			}
			// Reset the timer
			_mmio.store(regs::timer_count, 1);
		}

		HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckAcknowledge, sequence));
	}
}

namespace nic::rtl8168 {

async::result<std::shared_ptr<nic::Link>> makeShared(protocols::hw::Device device) {
	co_await device.enableBusmaster();
	co_await device.enableDma(false);
	auto [iommuActive, dmaSpace, dmaCoherent] = co_await device.getDmaSpace();

	// The descriptor rings would need uncached memory, which dma_pool cannot provide yet.
	if(!dmaCoherent) {
		std::cout << "drivers/rtl8168: DMA is not cache-coherent, which is not supported" << std::endl;
		co_return nullptr;
	}

	auto nic = std::make_shared<RealtekNic>(std::move(device), std::move(dmaSpace), iommuActive);
	co_await nic->init();
	co_return std::move(nic);
}

} // namespace nic::rtl8168
