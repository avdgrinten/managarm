#pragma once

#include <async/counting-semaphore.hpp>
#include <helix/memory.hpp>
#include <nic/rtl8168/common.hpp>
#include <nic/rtl8168/descriptor.hpp>
#include <queue>
#include <vector>

struct RealtekNic;

struct TxQueue {
	static async::result<std::unique_ptr<TxQueue>> create(RealtekNic &nic, size_t descriptorCount);
	TxQueue(arch::dma_array<Descriptor> descriptors, std::vector<arch::dma_buffer> descriptorBuffers);

	void handleTxOk();

	uintptr_t getBase() {
		return _descriptorIova;
	}

	async::result<void> submitDescriptor(arch::dma_buffer_view frame, RealtekNic &nic);
	async::result<void> postDescriptor(arch::dma_buffer_view frame, RealtekNic &nic, std::shared_ptr<Request> req);

	bool bufferEmpty() {
		return _amount_free_descriptors == _descriptors.size();
	}
protected:
	std::vector<arch::dma_buffer> _descriptor_buffers;
	std::queue<std::shared_ptr<Request>> _requests;
	arch::dma_array<Descriptor> _descriptors;
	uintptr_t _descriptorIova;
	size_t _amount_free_descriptors;
	async::counting_semaphore _freeDescriptors;
	QueueIndex tx_index;    // Our index into the TX buffer
	QueueIndex hw_tx_index; // The index into the TX buffer that the card currently has
};
