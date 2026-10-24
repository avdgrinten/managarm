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
#include <string_view>
#include <unistd.h>

template<typename ConditionFunctor>
async::result<bool> busyWaitFor(std::string_view what, ConditionFunctor&& functor, const int loopTimes, const uint64_t loopDelay) {
	for(int i = 0; i < loopTimes; i++) {
		if(functor()) {
			co_return true;
		}
		co_await helix::sleepFor(loopDelay);
	}

	if(logHardwareState)
		std::println("drivers/rtl8168: timed out waiting for {}", what);
	co_return false;
}

async::result<bool> RealtekNic::RTL8168gWaitLLShareFifoReady() {
	const int loopTimes = 42;
	const uint64_t loopDelay = 1'000'000;

	co_return co_await busyWaitFor("link list FIFO ready", [this] () { return _mmio.load(regs::mcu) & flags::mcu::link_list_ready; },
		loopTimes, loopDelay);
}

async::result<bool> RealtekNic::waitEPHYARReadReady() {
	const int loopTimes = 100;
	const uint64_t loopDelay = 1'000'00;

	co_return co_await busyWaitFor("EPHY read", [this] () { return _mmio.load(regs::ephyar) & flags::ephyar::flag; },
		loopTimes, loopDelay);
}

async::result<bool> RealtekNic::waitEPHYARWriteReady() {
	const int loopTimes = 100;
	const uint64_t loopDelay = 1'000'00;

	co_return co_await busyWaitFor("EPHY write", [this] () { return !(_mmio.load(regs::ephyar) & flags::ephyar::flag); },
		loopTimes, loopDelay);
}

async::result<bool> RealtekNic::waitPHYARReadReady() {
	const int loopTimes = 25;
	const uint64_t loopDelay = 20'000;

	co_return co_await busyWaitFor("PHYAR read", [this] () { return _mmio.load(regs::phy_access) & flags::phy_access::rw; },
		loopTimes, loopDelay);
}

async::result<bool> RealtekNic::waitPHYARWriteReady() {
	const int loopTimes = 25;
	const uint64_t loopDelay = 20'000;

	co_return co_await busyWaitFor("PHYAR write", [this] () { return !(_mmio.load(regs::phy_access) & flags::phy_access::rw); },
		loopTimes, loopDelay);
}

async::result<bool> RealtekNic::waitERIARReadReady() {
	const int loopTimes = 100;
	const uint64_t loopDelay = 1'000'00;

	co_return co_await busyWaitFor("ERI read", [this] () { return _mmio.load(regs::eriar) & flags::eriar::flag; },
		loopTimes, loopDelay);
}

async::result<bool> RealtekNic::waitERIARWriteReady() {
	const int loopTimes = 100;
	const uint64_t loopDelay = 1'000'00;

	co_return co_await busyWaitFor("ERI write", [this] () { return !(_mmio.load(regs::eriar) & flags::eriar::flag); },
		loopTimes, loopDelay);
}

async::result<bool> RealtekNic::waitCSIReadReady() {
	const int loopTimes = 100;
	const uint64_t loopDelay = 1'000'00;

	co_return co_await busyWaitFor("CSI read", [this] () { return _mmio.load(regs::csiar) & flags::csiar::flag; },
		loopTimes, loopDelay);
}

async::result<bool> RealtekNic::waitCSIWriteReady() {
	const int loopTimes = 100;
	const uint64_t loopDelay = 1'000'00;

	co_return co_await busyWaitFor("CSI write", [this] () { return !(_mmio.load(regs::csiar) & flags::csiar::flag); },
		loopTimes, loopDelay);
}

async::result<bool> RealtekNic::waitTallyCommandDone() {
	const int loopTimes = 1000;
	const uint64_t loopDelay = 10'000;

	co_return co_await busyWaitFor("tally counter command", [this] () {
		return !(_mmio.load(regs::counter_addr_low)
			& (flags::counter_addr_low::reset | flags::counter_addr_low::dump));
	}, loopTimes, loopDelay);
}

async::result<bool> RealtekNic::waitTxRxFifoEmpty() {
	const int loopTimes = 42;
	const uint64_t loopDelay = 1'000'000;

	switch(_revision) {
		case MacRevision::MacVer40 ... MacRevision::MacVer53: {
			co_await busyWaitFor("TX FIFO empty (TxConfig)", [this] () { return _mmio.load(regs::transmit_config) & flags::transmit_config::empty; },
				loopTimes, loopDelay);
			co_await busyWaitFor("TX/RX FIFO empty (MCU)", [this] () { auto reg = _mmio.load(regs::mcu); return (reg & flags::mcu::rx_empty) && (reg & flags::mcu::tx_empty); },
				loopTimes, loopDelay);
			break;
		}
		case MacRevision::MacVer61: {
			_mmio.store(regs::cmd, _mmio.load(regs::cmd) / flags::cmd::stop_req(true));
			co_await busyWaitFor("TX/RX FIFO empty (MCU)", [this] () {
				auto reg = _mmio.load(regs::mcu);
				return (reg & flags::mcu::rx_empty) && (reg & flags::mcu::tx_empty);
			}, loopTimes, loopDelay);
			break;
		}
		case MacRevision::MacVer63 ... MacRevision::MacVer70: {
			_mmio.store(regs::cmd, _mmio.load(regs::cmd) / flags::cmd::stop_req(true));
			co_await busyWaitFor("TX/RX FIFO empty (MCU)", [this] () {
				auto reg = _mmio.load(regs::mcu);
				return (reg & flags::mcu::rx_empty) && (reg & flags::mcu::tx_empty);
			}, loopTimes, loopDelay);
			co_await busyWaitFor("TX/RX FIFO empty (IntrMitigate)", [this] () {
				auto reg = _mmio.load(regs::interrupt_mitigate);
				return (uint16_t(reg) & 0x0103) == 0x0103;
			}, loopTimes, loopDelay);
			break;
		}
		default: {
			break;
		}
	}

	co_return true;
}
