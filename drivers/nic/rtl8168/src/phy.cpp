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

async::result<void> RealtekNic::writeRTL8168gMDIO(int reg, int val) {
	(void) val;

	if(reg == 0x1F) { assert(!"Not implemented"); }

	// TODO: OCP base memes

	co_return;
}

async::result<int> RealtekNic::readRTL8168gMDIO(int reg) {
	if(reg == 0x1F) { assert(!"Not implemented"); }


	co_return 0;
}


async::result<void> RealtekNic::writeR8169MDIO(int reg, int val) {
	_mmio.store(regs::phy_access, flags::phy_access::rw(true) | flags::phy_access::addr(reg)
		| flags::phy_access::data(val));

	co_await waitPHYARWriteReady();
	// The hardware requires a 20 us delay before the next command.
	co_await helix::sleepFor(20'000);
}

async::result<int> RealtekNic::readR8169MDIO(int reg) {
	_mmio.store(regs::phy_access, flags::phy_access::addr(reg));

	int val = -1;
	if(co_await waitPHYARReadReady())
		val = _mmio.load(regs::phy_access) & flags::phy_access::data;

	co_await helix::sleepFor(20'000);
	co_return val;
}

async::result<void> RealtekNic::writePHY(int reg, int val) {
	switch(_revision) {
		case MacRevision::MacVer28:
		case MacRevision::MacVer31: {
			assert(!"Not implemented");
			break;
		}
		case MacRevision::MacVer40 ... MacRevision::MacVer63: {
			co_await writeRTL8168gMDIO(reg, val);
			break;
		}
		case MacRevision::MacVer02 ... MacRevision::MacVer26:
		case MacRevision::MacVer29 ... MacRevision::MacVer30:
		case MacRevision::MacVer32 ... MacRevision::MacVer39: {
			co_await writeR8169MDIO(reg, val);
			break;
		}
		default: {
			assert(!"Not implemented");
			break;
		}
	}
}

async::result<int> RealtekNic::readPHY(int reg) {
	switch(_revision) {
		case MacRevision::MacVer28:
		case MacRevision::MacVer31: {
			assert(!"Not implemented");
			break;
		}
		case MacRevision::MacVer40 ... MacRevision::MacVer63: {
			co_return co_await readRTL8168gMDIO(reg);
		}
		case MacRevision::MacVer02 ... MacRevision::MacVer26:
		case MacRevision::MacVer29 ... MacRevision::MacVer30:
		case MacRevision::MacVer32 ... MacRevision::MacVer39: {
			co_return co_await readR8169MDIO(reg);
		}
		default: {
			assert(!"Not implemented");
			break;
		}
	}

	assert(!"Not reachable");
	__builtin_unreachable();
}

// TODO: implement PHY control
//       this is kind of weird, but we can (hopefully...) get
//       away with not doing this for now PHY bring-up is partially
//       implemented in a off-driver system in linux, since the phy
//       models are not really network card dependent
void RealtekNic::configurePHY() {
//	switch(_revision) {
//		case MacRevision::MacVer42 ... MacRevision::MacVer44: {
//
//		}
//		default: {
//			std::cerr << "drivers/rtl8168: PHY configure for MacVer" << _revision << " is not implemented!" << std::endl;
//			assert(!"Not implemented");
//		}
//	}
}

namespace {

const char *onOff(bool v) {
	return v ? "on" : "off";
}

// Highest mode that both our advertisement and the link partner's include.
const char *commonMode(int anar, int anlpar, int gbcr, int gbsr) {
	if((gbcr & (1 << 9)) && (gbsr & (1 << 11)))
		return "1000 Mbit/s full duplex";
	if((gbcr & (1 << 8)) && (gbsr & (1 << 10)))
		return "1000 Mbit/s half duplex";
	auto common = anar & anlpar;
	if(common & (1 << 8))
		return "100 Mbit/s full duplex";
	if(common & (1 << 7))
		return "100 Mbit/s half duplex";
	if(common & (1 << 6))
		return "10 Mbit/s full duplex";
	if(common & (1 << 5))
		return "10 Mbit/s half duplex";
	return "none";
}

} // namespace

async::result<void> RealtekNic::dumpPhyState(std::string_view when) {
	if(_revision == MacRevision::MacVer28 || _revision == MacRevision::MacVer31
			|| _revision >= MacRevision::MacVer40) {
		std::println("drivers/rtl8168: PHY state ({}): PHY access is not implemented for MacVer{:02}",
			when, uint16_t(_revision));
		co_return;
	}

	// Firmware may leave a vendor page selected, which hides the standard registers.
	auto page = co_await readPHY(0x1F);
	if(page < 0) {
		std::println("drivers/rtl8168: PHY state ({}): PHY does not respond to MDIO reads", when);
		co_return;
	}
	if(page)
		co_await writePHY(0x1F, 0);

	auto bmcr = co_await readPHY(0x00);
	// The link status bit latches low; only the second read reflects the current state.
	auto bmsrLatched = co_await readPHY(0x01);
	auto bmsr = co_await readPHY(0x01);
	auto id1 = co_await readPHY(0x02);
	auto id2 = co_await readPHY(0x03);
	auto anar = co_await readPHY(0x04);
	auto anlpar = co_await readPHY(0x05);
	auto aner = co_await readPHY(0x06);
	auto gbcr = co_await readPHY(0x09);
	auto gbsr = co_await readPHY(0x0A);
	auto physr = co_await readPHY(0x11);

	if(page)
		co_await writePHY(0x1F, page);

	std::println("drivers/rtl8168: PHY state ({}): page 0x{:04x}, PHY ID {:04x}:{:04x}",
		when, page, id1, id2);
	std::println("drivers/rtl8168:   BMCR 0x{:04x}: reset {}, loopback {}, autoneg {}, power down {}, isolate {}",
		bmcr, onOff(bmcr & (1 << 15)), onOff(bmcr & (1 << 14)), onOff(bmcr & (1 << 12)),
		onOff(bmcr & (1 << 11)), onOff(bmcr & (1 << 10)));
	std::println("drivers/rtl8168:   BMSR 0x{:04x} (latched 0x{:04x}): link {}, autoneg complete {}, remote fault {}",
		bmsr, bmsrLatched, (bmsr & (1 << 2)) ? "up" : "down", onOff(bmsr & (1 << 5)),
		onOff(bmsr & (1 << 4)));
	std::println("drivers/rtl8168:   ANAR 0x{:04x}, ANLPAR 0x{:04x}, ANER 0x{:04x}, GBCR 0x{:04x}, GBSR 0x{:04x}",
		anar, anlpar, aner, gbcr, gbsr);
	std::println("drivers/rtl8168:   common advertised mode: {}", commonMode(anar, anlpar, gbcr, gbsr));
	// Register 0x11 is decoded with the RTL8211 PHYSR layout.
	static const char *physrSpeeds[] = {"10", "100", "1000", "reserved"};
	std::println("drivers/rtl8168:   PHYSR 0x{:04x}: {} Mbit/s, {} duplex, resolved {}, link {}",
		physr, physrSpeeds[(physr >> 14) & 3], (physr & (1 << 13)) ? "full" : "half",
		onOff(physr & (1 << 11)), (physr & (1 << 10)) ? "up" : "down");

	if(id1 == 0xFFFF || id1 == 0)
		std::println("drivers/rtl8168:   WARNING: implausible PHY ID, MDIO access may not work");
	if(bmcr & (1 << 11))
		std::println("drivers/rtl8168:   WARNING: PHY is powered down, the link cannot come up");
	if(bmcr & (1 << 10))
		std::println("drivers/rtl8168:   WARNING: PHY is isolated from the MAC");
	if(bmcr & (1 << 14))
		std::println("drivers/rtl8168:   WARNING: PHY is in loopback mode");
	if(!(bmcr & (1 << 12)))
		std::println("drivers/rtl8168:   WARNING: autonegotiation is disabled");
}
