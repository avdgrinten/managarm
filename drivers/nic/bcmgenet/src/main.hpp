#include <format>
#include <arpa/inet.h>
#include <arch/barrier.hpp>
#include <arch/mem_space.hpp>
#include <async/recurring-event.hpp>
#include <helix/memory.hpp>
#include <protocols/hw/client.hpp>
#include <nic/bcmgenet/bcmgenet.hpp>
#include <netserver/phy.hpp>


namespace nic::bcmgenet {

constexpr size_t mtuSize = 1500;
constexpr size_t ethernetHeaderSize = 14;

constexpr size_t bufferSize = 2048;
constexpr size_t defaultRing = 16;
constexpr size_t descriptorCount = 256;

struct BcmGenetNic;

struct BcmGenetMii : nic::Mdio {
	BcmGenetMii(BcmGenetNic *parent) : parent_{parent} {}

	async::result<std::expected<uint16_t, nic::PhyError>>
	read(uint8_t phyAddress, uint8_t registerNum) override;

	async::result<std::expected<void, nic::PhyError>>
	write(uint8_t phyAddress, uint8_t registerNum, uint16_t value) override;

private:
	BcmGenetNic *parent_;
};


struct BcmGenetNic : nic::Link {
	friend struct BcmGenetMii;

	BcmGenetNic(uintptr_t base, protocols::hw::Device device,
			helix_ng::Mapping mapping,
			helix::UniqueDescriptor irq,
			nic::PhyMode phyMode,
			nic::MacAddress macAddr)
	: nic::Link{mtuSize, &dmaPool_}
	, base{base}
	, device_{std::move(device)}
	, mmioMapping_{std::move(mapping)}
	, irq_{std::move(irq)}
	, space_{mmioMapping_.get()}
	, phyMode_{phyMode}
	, mii_{std::make_shared<BcmGenetMii>(this)} { mac_ = macAddr; }

	async::result<void> send(const arch::dma_buffer_view) override;

	async::result<void> initialize();

	const uintptr_t base;

private:
	async::result<void> reset_();

	void updateLink_();

	void setupBufs_();
	void writeRxdesc_(size_t idx);
	void writeTxdesc_(size_t idx, const arch::dma_buffer_view buf, arch::bit_value<uint32_t> flags);

	void setupRings_();

	void writeMacAddr_();

	void setupRxFilter_();

	async::detached processIrqs_();
	async::detached processRx_();

	void processRxRing_();
	void processTxRing_();

	void debug_();

private:
	protocols::hw::Device device_;
	helix_ng::Mapping mmioMapping_;
	helix::UniqueDescriptor irq_;
	arch::dma_barrier barrier_{false};
	std::vector<arch::dma_buffer> txBufs_;
	std::vector<arch::dma_buffer> rxBufs_;
	arch::mem_space space_;
	arch::dma_realm dmaRealm_;
	arch::contiguous_pool dmaPool_{&dmaRealm_, {.addressBits = 64}};
	nic::PhyMode phyMode_;

	async::recurring_event rxEvent_;
	uint16_t rxCidx_ = 0;
	uint16_t rxPidx_ = 0;
	size_t rxInFlight_() const {
		if (rxCidx_ > rxPidx_) {
			return rxPidx_ + 0x10000 - rxCidx_;
		} else {
			return rxPidx_ - rxCidx_;
		}
	}

	async::recurring_event txEvent_;
	uint16_t txCidx_ = 0;
	uint16_t txPidx_ = 0;
	size_t txInFlight_() const {
		if (txCidx_ > txPidx_) {
			return txPidx_ + 0x10000 - txCidx_;
		} else {
			return txPidx_ - txCidx_;
		}
	}

	std::shared_ptr<BcmGenetMii> mii_;
	std::shared_ptr<nic::EthernetPhy> phy_;
};

} // namespace nic::bcmgenet

template<>
struct std::formatter<nic::bcmgenet::BcmGenetNic *, char> {
	template<class Ctx>
	constexpr Ctx::iterator parse(Ctx &ctx) {
		return ctx.begin();
	}

	template<class Ctx>
	Ctx::iterator format(nic::bcmgenet::BcmGenetNic *nic, Ctx &ctx) const {
		return std::format_to(ctx.out(), "bcmgenet dt.{:08x}:", nic->base);
	}
};
