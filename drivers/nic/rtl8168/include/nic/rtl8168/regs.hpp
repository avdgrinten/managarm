#pragma once

#include <arch/variable.hpp>

namespace regs {

	constexpr arch::scalar_register<uint32_t> idr0{0x00};
	constexpr arch::scalar_register<uint32_t> idr4{0x04};
	constexpr arch::scalar_register<uint8_t> eee_led{0x1B};
	constexpr arch::scalar_register<uint32_t> tnpds_low{0x20};
	constexpr arch::scalar_register<uint32_t> tnpds_high{0x24};
	constexpr arch::scalar_register<uint32_t> thpds_low{0x28};
	constexpr arch::scalar_register<uint32_t> thpds_high{0x2C};
	constexpr arch::scalar_register<uint32_t> rbstart{0x30};
	constexpr arch::bit_register<uint8_t> int_cfg0_8125{0x34};
	constexpr arch::bit_register<uint8_t> cmd{0x37};
	constexpr arch::bit_register<uint8_t> tppoll{0x38};
	constexpr arch::bit_register<uint8_t> config1{0x52};
	constexpr arch::bit_register<uint8_t> config2{0x53};
	constexpr arch::bit_register<uint8_t> config3{0x54};
	constexpr arch::bit_register<uint8_t> config5{0x56};
	constexpr arch::scalar_register<uint32_t> timer_count{0x48};
	constexpr arch::scalar_register<uint32_t> timer_interrupt{0x58};
	constexpr arch::scalar_register<uint32_t> rdsar_low{0xE4};
	constexpr arch::scalar_register<uint32_t> rdsar_high{0xE8};
	constexpr arch::bit_register<uint16_t> interrupt_mask{0x3C};
	constexpr arch::scalar_register<uint16_t> interrupt_mask_val{0x3C};
	constexpr arch::bit_register<uint16_t> interrupt_status{0x3E};
	constexpr arch::scalar_register<uint16_t> interrupt_status_val{0x3E};
	constexpr arch::bit_register<uint32_t> transmit_config{0x40};
	constexpr arch::bit_register<uint32_t> receive_config{0x44};
	constexpr arch::scalar_register<uint32_t> mpc{0x4C};
	constexpr arch::scalar_register<uint8_t> cr9346{0x50};
	constexpr arch::scalar_register<uint8_t> misr{0x5C};
	constexpr arch::bit_register<uint32_t> phy_access{0x60};
	constexpr arch::scalar_register<uint32_t> csidr{0x64};
	constexpr arch::bit_register<uint32_t> csiar{0x68};
	constexpr arch::bit_register<uint8_t> phy_status{0x6C};
	constexpr arch::scalar_register<uint32_t> eridr{0x70};
	constexpr arch::bit_register<uint32_t> eriar{0x74};
	constexpr arch::scalar_register<uint16_t> int_cfg1_8125{0x7A};
	constexpr arch::bit_register<uint32_t> ephyar{0x80};
	constexpr arch::scalar_register<uint32_t> ocpdr{0xb0};
	constexpr arch::bit_register<uint32_t> gphy_ocp{0xb8};
	constexpr arch::bit_register<uint8_t> dllpr{0xd0};
	constexpr arch::bit_register<uint8_t> mcu{0xd3};
	constexpr arch::bit_register<uint8_t> tppoll_8139{0xD9};
	constexpr arch::scalar_register<uint16_t> rx_max_size{0xDA};
	constexpr arch::bit_register<uint16_t> cp_cmd{0xE0};
	constexpr arch::bit_register<uint16_t> interrupt_mitigate{0xE2};
	constexpr arch::scalar_register<uint8_t> tx_max_size{0xEC};
	constexpr arch::scalar_register<uint32_t> mac0_bkp_low{0x19E0};
	constexpr arch::scalar_register<uint32_t> mac0_bkp_high{0x19E4};

	// These registers are named like this!
	// Yes, they clash, but they also do in the linux driver...
	constexpr arch::bit_register<uint32_t> misc{0xF0};
	constexpr arch::bit_register<uint8_t> misc_1{0xF2};

// The RTL8125 has some special / different registers
namespace rtl8125 {
	// Yes, the types here are correct
	constexpr arch::bit_register<uint32_t> interrupt_mask{0x38};
	constexpr arch::scalar_register<uint32_t> interrupt_mask_val{0x38};
	constexpr arch::bit_register<uint32_t> interrupt_status{0x3C};
	constexpr arch::scalar_register<uint32_t> interrupt_status_val{0x3C};
	constexpr arch::bit_register<uint16_t> tx_start{0x90};
	constexpr arch::scalar_register<uint16_t> reg_0x382{0x382};
	constexpr arch::scalar_register<uint8_t> reg_0xd8{0xD8};
	constexpr arch::scalar_register<uint16_t> reg_0x1880{0x1880};
	constexpr arch::scalar_register<uint32_t> rss_ctrl{0x4500};
	constexpr arch::scalar_register<uint16_t> q_num_ctrl{0x4800};
}

} // namespace regs

