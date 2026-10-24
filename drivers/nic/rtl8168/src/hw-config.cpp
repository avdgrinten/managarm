#include <async/basic.hpp>
#include <cstdint>
#include <initializer_list>
#include <nic/rtl8168/common.hpp>
#include <nic/rtl8168/rtl8168.hpp>
#include <nic/rtl8168/regs.hpp>
#include <nic/rtl8168/debug_options.hpp>
#include <frg/logging.hpp>
#include <helix/timer.hpp>
#include <memory>
#include <print>
#include <unistd.h>

async::result<void> RealtekNic::writeToEPHY(int reg, int value) {
	_mmio.store(regs::ephyar, flags::ephyar::data(value) | flags::ephyar::address(reg) | flags::ephyar::write(true));

	co_await waitEPHYARWriteReady();
	co_await helix::sleepFor(1'000'00);
}

async::result<uint16_t> RealtekNic::readFromEPHY(int reg) {
	_mmio.store(regs::ephyar, flags::ephyar::address(reg));

	co_return (co_await waitEPHYARReadReady()) ? _mmio.load(regs::ephyar) & flags::ephyar::data : ~0;
}

async::result<void> RealtekNic::initializeEPHY(ephy_info* info, int len) {
	while(len--) {
		auto old = co_await readFromEPHY(info->offset);
		uint16_t val = (old & ~info->mask) | info->bits;
		co_await writeToEPHY(info->offset, val);
		if(logHardwareState) {
			auto readback = co_await readFromEPHY(info->offset);
			std::println("drivers/rtl8168: EPHY 0x{:02x}: 0x{:04x} -> 0x{:04x}, read back 0x{:04x}{}",
				info->offset, old, val, readback, readback != val ? " (MISMATCH)" : "");
		}
		info++;
	}
}

async::result<void> RealtekNic::RTL8168gCommonConfiguration() {
	co_await setFifoSize(0x08, 0x10, 0x02, 0x06);
	co_await setPauseThreshold(0x38, 0x48);

	// Use CSI to set the ASPM latency
	co_await writeCSIRegister(0x070C, (co_await readCSIRegister(0x070C) & 0x00FFFFFF) | (0x27 << 24));

	// Reset packet filter
	co_await writeERIRegister(0xDC, 0b1111, co_await readERIRegister(0xDC) & ~1);
	co_await writeERIRegister(0xDC, 0b1111, co_await readERIRegister(0xDC) | 1);

	co_await writeERIRegister(0x2F8, 0b0011, 0x1D8F);

	disableRXDVGate();

	co_await writeERIRegister(0xC0, 0b0011, 0x0000);
	co_await writeERIRegister(0xB8, 0b0011, 0x0000);

	co_await configure8168EEEMac();

	co_await writeERIRegister(0x2FC, 0b1111, (co_await readERIRegister(0x2FC) & ~0x06) | 1);
	co_await writeERIRegister(0x1B0, 0b1111, co_await readERIRegister(0x1B0) & ~(1 << 12));

	disablePCIeL2L3State();
}

// Also handles MacVer34, which is a RTL8168e card, but seems to have a quite similar hardware init phase.
async::result<void> RealtekNic::RTL8168fCommonConfiguration() {
	// Use CSI to set the ASPM latency
	// On MacVer34, this is done in the revision switchcase in configureHardware, as it is done earlier.
	if(_revision != MacRevision::MacVer34) {
		co_await writeCSIRegister(0x070C, (co_await readCSIRegister(0x070C) & 0x00FFFFFF) | (0x27 << 24));
	}

	co_await writeERIRegister(0xC0, 0b0011, 0x0000);
	co_await writeERIRegister(0xB8, 0b1111, 0x0000);

	co_await setFifoSize(0x10, 0x10, 0x02, 0x06);

	// The register inits between MacVer 34 and 35/36 in the Linux kernel are very similar, except this register is set earlier.
	// Given this could have been quite easily broken out into another function if that wasn't the case, we do that here too.
	if(_revision == MacRevision::MacVer34) {
		co_await writeERIRegister(0x1B0, 0b1111, co_await readERIRegister(0x1B0) | (1 << 4));
	}

	// Reset packet filter
	co_await writeERIRegister(0xDC, 0b1111, co_await readERIRegister(0xDC) & ~1);
	co_await writeERIRegister(0xDC, 0b1111, co_await readERIRegister(0xDC) | 1);

	if(_revision != MacRevision::MacVer34) {
		co_await writeERIRegister(0x1B0, 0b1111, co_await readERIRegister(0x1B0) | (1 << 4));
	}
	co_await writeERIRegister(0x1D0, 0b1111, co_await readERIRegister(0x1B0) | (1 << 4) | (1 << 1));
	co_await writeERIRegister(0xCC, 0b1111, 0x50);
	co_await writeERIRegister(0xD0, 0b1111, 0x60);

	// TODO: rtl_disable_clock_request
	_mmio.store(regs::mcu, _mmio.load(regs::mcu) / flags::mcu::now_is_oob(false));
	// TODO: for macver34, config_eee_mac is called here
	_mmio.store(regs::dllpr, _mmio.load(regs::dllpr) | flags::dllpr::pfm_en(true));
	_mmio.store(regs::misc, _mmio.load(regs::misc) | flags::misc::pwm_enable(true));
	_mmio.store(regs::config5, _mmio.load(regs::config5) / flags::config5::spi_enable(false));
}

