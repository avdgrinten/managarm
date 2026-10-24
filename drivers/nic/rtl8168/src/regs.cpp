#include <async/basic.hpp>
#include <cstdint>
#include <nic/rtl8168/common.hpp>
#include <nic/rtl8168/rtl8168.hpp>
#include <nic/rtl8168/regs.hpp>
#include <frg/logging.hpp>
#include <helix/timer.hpp>
#include <print>
#include <unistd.h>

// TODO: the linux kernel locks on these
void RealtekNic::unlockConfigRegisters() {
	_mmio.store(regs::cr9346, flags::cr9346::unlock_regs);
}

void RealtekNic::lockConfigRegisters() {
	_mmio.store(regs::cr9346, flags::cr9346::lock_regs);
}

void RealtekNic::maskIRQsAndAck() {
	if(_model == PciModel::RTL8125) {
		_mmio.store(regs::rtl8125::interrupt_mask_val, 0);
		_mmio.store(regs::rtl8125::interrupt_status_val, ~0);
	} else {
		_mmio.store(regs::interrupt_mask_val, 0);
		_mmio.store(regs::interrupt_status_val, ~0);
	}
	forcePCICommit();
}

async::result<void> RealtekNic::enableRXDVGate() {
	_mmio.store(regs::misc, _mmio.load(regs::misc) | flags::misc::rxdv_gate(true));

	std::cout << "drivers/rtl8168: enabled RXDV gate" << std::endl;
	co_await helix::sleepFor(2'000'000);
	co_await waitTxRxFifoEmpty();
}

void RealtekNic::disableRXDVGate() {
	_mmio.store(regs::misc, _mmio.load(regs::misc) / flags::misc::rxdv_gate(false));
	std::cout << "drivers/rtl8168: disabled RXDV gate" << std::endl;
}

void RealtekNic::setHardwareASPMClockEnable(bool val) {
	if(_revision <= MacRevision::MacVer32) {
		return;
	}

	if(val) {
		_mmio.store(regs::config5, _mmio.load(regs::config5) | flags::config5::aspm_enable(true));
		_mmio.store(regs::config2, _mmio.load(regs::config2) | flags::config2::clk_rq_enable(true));

		switch(_revision) {
			case MacRevision::MacVer46 ... MacRevision::MacVer48:
			case MacRevision::MacVer61 ... MacRevision::MacVer63: {
				// assert(!"Not Implemented");
				break;
			}
			default: {
				break;
			}
		}
	} else {
		switch(_revision) {
			case MacRevision::MacVer46 ... MacRevision::MacVer48:
			case MacRevision::MacVer61 ... MacRevision::MacVer63: {
				// assert(!"Not Implemented");
				break;
			}
			default: {
				break;
			}
		}

		_mmio.store(regs::config2, _mmio.load(regs::config2) / flags::config2::clk_rq_enable(false));
		_mmio.store(regs::config5, _mmio.load(regs::config5) / flags::config5::aspm_enable(false));
	}
}

uint16_t RealtekNic::read8168MacOCPRegister(uint32_t reg) {
	// TODO: verify if reg is valid
	// TODO: lock
	_mmio.store(regs::ocpdr, reg << 15);
	return _mmio.load(regs::ocpdr);
}

void RealtekNic::write8168MacOCPRegister(uint32_t reg, uint16_t val) {
	// TODO: same as read8168PhyOCPRegister
	_mmio.store(regs::ocpdr, reg << 15 | val | 0x80000000);
}

// TODO: MacVer 52, 53 need some adjusting in the command here; not gonna handle this atm
//       since its a bit super specific
async::result<uint32_t> RealtekNic::readERIRegister(int reg) {
	_mmio.store(regs::eriar, flags::eriar::type(flags::eriar::exgmac) | flags::eriar::address(reg) | flags::eriar::mask(0b1111));

	co_return (co_await waitERIARReadReady()) ? _mmio.load(regs::eridr) : ~0;
}

