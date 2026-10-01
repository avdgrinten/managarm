
#include <linux/if_link.h>
#include <memory>
#include <nic/rtl8168/common.hpp>
#include <nic/rtl8168/tx.hpp>
#include <nic/rtl8168/descriptor.hpp>
#include <nic/rtl8168/regs.hpp>
#include <nic/rtl8168/debug_options.hpp>

void RealtekNic::setTxConfigRegisters() {
	auto val = flags::transmit_config::mxdma(flags::transmit_config::mxdma_burst) |
		flags::transmit_config::ifg(flags::transmit_config::ifg_normal);

	if (_revision >= MacRevision::MacVer34 &&
		_revision != MacRevision::MacVer39) {
		val |= flags::transmit_config::auto_fifo(true);
	}

	_mmio.store(regs::transmit_config, val);
}

async::result<std::unique_ptr<TxQueue>> TxQueue::create(RealtekNic &nic, size_t descriptorCount) {
	arch::dma_array<Descriptor> descriptors{nic.dmaPool(), descriptorCount};
	std::vector<arch::dma_buffer> descriptorBuffers;

	for(size_t i = 0; i < descriptorCount; i++) {
		descriptorBuffers.emplace_back(nic.dmaPool(), 2048);
		memset(descriptorBuffers.back().data(), 0, 2048);

		co_await nic.dmaSpace().ensure_mapped(descriptorBuffers.back());
		uintptr_t addr = nic.dmaSpace().iova_of(descriptorBuffers.back());

		descriptors[i].flags = flags::tx::frame_length(0);
		descriptors[i].vlan = 0;
		descriptors[i].base_low = addr & 0xFFFF'FFFF;
		descriptors[i].base_high = (addr >> 32) & 0xFFFF'FFFF;
	}

	descriptors[descriptorCount - 1].flags |= flags::tx::eor(true);

	co_await nic.dmaSpace().ensure_mapped(descriptors);
	auto descriptorIova = nic.dmaSpace().iova_of(descriptors);
	auto tx = std::make_unique<TxQueue>(std::move(descriptors), std::move(descriptorBuffers));
	tx->_descriptorIova = descriptorIova;
	co_return tx;
}

TxQueue::TxQueue(
    arch::dma_array<Descriptor> descriptors, std::vector<arch::dma_buffer> descriptorBuffers
)
: _descriptor_buffers{std::move(descriptorBuffers)},
  _descriptors{std::move(descriptors)},
  _amount_free_descriptors{_descriptors.size()},
  tx_index{0, _descriptors.size()},
  hw_tx_index{0, _descriptors.size()} {
	_freeDescriptors.release(_descriptors.size());
}

async::result<void> TxQueue::submitDescriptor(arch::dma_buffer_view payload, RealtekNic &nic) {
	auto ev_req = std::make_shared<Request>(_descriptors.size());

	co_await postDescriptor(payload, nic, ev_req);
	co_await ev_req->event.wait();
}

// TODO: support large packets
async::result<void> TxQueue::postDescriptor(arch::dma_buffer_view payload, RealtekNic &nic, std::shared_ptr<Request> req) {
	co_await _freeDescriptors.async_acquire();
	assert(_amount_free_descriptors);

	_requests.push(req);

	auto desc = &_descriptors[tx_index()];

	// Manually pad the buffer
	// This bypasses issues on some cards.
	// TODO: gate this to only the cards which have these issues, and only do it if these issues would occur
	auto actual_size = payload.size() > 60 ? payload.size() : 60;

	if(actual_size != payload.size()) {
		memset(_descriptor_buffers[tx_index()].data(), 0, actual_size);
	}

	memcpy(_descriptor_buffers[tx_index()].data(), payload.data(), payload.size());

	storeFlags(*desc, flags::tx::eor(loadFlags(*desc) & flags::tx::eor) |
		flags::tx::first_segment(true) | flags::tx::last_segment(true) |
		flags::tx::frame_length(actual_size) | flags::tx::ownership(flags::tx::owner_nic));
	--_amount_free_descriptors;

	// TOOD: Technically, we should not be ringing the doorbell after every transmision, but only if there
	//       are no current transmissions in progress.
	//       Howver, always ringing the doorbell seems to work, and avoids the problem of having to detect
	//       stalls caused by not ringing the doorbell when it should have been rung
	nic.ringDoorbell();
	if(logTXDescriptor) {
		std::cout << "drivers/rtl8168: posting TX descriptor: "
			<< "tx_index: " << tx_index
			<< ", hw_tx_index: " << hw_tx_index
			<< ", amount_free_descriptors: " << _amount_free_descriptors
			<< std::endl;

		std::cout << "drivers/rtl8168: "
			<< "payload size: " << payload.size()
			<< ", actual_size: " << actual_size
			<< std::endl;
	}
	++tx_index;
	co_return;
}

// TODO: potential race conditions?
// TODO: locking the queue indexes might be a good idea
void TxQueue::handleTxOk() {
	while(hw_tx_index != tx_index) {
		auto i = hw_tx_index();
		auto descFlags = loadFlags(_descriptors[i]);
		if((descFlags & flags::tx::ownership) == flags::tx::owner_nic) // Descriptor was not transmitted?
			break;

		// Remove all info except EOR
		storeFlags(_descriptors[i], flags::tx::eor(descFlags & flags::tx::eor));
		_descriptors[i].vlan = 0;

		if(_requests.empty())
			break;

		auto req = _requests.front();

		req->event.raise();
		_requests.pop();
		++hw_tx_index;
		++_amount_free_descriptors;
		_freeDescriptors.release();
	}
	if(logIRQs) {
		std::cout << "drivers/rtl8168: completed handling of TX_OK"
			<< ", hw_tx_index: " << hw_tx_index()
			<< ", tx_index: " << tx_index()
			<< std::endl;
	}
}