namespace flags {

namespace cp_cmd {
	constexpr arch::field<uint16_t, bool> vlan_detag{6, 1};
	constexpr arch::field<uint16_t, bool> checksum{5, 1};
	constexpr arch::field<uint16_t, bool> pci_mul_rw{3, 1};
	constexpr arch::field<uint16_t, bool> tx_enable{0, 1};
	constexpr arch::field<uint16_t, bool> rx_enable{1, 1};
}

namespace cmd {
	constexpr arch::field<uint8_t, bool> stop_req{7, 1};
	constexpr arch::field<uint8_t, bool> transmitter{2, 1};
	constexpr arch::field<uint8_t, bool> receiver{3, 1};
	constexpr arch::field<uint8_t, bool> reset{4, 1};
}

namespace config2 {
	constexpr arch::field<uint8_t, bool> clk_rq_enable{7, 1};
}

namespace int_cfg0_8125 {
	constexpr arch::field<uint8_t, bool> clk_rq_enable{3, 1};
}

namespace config3 {
	constexpr arch::field<uint8_t, bool> enable_beacon{0, 1};
	constexpr arch::field<uint8_t, bool> enable_l2l3{1, 1};
	constexpr arch::field<uint8_t, bool> enable0_jumbo{2, 1};
	constexpr arch::field<uint8_t, bool> linkup_wakeup{4, 1};
	constexpr arch::field<uint8_t, bool> magic_packet_wakeup{5, 1};
}

namespace config5 {
	constexpr arch::field<uint8_t, bool> aspm_enable{0, 1};
	constexpr arch::field<uint8_t, bool> spi_enable{3, 1};
}

namespace interrupt_status {
	constexpr arch::field<uint32_t, bool> rx_ok{0, 1};
	constexpr arch::field<uint32_t, bool> rx_err{1, 1};
	constexpr arch::field<uint32_t, bool> tx_ok{2, 1};
	constexpr arch::field<uint32_t, bool> tx_err{3, 1};
	constexpr arch::field<uint32_t, bool> rx_overflow{4, 1};
	constexpr arch::field<uint32_t, bool> link_change{5, 1};
	constexpr arch::field<uint32_t, bool> rx_fifo_overflow{6, 1};
	constexpr arch::field<uint32_t, bool> tx_desc_unavailable{7, 1};
	constexpr arch::field<uint32_t, bool> sw_int{9, 1};
	constexpr arch::field<uint32_t, bool> pcs_timeout{14, 1};
	constexpr arch::field<uint32_t, bool> system_error{15, 1};
}

namespace phy_access {
	constexpr arch::field<uint32_t, bool> bmcr_auto_negotiation{12, 1};
	constexpr arch::field<uint32_t, bool> bmcr_reset{15, 1};
	constexpr arch::field<uint32_t, uint16_t> data{0, 16};
	constexpr arch::field<uint32_t, uint8_t> addr{16, 5};
	constexpr arch::field<uint32_t, bool> rw{31, 1};
}

namespace cr9346 {
	constexpr arch::field<uint8_t, uint8_t> operating_mode{6, 2};
	constexpr uint8_t lock_regs = 0;
	constexpr uint8_t unlock_regs = 3;
}

namespace transmit_config {
	constexpr arch::field<uint32_t, uint8_t> mxdma{8, 3};
	constexpr uint8_t mxdma_unlimited = 0b111;
	constexpr uint8_t mxdma_2048 = 0b111;
	constexpr uint8_t mxdma_burst = 0b111;

	constexpr arch::field<uint32_t, uint8_t> ifg{24, 2};
	constexpr uint8_t ifg_normal = 0b11;

	constexpr arch::field<uint32_t, bool> auto_fifo{7, 1};
	constexpr arch::field<uint32_t, bool> empty{11, 1};

	// This field is used to detect which revision the card is.
	// It encompasses several fields and a couple reserved regions
	// (depending on which revision of card)
	constexpr arch::field<uint32_t, uint32_t> detect_bits{20, 12};
}

namespace receive_config {
	constexpr arch::field<uint32_t, uint8_t> mxdma{8, 3};
	constexpr uint8_t mxdma_unlimited = 0b111;

	constexpr arch::field<uint32_t, uint8_t> rxfth{13, 3};
	constexpr uint8_t rxfth_none = 0b111;

	constexpr arch::field<uint32_t, bool> accept_packet_with_destination_addr{0, 1};
	constexpr arch::field<uint32_t, bool> accept_packet_with_physical_match{1, 1};
	constexpr arch::field<uint32_t, bool> accept_multicast_packets{2, 1};
	constexpr arch::field<uint32_t, bool> accept_broadcast_packets{3, 1};
	constexpr arch::field<uint32_t, bool> wrap{7, 1};