async::result<void> RealtekNic::configureHardware() {
	// Disable the on-chip timer
	_mmio.store(regs::timer_interrupt, 0);

	switch(_revision) {
		case MacRevision::MacVer32:
		case MacRevision::MacVer33: {
			auto e_info_8168e_1 = std::to_array<struct ephy_info>({
			    {0x00, 0x0200, 0x0100},
			    {0x00, 0x0000, 0x0004},
			    {0x06, 0x0002, 0x0001},
			    {0x06, 0x0000, 0x0030},
			    {0x07, 0x0000, 0x2000},
			    {0x00, 0x0000, 0x0020},
			    {0x03, 0x5800, 0x2000},
			    {0x03, 0x0000, 0x0001},
			    {0x01, 0x0800, 0x1000},
			    {0x07, 0x0000, 0x4000},
			    {0x1e, 0x0000, 0x2000},
			    {0x19, 0xffff, 0xfe6c},
			    {0x0a, 0x0000, 0x0040},
			});

			auto aspmLatency = co_await readCSIRegister(0x070C);
			co_await writeCSIRegister(0x070C, (aspmLatency & 0x00FFFFFF) | (0x27 << 24));
			if(logHardwareState)
				std::println("drivers/rtl8168: CSI 0x070c: 0x{:08x} -> read back 0x{:08x}",
					aspmLatency, co_await readCSIRegister(0x070C));
			co_await initializeEPHY(e_info_8168e_1.data(), e_info_8168e_1.size());

			_mmio.store(regs::misc, _mmio.load(regs::misc) | flags::misc::txpla_rst(true));
			_mmio.store(regs::misc, _mmio.load(regs::misc) / flags::misc::txpla_rst(false));

			_mmio.store(
			    regs::config5, _mmio.load(regs::config5) / flags::config5::spi_enable(false)
			);
			break;
		}
		// MacVer34 seems to have very similar, but slightly different, init logic to MacVers 35/36.
		case MacRevision::MacVer34: {
			static const struct ephy_info e_info_8168e_2[] = {
				{ 0x09, 0x0000,	0x0080 },
				{ 0x19, 0x0000,	0x0224 },
				{ 0x00, 0x0000,	0x0004 },
				{ 0x0c, 0x3df0,	0x0200 },
			};

			co_await writeCSIRegister(0x070C, (co_await readCSIRegister(0x070C) & 0x00FFFFFF) | (0x27 << 24));
			co_await initializeEPHY((ephy_info*)e_info_8168e_2, 4);
			co_await RTL8168fCommonConfiguration();
			break;
		}

		case MacRevision::MacVer35:
		case MacRevision::MacVer36: {
			co_await RTL8168fCommonConfiguration();

			static const ephy_info e_info_8168f_1[] = {
				{ 0x06, 0x00c0,	0x0020 },
				{ 0x08, 0x0001,	0x0002 },
				{ 0x09, 0x0000,	0x0080 },
				{ 0x19, 0x0000,	0x0224 },
				{ 0x00, 0x0000,	0x0008 },
				{ 0x0c, 0x3df0,	0x0200 },
			};

			co_await initializeEPHY((ephy_info*)e_info_8168f_1, 6);
			break;
		}
		case MacRevision::MacVer40: {
			co_await RTL8168gCommonConfiguration();

			static const ephy_info e_info_8168g_1[] = {
				{ 0x00, 0x0008,	0x0000 },
				{ 0x0c, 0x3ff0,	0x0820 },
				{ 0x1e, 0x0000,	0x0001 },
				{ 0x19, 0x8000,	0x0000 }
			};

			co_await initializeEPHY((ephy_info*)e_info_8168g_1, 4);
			break;
		}
		case MacRevision::MacVer42:
		case MacRevision::MacVer43: {
			co_await RTL8168gCommonConfiguration();

			static const ephy_info e_info_8168g_2[] = {
				{ 0x00, 0x0008,	0x0000 },
				{ 0x0c, 0x3ff0,	0x0820 },
				{ 0x19, 0xffff,	0x7c00 },
				{ 0x1e, 0xffff,	0x20eb },
				{ 0x0d, 0xffff,	0x1666 },
				{ 0x00, 0xffff,	0x10a3 },
				{ 0x06, 0xffff,	0xf050 },
				{ 0x04, 0x0000,	0x0010 },
				{ 0x1d, 0x4000,	0x0000 },
			};

			co_await initializeEPHY((ephy_info*)e_info_8168g_2, 9);
			break;
		}
		case MacRevision::MacVer46:
		case MacRevision::MacVer48: {
			static const ephy_info e_info_8168h_1[] = {
				{ 0x1e, 0x0800,	0x0001 },
				{ 0x1d, 0x0000,	0x0800 },
				{ 0x05, 0xffff,	0x2089 },
				{ 0x06, 0xffff,	0x5881 },
				{ 0x04, 0xffff,	0x854a },
				{ 0x01, 0xffff,	0x068b }
			};

			co_await initializeEPHY((ephy_info*)e_info_8168h_1, 6);

			co_await setFifoSize(0x08, 0x10, 0x02, 0x06);
			co_await setPauseThreshold(0x38, 0x48);

			// Use CSI to set the ASPM latency
			co_await writeCSIRegister(0x070C, (co_await readCSIRegister(0x070C) & 0x00FFFFFF) | (0x27 << 24));

			// Reset packet filter
			co_await writeERIRegister(0xDC, 0b1111, co_await readERIRegister(0xDC) & ~1);
			co_await writeERIRegister(0xDC, 0b1111, co_await readERIRegister(0xDC) | 1);

			co_await writeERIRegister(0xDC, 0b1111, co_await readERIRegister(0xDC) | 0x001C);

			co_await writeERIRegister(0x5F0, 0b0011, 0x4F87);

			disableRXDVGate();

			co_await writeERIRegister(0xC0, 0b0011, 0x0000);
			co_await writeERIRegister(0xB8, 0b0011, 0x0000);

			co_await configure8168EEEMac();

			_mmio.store(regs::dllpr, _mmio.load(regs::dllpr) / flags::dllpr::pfm_en(false));
			_mmio.store(regs::misc_1, _mmio.load(regs::misc_1) / flags::misc_1::pfm_d3cold_en(false));
			_mmio.store(regs::dllpr, _mmio.load(regs::dllpr) / flags::dllpr::tx_10m_ps_en(false));

			co_await writeERIRegister(0x1B0, 0b1111, co_await readERIRegister(0x1B0) & ~(1 << 12));

			disablePCIeL2L3State();
			// TODO:
			// 		rg_saw_cnt = phy_read_paged(tp->phydev, 0x0c42, 0x13) & 0x3fff;
			// 		if (rg_saw_cnt > 0) {
			// 			u16 sw_cnt_1ms_ini;
 			// 			sw_cnt_1ms_ini = 16000000/rg_saw_cnt;
 			// 			sw_cnt_1ms_ini &= 0x0fff;
 			// 			modifyMacOCPRegister(0xd412, 0x0fff, sw_cnt_1ms_ini);
			// 		}

			modify8168MacOCPRegister(0xe056, 0x00f0, 0x0070);
			modify8168MacOCPRegister(0xe052, 0x6000, 0x8008);
			modify8168MacOCPRegister(0xe0d6, 0x01ff, 0x017f);
			modify8168MacOCPRegister(0xd420, 0x0fff, 0x047f);

			write8168MacOCPRegister(0xe63e, 0x0001);
			write8168MacOCPRegister(0xe63e, 0x0000);
			write8168MacOCPRegister(0xc094, 0x0000);
			write8168MacOCPRegister(0xc09e, 0x0000);
			break;
		}
		default: {
			std::cerr << "drivers/rtl8168: no hardware configuration logic for MacVer" << _revision << "!" << std::endl;
			assert(!"Not implemented");
		}
	}
}
