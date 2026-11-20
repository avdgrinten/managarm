/* This file is derived from fuchsia's `fuchsia.c` and has been adapted for use in managarm.
 * The license is preserved below. For the original file, look here:
 * https://fuchsia.googlesource.com/fuchsia/+/a10b58da9f068289236785bd2f707142ca13798a/zircon/third_party/dev/ethernet/e1000/fuchsia.c
 */

/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2016 Nicole Graziano <nicole@nextbsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <nic/freebsd-e1000/common.hpp>

void E1000Nic::em_eth_rx_ack() {
	uint32_t n = _rxIndex();
	union e1000_rx_desc_extended* desc = (union e1000_rx_desc_extended*)&_rxd[n];

	/* Zero out the receive descriptors status. */
	desc->read.buffer_addr = rxdIova_[n];
	desc->wb.upper.status_error = 0;
}

void E1000Nic::em_rxd_setup() {
	union e1000_rx_desc_extended* rxd = (union e1000_rx_desc_extended*) &_rxd[0];

	for (size_t n = 0; n < RX_QUEUE_SIZE; n++) {
		rxd[n].read.buffer_addr = rxdIova_[n];
		/* DD bits must be cleared */
		rxd[n].wb.upper.status_error = 0;
	}
}

bool E1000Nic::eth_rx_pop() {
	size_t length;
	if(_hw.mac.type >= em_mac_min) {
		union e1000_rx_desc_extended* desc = (union e1000_rx_desc_extended*) &_rxd[_rxIndex];

		if(!(desc->wb.upper.status_error & E1000_RXD_STAT_DD)) {
			return false;
		}

		length = desc->wb.upper.length;
	} else {
		struct e1000_rx_desc* desc = &_rxd[_rxIndex];

		if(!(desc->status & E1000_RXD_STAT_DD)) {
			return false;
		}

		length = desc->length;
	}

	if(length <= sizeof(DescriptorSpace)) {
		feedPacket(_rxdbuf.object_view(_rxIndex()).view_buffer().subview(0, length));
	} else if(e1000_log_trace) {
		printf("e1000: dropping RX frame of %zu bytes\n", length);
	}

	if(_hw.mac.type >= em_mac_min) {
		em_eth_rx_ack();
	} else {
		_rxd[_rxIndex].status = 0;
	}

	E1000_WRITE_REG(&_hw, E1000_RDT(0), _rxIndex());
	++_rxIndex;

	return true;
}

#define EM_RADV 64
#define EM_RDTR 0

#define IGB_RX_PTHRESH \
  ((_hw.mac.type == e1000_i354) ? 12 : ((_hw.mac.type <= e1000_82576) ? 16 : 8))
#define IGB_RX_HTHRESH 8
#define IGB_RX_WTHRESH ((_hw.mac.type == e1000_82576) ? 1 : 4)
#define IGB_TX_PTHRESH ((_hw.mac.type == e1000_i354) ? 20 : 8)
#define IGB_TX_HTHRESH 1
#define IGB_TX_WTHRESH ((_hw.mac.type != e1000_82575) ? 1 : 16)

#define MAX_INTS_PER_SEC 8000
#define DEFAULT_ITR (1000000000 / (MAX_INTS_PER_SEC * 256))