	constexpr arch::field<uint32_t, bool> rx_early_off{11, 1};
	// Same bit as rx_early_off; only on the RTL8125B and later.
	constexpr arch::field<uint32_t, bool> rx_pause_slot_on{11, 1};
	constexpr arch::field<uint32_t, bool> rx_multi_en{14, 1};
	constexpr arch::field<uint32_t, bool> rx128_int_en{15, 1};

	// TODO: investigate this
	// Not sure what register this is tbh
	constexpr arch::field<uint32_t, uint8_t> rx_fetch{27, 4};
	constexpr uint8_t rx_fetch_default_8125 = 8;

	// Used during card cleanup.
	constexpr arch::field<uint32_t, uint16_t> accept_mask_bits{0, 6};
	constexpr uint16_t accept_mask = 0x3F;
	constexpr uint16_t accept_ok = 0x0F;
}

namespace interrupt_mask {
	constexpr arch::field<uint16_t, bool> rx_ok{0, 1};
	constexpr arch::field<uint16_t, bool> rx_err{1, 1};
	constexpr arch::field<uint16_t, bool> tx_ok{2, 1};
	constexpr arch::field<uint16_t, bool> tx_err{3, 1};
	constexpr arch::field<uint16_t, bool> link_change{5, 1};
	constexpr arch::field<uint16_t, bool> rx_fifo_overflow{6, 1};
	constexpr arch::field<uint16_t, bool> tx_desc_unavailable{7, 1};
	constexpr arch::field<uint16_t, bool> sw_int{9, 1};
	constexpr arch::field<uint16_t, bool> pcs_timeout{14, 1};
	constexpr arch::field<uint16_t, bool> system_error{15, 1};
}

namespace tppoll {
	constexpr arch::field<uint8_t, bool> poll_normal_prio(6, 1);
}


namespace eridr {

}

namespace eriar {
	// TODO: check how big this is
	constexpr arch::field<uint32_t, uint8_t> address(0, 8);
	// Mask of which _bytes_ we want to overwrite
	constexpr arch::field<uint32_t, uint8_t> mask(12, 4);
	constexpr arch::field<uint32_t, uint8_t> type(16, 2);
	constexpr uint8_t exgmac = 0x00;
	constexpr uint8_t msi_x = 0x01;
	constexpr uint8_t asf = 0x02;
	constexpr uint8_t oob = 0x02;

	constexpr arch::field<uint32_t, bool> write(31, 1);
	constexpr arch::field<uint32_t, bool> flag(31, 1);
}

namespace ephyar {
	constexpr arch::field<uint32_t, uint16_t> data(0, 16);
	constexpr arch::field<uint32_t, uint16_t> address(16, 9);
	constexpr arch::field<uint32_t, bool> write(31, 1);
	constexpr arch::field<uint32_t, bool> flag(31, 1);
}

namespace gphy_ocp {
	constexpr arch::field<uint32_t, uint16_t> data(0, 16);
	constexpr arch::field<uint32_t, uint16_t> reg(15, 16);
	constexpr arch::field<uint32_t, bool> flag(31, 1);
}

namespace dllpr {
	constexpr arch::field<uint8_t, bool> pfm_en(6, 1);
	constexpr arch::field<uint8_t, bool> tx_10m_ps_en(7, 1);
}

namespace csiar {
	constexpr arch::field<uint32_t, uint16_t> address(0, 12);
	constexpr arch::field<uint32_t, uint8_t> byte_enable(12, 4);
	constexpr arch::field<uint32_t, uint8_t> pci_function(16, 3);
	constexpr arch::field<uint32_t, bool> write(31, 1);
	constexpr arch::field<uint32_t, bool> flag(31, 1);
}

namespace mcu {
	constexpr arch::field<uint8_t, bool> now_is_oob(7, 1);
	constexpr arch::field<uint8_t, bool> tx_empty(5, 1);
	constexpr arch::field<uint8_t, bool> rx_empty(4, 1);
	constexpr arch::field<uint8_t, bool> ndp(3, 1);
	constexpr arch::field<uint8_t, bool> oob_reset(2, 1);
	constexpr arch::field<uint8_t, bool> link_list_ready(1, 1);
}

namespace misc {
	constexpr arch::field<uint32_t, bool> rxdv_gate(19, 1);
	constexpr arch::field<uint32_t, bool> pwm_enable(22, 1);
	constexpr arch::field<uint32_t, bool> txpla_rst(29, 1);
}

namespace misc_1 {
	constexpr arch::field<uint8_t, bool> pfm_d3cold_en(6, 1);
}

namespace rtl8125 {
	namespace tx_start {
		constexpr arch::field<uint16_t, bool> start(0, 1);
	} // namespace tx_start
} // namespace rtl8125

} // namespace flags
