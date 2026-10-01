#include <arch/bit.hpp>
#include <arch/barrier.hpp>
#include <helix/memory.hpp>
#include <unistd.h>

#include "command.hpp"
#include "controller.hpp"

async::result<void> Command::setupBuffer(Controller *controller, arch::dma_buffer_view view, spec::DataTransfer policy) {
	using arch::convert_endian;
	using arch::endian;

	view_ = view;

	if(policy == spec::DataTransfer::PRP) {
		static size_t pageSize = getpagesize();

		co_await controller->ensureMapped(view);

		// Offset of the first byte of the buffer relative to page base.
		auto startingOffset = reinterpret_cast<uintptr_t>(view.data()) % pageSize;
		// Offset into the buffer, starting from the beginning of the buffer.
		size_t offset = 0;

		// If the transfer crosses no more than a single page boundary
		if (startingOffset + view.size() <= pageSize * 2) {
			// Inline
			auto firstPrpLen = std::min(pageSize - startingOffset, view.size());
			command_.common.dataPtr.prp.prp1 = convert_endian<endian::little, endian::native>(
			    controller->prpAddressOf(view.subview(0, firstPrpLen)).value()
			);

			// If the transfer crosses a single page boundary, use the page base address.
			if (view.size() > firstPrpLen) {
				command_.common.dataPtr.prp.prp2 = convert_endian<endian::little, endian::native>(
				    controller->prpAddressOf(view.subview(firstPrpLen)).value()
				);
			} else {
				command_.common.dataPtr.prp.prp2 = 0;
			}

			co_return;
		}

		uintptr_t prp1, prp2;
		auto size = view.size();

		size_t prp1Len = std::min(pageSize - startingOffset, size);
		prp1 = controller->prpAddressOf(view.subview(0, prp1Len)).value();
		size -= prp1Len;
		offset += prp1Len;

		auto prpObj = arch::dma_array<uint64_t>{&controller->memoryPool(), pageSize >> 3};
		auto *prpList = prpObj.data();

		co_await controller->ensureMapped(prpObj.view_buffer());
		prp2 = controller->prpAddressOf(prpObj.view_buffer()).value();
		prpLists.push_back(std::move(prpObj));

		size_t i = 0;
		for (;;) {
			if (i == pageSize >> 3) {
				auto *oldPrpList = prpList;
				prpObj = arch::dma_array<uint64_t>{&controller->memoryPool(), pageSize >> 3};
				prpList = prpObj.data();

				co_await controller->ensureMapped(prpObj.view_buffer());
				prpList[0] = oldPrpList[i - 1];
				oldPrpList[i - 1] = convert_endian<endian::little, endian::native>(
					controller->prpAddressOf(prpObj.view_buffer()).value());
				i = 1;

				prpLists.push_back(std::move(prpObj));
			}
			prpList[i++] = convert_endian<endian::little, endian::native>(
			    controller->prpAddressOf(view.subview(offset, std::min(pageSize, size))).value()
			);
			offset += pageSize;

			if (size <= pageSize)
				break;
			size -= pageSize;
		}

		command_.common.dataPtr.prp.prp1 = convert_endian<endian::little, endian::native>(prp1);
		command_.common.dataPtr.prp.prp2 = convert_endian<endian::little, endian::native>(prp2);
	} else {
		// TODO(no92): this heavily assumes NVMe-over-fabrics; we might want to support SGL on
		// PCIe bindings, too. Conveniently, qemu's emulation supports the most basic tier of SGLs.
		command_.common.flags &= ~0xB0;
		command_.common.flags |= 0x40;
		command_.common.dataPtr.sgl.dataBlock.length = view.size();

		// Fabrics commands (i.e., Connect) always carry their data in the capsule. For other
		// writes, the transport decides based on the in-capsule data size of the queue.
		if(view.size() && command_.common.opcode == static_cast<uint8_t>(spec::AdminOpcode::Fabrics)) {
			command_.common.dataPtr.sgl.generic.sglDescriptorType = 0;
			command_.common.dataPtr.sgl.generic.sglSubType = 1;
		} else {
			command_.common.dataPtr.sgl.generic.sglDescriptorType = 0x05;
			command_.common.dataPtr.sgl.generic.sglSubType = 0x0A;
		}
	}
}
