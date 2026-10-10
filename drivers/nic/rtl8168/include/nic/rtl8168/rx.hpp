#pragma once

#include <helix/memory.hpp>
#include <nic/rtl8168/common.hpp>
#include <nic/rtl8168/descriptor.hpp>
#include <queue>

struct RealtekNic;

struct RxQueue {
	static async::result<std::unique_ptr<RxQueue>> create(RealtekNic &nic, size_t descriptors);
	RxQueue(arch::dma_array<Descriptor> descriptors, std::vector<arch::dma_buffer>);

	uintptr_t getBase() {
		return _descriptorIova;
	}

	void handleRxOk();
	bool checkOwnerOfNextDescriptor();
	async::result<size_t> submitDescriptor(arch::dma_buffer_view frame, RealtekNic &nic);
	async::result<void> postDescriptor(arch::dma_buffer_view frame, RealtekNic &nic, std::shared_ptr<Request> req);
private:
	std::queue<std::shared_ptr<Request>> _requests;
	arch::dma_array<Descriptor> _descriptors;
	uintptr_t _descriptorIova;
	std::vector<arch::dma_buffer> _descriptor_buffers;
	QueueIndex _last_rx_index;
	QueueIndex _next_index;
	bool _loggedOversizedFrame = false;
};