async::result<void> RealtekNic::writeERIRegister(int reg, uint8_t mask, uint32_t val) {
	(void) reg;
	_mmio.store(regs::eridr, val);
	_mmio.store(regs::eriar, flags::eriar::write(true) | flags::eriar::mask(mask) | flags::eriar::type(flags::eriar::exgmac));

	co_await waitERIARWriteReady();
}

async::result<uint32_t> RealtekNic::readCSIRegister(int reg) {
	_mmio.store(regs::csiar, flags::csiar::pci_function(_pci_function) | flags::csiar::address(reg) | flags::csiar::byte_enable(0xF));
	co_return (co_await waitCSIReadReady()) ? _mmio.load(regs::csidr) : ~0;
}

async::result<void> RealtekNic::writeCSIRegister(int reg, uint32_t val) {
	_mmio.store(regs::csidr, val);
	_mmio.store(regs::csiar, flags::csiar::pci_function(_pci_function) | flags::csiar::write(true) | flags::csiar::address(reg) | flags::csiar::byte_enable(0xF));
	co_await waitCSIWriteReady();
}

async::result<void> write8168PhyOCPRegister(uint32_t reg, uint32_t data) {
	(void) reg;
	(void) data;
	assert(!"Not Implemented");
}

async::result<uint32_t> read8168PhyOCPRegister(uint32_t reg) {
	(void) reg;
	assert(!"Not Implemented");
	co_return 0;
}

async::result<void> RealtekNic::enableExitL1() {
	switch(_revision) {
		case MacRevision::MacVer34 ... MacRevision::MacVer36: {
			co_await writeERIRegister(0xD4, 0b1111, co_await readERIRegister(0xD4) | 0x1F00);
			break;
		}
		case MacRevision::MacVer37 ... MacRevision::MacVer38: {
			co_await writeERIRegister(0xD4, 0b1111, co_await readERIRegister(0xD4) | 0x0C00);
			break;
		}
		case MacRevision::MacVer40 ... MacRevision::MacVer63: {
			modify8168MacOCPRegister(0xC0AC, 0, 0x1F80);
			break;
		}
		default: {
			break;
		}
	}
}

void RealtekNic::ringDoorbell() {
	if(_model == PciModel::RTL8125) {
		_mmio.store(regs::rtl8125::tx_start, flags::rtl8125::tx_start::start(true));
	} else {
		_mmio.store(regs::tppoll, flags::tppoll::poll_normal_prio(true));
	}
}

