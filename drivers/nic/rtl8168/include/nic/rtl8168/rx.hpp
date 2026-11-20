#pragma once

#include <helix/memory.hpp>
#include <nic/rtl8168/common.hpp>
#include <nic/rtl8168/descriptor.hpp>

struct RealtekNic;

struct RxQueue {
	static async::result<std::unique_ptr<RxQueue>> create(RealtekNic &nic, size_t descriptors);
	RxQueue(arch::dma_array<Descriptor> descriptors, std::vector<arch::dma_buffer>);

	uintptr_t getBase() {
		return _descriptorIova;
	}

	void handleRxOk(RealtekNic &nic);
private:
	arch::dma_array<Descriptor> _descriptors;
	uintptr_t _descriptorIova;
	std::vector<arch::dma_buffer> _descriptor_buffers;
	QueueIndex _next_index;
	bool _loggedOversizedFrame = false;
};
