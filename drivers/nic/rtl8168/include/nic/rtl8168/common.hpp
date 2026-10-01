#pragma once

#include <async/basic.hpp>
#include <arch/mem_space.hpp>
#include <helix/memory.hpp>
#include <netserver/nic.hpp>
#include <nic/rtl8168/rx.hpp>
#include <nic/rtl8168/tx.hpp>
#include <protocols/hw/client.hpp>
#include <memory>

constexpr size_t NUM_RX_DESCRIPTORS = 256;
constexpr size_t NUM_TX_DESCRIPTORS = 256;

struct RxQueue;
struct TxQueue;
struct TxQueue8168;

enum class PciModel : uint16_t {
	RTL8136 = 0x8136,
	RTL8125 = 0x8125,
	RTL8126 = 0x8126,
	RTL8161 = 0x8161,
	RTL8162 = 0x8162,
	RTL8167 = 0x8167,
	RTL8168 = 0x8168,
	RTL8169 = 0x8169,
};

struct RealtekNic : nic::Link {
	friend TxQueue8168;
public:
	RealtekNic(protocols::hw::Device device, helix::UniqueDescriptor dmaSpace, bool iommuActive);

	enum MacRevision : uint16_t {
		MacVerNone = 0, // Error case
		MacVer02 = 2,
		MacVer03 = 3,
		MacVer04 = 4,
		MacVer05 = 5,
		MacVer06 = 6,
		MacVer07 = 7,
		MacVer08 = 8,
		MacVer09 = 9,
		MacVer10 = 10,
		MacVer11 = 11,
		MacVer14 = 14,
		MacVer17 = 17,
		MacVer18 = 18,
		MacVer19 = 19,
		MacVer20 = 20,
		MacVer21 = 21,
		MacVer22 = 22,
		MacVer23 = 23,
		MacVer24 = 24,
		MacVer25 = 25,
		MacVer26 = 26,
		MacVer28 = 28,
		MacVer29 = 29,
		MacVer30 = 30,
		MacVer31 = 31,
		MacVer32 = 32,
		MacVer33 = 33,
		MacVer34 = 34,
		MacVer35 = 35,
		MacVer36 = 36,
		MacVer37 = 37,
		MacVer38 = 38,
		MacVer39 = 39,
		MacVer40 = 40,
		MacVer42 = 42,
		MacVer43 = 43,
		MacVer44 = 44,
		MacVer46 = 46,
		MacVer48 = 48,
		MacVer51 = 51,
		MacVer52 = 52,
		MacVer53 = 53,
		MacVer61 = 61,
		MacVer63 = 63,
		MacVer70 = 70,
		MacVer80 = 80,
	};
	enum class DashType : uint8_t {
		DashNone,
		DashDP,
		DashEP
	};

	async::result<size_t> receive(arch::dma_buffer_view) override;
	async::result<void> send(const arch::dma_buffer_view) override;

	async::result<void> init();

	void ringDoorbell();

	void printRegisters();

	arch::dma_space &dmaSpace() {
		return dmaSpace_;
	}

	// Workarounds
	bool restart_transmitter_on_tx_ok_and_tx_desc_unavailable = true;
	bool use_timer_interrupt_to_check_received_packets = false;
private:
	async::result<void> getMmio();
	void determineMacRevision();
	void determineDashType();

	// The RTL8125, RTL8126 and RTL8127 share their register layout; Linux' rtl_is_8125().
	bool is8125Family() const {
		return _revision >= MacRevision::MacVer61;
	}

	// The core configuration registers have a hardware lock
	void unlockConfigRegisters();
	void lockConfigRegisters();

	void setupRxDescriptors();
	void setupTxDescriptors();
	void configureQueues();

	void closeRX();

	void maskIRQsAndAck();
	async::result<void> enableRXDVGate();
	void disableRXDVGate();
	void setHardwareASPMClockEnable(bool val);

	async::result<bool> startCard();
	async::result<bool> up();
	[[maybe_unused]] async::result<bool> down();

	void configurePHY();
	async::result<bool> cleanup();
	async::result<void> issueHardwareReset();

	async::result<void> enableExitL1();