void RealtekNic::printRegisters(std::string_view when) {
	std::println("drivers/rtl8168: dumping registers ({}):", when);
	frg::to(std::cout) << frg::fmt("\t cmd: 0x{:02x}", uint8_t(_mmio.load(regs::cmd))) << frg::endlog;
	frg::to(std::cout) << frg::fmt("\t config1: 0x{:02x}", uint8_t(_mmio.load(regs::config1))) << frg::endlog;
	frg::to(std::cout) << frg::fmt("\t config2: 0x{:02x}", uint8_t(_mmio.load(regs::config2))) << frg::endlog;
	frg::to(std::cout) << frg::fmt("\t config3: 0x{:02x}", uint8_t(_mmio.load(regs::config3))) << frg::endlog;
	frg::to(std::cout) << frg::fmt("\t config4: 0x{:02x}", uint8_t(_mmio.load(regs::config4))) << frg::endlog;
	frg::to(std::cout) << frg::fmt("\t config5: 0x{:02x}", uint8_t(_mmio.load(regs::config5))) << frg::endlog;
	if(_model == PciModel::RTL8125) {
		frg::to(std::cout) << frg::fmt("\t interrupt_mask: 0x{:08x}", uint32_t(_mmio.load(regs::rtl8125::interrupt_mask))) << frg::endlog;
		frg::to(std::cout) << frg::fmt("\t interrupt_status: 0x{:08x}", uint32_t(_mmio.load(regs::rtl8125::interrupt_status))) << frg::endlog;
	} else {
		frg::to(std::cout) << frg::fmt("\t interrupt_mask: 0x{:04x}", uint16_t(_mmio.load(regs::interrupt_mask))) << frg::endlog;
		frg::to(std::cout) << frg::fmt("\t interrupt_status: 0x{:04x}", uint16_t(_mmio.load(regs::interrupt_status))) << frg::endlog;
	}
	frg::to(std::cout) << frg::fmt("\t transmit_config: 0x{:08x}", uint32_t(_mmio.load(regs::transmit_config))) << frg::endlog;
	frg::to(std::cout) << frg::fmt("\t receive_config: 0x{:08x}", uint32_t(_mmio.load(regs::receive_config))) << frg::endlog;
	frg::to(std::cout) << frg::fmt("\t dllpr: 0x{:02x}", uint8_t(_mmio.load(regs::dllpr))) << frg::endlog;
	frg::to(std::cout) << frg::fmt("\t cp_cmd: 0x{:04x}", uint16_t(_mmio.load(regs::cp_cmd))) << frg::endlog;
	frg::to(std::cout) << frg::fmt("\t interrupt_mask: 0x{:04x}", uint16_t(_mmio.load(regs::interrupt_mask))) << frg::endlog;
	frg::to(std::cout) << frg::fmt("\t tx_max_size: 0x{:04x}", uint16_t(_mmio.load(regs::tx_max_size))) << frg::endlog;
	frg::to(std::cout) << frg::fmt("\t rx_max_size: 0x{:04x}", uint16_t(_mmio.load(regs::rx_max_size))) << frg::endlog;
	frg::to(std::cout) << frg::fmt("\t misc: 0x{:08x}", uint32_t(_mmio.load(regs::misc))) << frg::endlog;
	frg::to(std::cout) << frg::fmt("\t misc_1: 0x{:02x}", uint8_t(_mmio.load(regs::misc_1))) << frg::endlog;
	frg::to(std::cout) << frg::fmt("\t phy_status: 0x{:02x}", uint8_t(_mmio.load(regs::phy_status))) << frg::endlog;
	frg::to(std::cout) << frg::fmt("\t interrupt_mitigate: 0x{:04x}", uint16_t(_mmio.load(regs::interrupt_mitigate))) << frg::endlog;
	frg::to(std::cout) << frg::fmt("\t timer_interrupt: 0x{:08x}", _mmio.load(regs::timer_interrupt)) << frg::endlog;
	std::println("\t mar: 0x{:08x}{:08x}", _mmio.load(regs::mar4), _mmio.load(regs::mar0));

	auto config2 = _mmio.load(regs::config2);
	auto config3 = _mmio.load(regs::config3);
	auto config5 = _mmio.load(regs::config5);
	std::println("drivers/rtl8168:   chip ASPM {}, chip CLKREQ {}, jumbo {}, ready to L2/L3 {}, SPI {}",
		(config5 & flags::config5::aspm_enable) ? "on" : "off",
		(config2 & flags::config2::clk_rq_enable) ? "on" : "off",
		(config3 & flags::config3::enable0_jumbo) ? "on" : "off",
		(config3 & flags::config3::enable_l2l3) ? "on" : "off",
		(config5 & flags::config5::spi_enable) ? "on" : "off");
	logPhyStatus(when);
}

void RealtekNic::logPhyStatus(std::string_view when) {
	auto status = _mmio.load(regs::phy_status);
	const char *speed = "unknown speed";
	if(status & flags::phy_status::speed_1000_full)
		speed = "1000 Mbit/s";
	else if(status & flags::phy_status::speed_100)
		speed = "100 Mbit/s";
	else if(status & flags::phy_status::speed_10)
		speed = "10 Mbit/s";

	std::println("drivers/rtl8168: PHYstatus ({}) 0x{:02x}: link {}, {}, {} duplex, flow control rx {} tx {}{}",
		when, uint8_t(status),
		(status & flags::phy_status::link_status) ? "up" : "down", speed,
		(status & flags::phy_status::full_duplex) ? "full" : "half",
		(status & flags::phy_status::rx_flow_control) ? "on" : "off",
		(status & flags::phy_status::tx_flow_control) ? "on" : "off",
		(status & flags::phy_status::tbi_enable) ? ", TBI" : "");
}