async::result<void> E1000Nic::rxInit() {
	/*
	* Make sure receives are disabled while setting
	* up the descriptor ring
	*/
	u32 rctl = E1000_READ_REG(&_hw, E1000_RCTL);

	/* Do not disable if ever enabled on this hardware */
	if ((_hw.mac.type != e1000_82574) && (_hw.mac.type != e1000_82583)) {
		E1000_WRITE_REG(&_hw, E1000_RCTL, rctl & ~E1000_RCTL_EN);
	}

	/* Setup the Receive Control Register */
	rctl &= ~(3 << E1000_RCTL_MO_SHIFT);
	rctl |= E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_LBM_NO | E1000_RCTL_RDMTS_HALF | (_hw.mac.mc_filter_type << E1000_RCTL_MO_SHIFT);

	/* Do not store bad packets */
	rctl &= ~E1000_RCTL_SBP;

	/* Disable Long Packet receive */
	rctl &= ~E1000_RCTL_LPE;

	/* Strip the CRC */
	rctl |= E1000_RCTL_SECRC;

	if(_hw.mac.type >= e1000_82540) {
		E1000_WRITE_REG(&_hw, E1000_RADV, EM_RADV);
		/*
		 * Set the interrupt throttling rate. Value is calculated
		 * as DEFAULT_ITR = 1/(MAX_INTS_PER_SEC * 256ns)
		 */
		E1000_WRITE_REG(&_hw, E1000_ITR, DEFAULT_ITR);
	}

	E1000_WRITE_REG(&_hw, E1000_RDTR, EM_RDTR);

	/* Use extended rx descriptor formats */
	u32 rfctl = E1000_READ_REG(&_hw, E1000_RFCTL);
	rfctl |= E1000_RFCTL_EXTEN;

	/*
	* When using MSIX interrupts we need to throttle
	* using the EITR register (82574 only)
	*/
	if (_hw.mac.type == e1000_82574) {
		for (int i = 0; i < 4; i++) {
			E1000_WRITE_REG(&_hw, E1000_EITR_82574(i), DEFAULT_ITR);
		}
		/* Disable accelerated acknowledge */
		rfctl |= E1000_RFCTL_ACK_DIS;
	}

	E1000_WRITE_REG(&_hw, E1000_RFCTL, rfctl);
	u32 rxcsum = E1000_READ_REG(&_hw, E1000_RXCSUM);
	rxcsum &= ~E1000_RXCSUM_TUOFL;
	E1000_WRITE_REG(&_hw, E1000_RXCSUM, rxcsum);

	/*
	* XXX TEMPORARY WORKAROUND: on some systems with 82573
	* long latencies are observed, like Lenovo X60. This
	* change eliminates the problem, but since having positive
	* values in RDTR is a known source of problems on other
	* platforms another solution is being sought.
	*/
	if (_hw.mac.type == e1000_82573) {
		E1000_WRITE_REG(&_hw, E1000_RDTR, 0x20);
	}

	/* Setup the Base and Length of the Rx Descriptor Ring */
	co_await dmaSpace_.ensure_mapped(_rxd);
	u64 bus_addr = dmaSpace_.iova_of(_rxd);
	E1000_WRITE_REG(&_hw, E1000_RDLEN(0), RX_QUEUE_SIZE * sizeof(union e1000_rx_desc_extended));
	E1000_WRITE_REG(&_hw, E1000_RDBAH(0), (u32)(bus_addr >> 32));
	E1000_WRITE_REG(&_hw, E1000_RDBAL(0), (u32)bus_addr);

	/*
	 * Set PTHRESH for improved jumbo performance
	 * According to 10.2.5.11 of Intel 82574 Datasheet,
	 * RXDCTL(1) is written whenever RXDCTL(0) is written.
	 * Only write to RXDCTL(1) if there is a need for different
	 * settings.
	 */
	if (_hw.mac.type == e1000_82574) {
		u32 rxdctl = E1000_READ_REG(&_hw, E1000_RXDCTL(0));

		rxdctl |= 0x20;    /* PTHRESH */
		rxdctl |= 4 << 8;  /* HTHRESH */
		rxdctl |= 4 << 16; /* WTHRESH */
		rxdctl |= 1 << 24; /* Switch to granularity */

		E1000_WRITE_REG(&_hw, E1000_RXDCTL(0), rxdctl);
	} else if (_hw.mac.type >= igb_mac_min) {
		u32 srrctl = 2048 >> E1000_SRRCTL_BSIZEPKT_SHIFT;
		rctl |= E1000_RCTL_SZ_2048;

		/* Setup the Base and Length of the Rx Descriptor Rings */
		bus_addr = dmaSpace_.iova_of(_rxd);

		u32 rxdctl;
		srrctl |= E1000_SRRCTL_DESCTYPE_ADV_ONEBUF;
		E1000_WRITE_REG(&_hw, E1000_RDLEN(0), RX_QUEUE_SIZE * sizeof(struct e1000_rx_desc));
		E1000_WRITE_REG(&_hw, E1000_RDBAH(0), (uint32_t)(bus_addr >> 32));
		E1000_WRITE_REG(&_hw, E1000_RDBAL(0), (uint32_t)bus_addr);
		E1000_WRITE_REG(&_hw, E1000_SRRCTL(0), srrctl);

		/* Enable this Queue */
		rxdctl = E1000_READ_REG(&_hw, E1000_RXDCTL(0));
		rxdctl |= E1000_RXDCTL_QUEUE_ENABLE;
		rxdctl &= 0xFFF00000;
		rxdctl |= IGB_RX_PTHRESH;
		rxdctl |= IGB_RX_HTHRESH << 8;
		rxdctl |= IGB_RX_WTHRESH << 16;

		E1000_WRITE_REG(&_hw, E1000_RXDCTL(0), rxdctl);

		/* poll for enable completion */
		do {
			rxdctl = E1000_READ_REG(&_hw, E1000_RXDCTL(0));
		} while (!(rxdctl & E1000_RXDCTL_QUEUE_ENABLE));
	} else if (_hw.mac.type >= e1000_pch2lan) {
		e1000_lv_jumbo_workaround_ich8lan(&_hw, false);
	}

	/* Make sure VLAN Filters are off */
	rctl &= ~E1000_RCTL_VFE;

	if (_hw.mac.type < igb_mac_min) {
		rctl |= E1000_RCTL_SZ_2048;
		/* ensure we clear use DTYPE of 00 here */
		rctl &= ~0x00000C00;
	}

	/* Setup the Head and Tail Descriptor Pointers */
	E1000_WRITE_REG(&_hw, E1000_RDH(0), 0);
	E1000_WRITE_REG(&_hw, E1000_RDT(0), RX_QUEUE_SIZE - 1);

	/* Write out the settings */
	E1000_WRITE_REG(&_hw, E1000_RCTL, rctl);

	co_return;
}