	// Busy wait loops
	async::result<bool> RTL8168gWaitLLShareFifoReady();
	async::result<bool> waitTxRxFifoEmpty();

	async::result<bool> waitEPHYARReadReady();
	async::result<bool> waitEPHYARWriteReady();

	async::result<bool> waitERIARReadReady();
	async::result<bool> waitERIARWriteReady();

	async::result<bool> waitCSIReadReady();
	async::result<bool> waitCSIWriteReady();


	// Indirect register access functions
	// TOOD: these functions should lock

	/// OCP code
	// 8168 MAC OCP
	uint16_t read8168MacOCPRegister(uint32_t reg);
	void write8168MacOCPRegister(uint32_t reg, uint16_t val);
	inline void modify8168MacOCPRegister(uint32_t reg, uint16_t mask, uint16_t set) {
		uint16_t data = read8168MacOCPRegister(reg);
		write8168MacOCPRegister(reg, (data & ~mask) | set);
	}
	// 8168 PHY OCP
	async::result<void> write8168PhyOCPRegister(uint32_t reg, uint32_t data);
	async::result<uint32_t> read8168PhyOCPRegister(uint32_t reg);

	// ERI code
	async::result<uint32_t> readERIRegister(int reg);
	async::result<void> writeERIRegister(int reg, uint8_t mask, uint32_t val);

	// CSI code
	async::result<uint32_t> readCSIRegister(int reg);
	async::result<void> writeCSIRegister(int reg, uint32_t val);

	// EPHY code
	struct ephy_info {
		uint32_t offset;
		uint16_t mask;
		uint16_t bits;
	};
	async::result<void> initializeEPHY(ephy_info* info, int len);
	async::result<void> writeToEPHY(int reg, int value);
	async::result<uint16_t> readFromEPHY(int reg);

	// PHY code
	async::result<void> writePHY(int reg, int val);
	async::result<int> readPHY(int reg);


	/// MDIO Code
	// RTL8168g MDIO
	async::result<void> writeRTL8168gMDIO(int reg, int val);
	async::result<int> readRTL8168gMDIO(int reg);


	void setRxConfigRegisters();
	void setTxConfigRegisters();
	async::result<void> initializeHardware();

	// The hardware configuration functions.
	async::result<void> RTL8168gCommonConfiguration();
	async::result<void> RTL8168fCommonConfiguration();
	async::result<void> configureHardware();

	async::result<void> setFifoSize(uint16_t rx_static, uint16_t tx_static, uint16_t rx_dynamic, uint16_t tx_dynamic) {
		co_await writeERIRegister(0xC8, 0b1111, (rx_static << 16) | rx_dynamic);
		co_await writeERIRegister(0xE8, 0b1111, (tx_static << 16) | tx_dynamic);
	}

	async::result<void> setPauseThreshold(uint8_t low, uint8_t high) {
		co_await writeERIRegister(0xCC, 0b0001, low);
		co_await writeERIRegister(0xD0, 0b0001, high);
	}

	async::result<void> configure8168EEEMac();
	void disablePCIeL2L3State();

	// This function loads something from PCI, forcing some
	// less-cooperative PCI controllers to commit writes
	constexpr void forcePCICommit() {
			constexpr arch::scalar_register<uint32_t> a_register{0x00};
			[[maybe_unused]] volatile auto c = _mmio.load(a_register);
	}

	async::detached processIrqs();

	helix::Mapping _mmio_mapping;
	arch::mem_space _mmio;

	arch::dma_realm _dmaRealm;
	arch::contiguous_pool _dmaPool{&_dmaRealm, {.addressBits = 64}};
	helix::UniqueDescriptor dmaSpaceHandle_;
	arch::dma_space dmaSpace_;
	protocols::hw::Device _device;

	helix::UniqueDescriptor _irq;

	std::unique_ptr<RxQueue> _rxQueue;
	std::unique_ptr<TxQueue> _txQueue;

	PciModel _model;
	bool _has_gmii = true; // Has GMII; basically, is this card gigabit?
	MacRevision _revision;
	DashType _dash_type;
	uint8_t _pci_function = 0;
};
