#pragma once

#include <arch/dma_pool.hpp>
#include <arch/dma_structs.hpp>
#include <arch/mem_space.hpp>
#include <array>
#include <async/recurring-event.hpp>
#include <async/result.hpp>
#include <cstdint>
#include <expected>
#include <helix/memory.hpp>
#include <helix/timer.hpp>
#include <netserver/nic.hpp>
#include <nic/igc/igc.hpp>
#include <protocols/hw/client.hpp>
#include <vector>

#include "regs.hpp"

namespace nic::igc {

inline constexpr size_t ringSize = 256;
inline constexpr size_t bufferSize = 2048;
inline constexpr size_t maxFrameLen = 1518;
inline constexpr size_t mtuSize = 1500;

struct [[gnu::packed]] RxDescriptor {
	uint64_t addr;
	uint32_t statusError;
	uint16_t length;
	uint16_t vlan;
};

struct [[gnu::packed]] TxDescriptor {
	uint64_t addr;
	uint32_t cmdTypeLen;
	uint32_t olinfoStatus;
};

struct Buffer {
	uint8_t data[bufferSize];
};

struct IgcNic : nic::Link {
	IgcNic(protocols::hw::Device device, helix::UniqueDescriptor dmaSpace, bool iommuActive);

	async::result<void> send(const arch::dma_buffer_view) override;

	async::result<void> init();

private:
	async::detached processIrqs();
	async::detached processRx();

	async::result<void> reset();
	void readMac(std::array<uint8_t, 6> &out);
	void initRxAddrs(const std::array<uint8_t, 6> &mac);

	async::result<bool> getHwSemaphore();
	void putHwSemaphore();
	async::result<bool> acquireSwfwSync(uint32_t mask);
	async::result<void> releaseSwfwSync(uint32_t mask);

	async::result<std::expected<uint16_t, int>> mdicRead(uint8_t phyReg);
	async::result<std::expected<void, int>> mdicWrite(uint8_t phyReg, uint16_t value);
	async::result<std::expected<uint16_t, int>> mdicWait();

	async::result<void> phyPowerUpAutoneg();
	void setupLink();
	void linkStatus(bool &up, uint32_t &speed, bool &fullDuplex);

	void wrfl() { space_.load(reg::status); }

	void armRx(size_t i) {
		rxDescs_[i].addr = rxBufIova_[i];
		rxDescs_[i].statusError = 0;
		rxDescs_[i].length = 0;
		rxDescs_[i].vlan = 0;
	}

	void reapTx() {
		__sync_synchronize();
		while (txOutstanding_ > 0) {
			if (!(txDescs_[txNtc_].olinfoStatus & desc::txdStatDd))
				break;
			txNtc_ = (txNtc_ + 1) % ringSize;
			--txOutstanding_;
		}
	}

	arch::dma_realm dmaRealm_;
	arch::contiguous_pool dmaPool_;
	helix::UniqueDescriptor dmaSpaceHandle_;
	arch::dma_space dmaSpace_;
	protocols::hw::Device device_;

	helix::Mapping mmioMapping_;
	arch::mem_space space_;

	helix::UniqueDescriptor irq_;

	arch::dma_array<RxDescriptor> rxDescs_;
	arch::dma_array<Buffer> rxBufs_;
	std::vector<uintptr_t> rxBufIova_;

	arch::dma_array<TxDescriptor> txDescs_;
	arch::dma_array<Buffer> txBufs_;
	std::vector<uintptr_t> txBufIova_;

	async::recurring_event rxEvent_;

	size_t rxNtc_ = 0;
	size_t txNtu_ = 0;
	size_t txNtc_ = 0;
	size_t txOutstanding_ = 0;
};

} // namespace nic::igc
