#pragma once

#include <arch/dma_pool.hpp>
#include <arch/mem_space.hpp>
#include <async/result.hpp>
#include <helix/memory.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/mbus/client.hpp>

enum class InterruptMode {
	None,
	LegacyIrq,
	Msi,
	MsiX,
};

#include "queue.hpp"
#include "namespace.hpp"
#include "spec.hpp"

enum class ControllerType {
	PciExpress,
	FabricsTcp,
};

struct Controller {
	Controller(int64_t parentId, std::string location, ControllerType type) : parentId_{parentId}, location_{location}, type_{type} {}
	virtual ~Controller() = default;

	virtual async::detached run(mbus_ng::EntityId subsystem) = 0;

	virtual async::result<Command::Result> submitAdminCommand(std::unique_ptr<Command> cmd) = 0;
	virtual async::result<Command::Result> submitIoCommand(std::unique_ptr<Command> cmd) = 0;

	virtual async::result<void> ensureMapped(arch::dma_buffer_view) = 0;
	virtual std::optional<uintptr_t> prpAddressOf(arch::dma_buffer_view) = 0;

	inline int64_t getParentId() const {
		return parentId_;
	}

	inline int64_t getMbusId() const {
		assert(mbusEntity_);
		return mbusEntity_->id();
	}

	inline ControllerType getType() const {
		return type_;
	}

	async::result<void> scanNamespaces();

	async::result<Command::Result> identifyController(arch::dma_object_view<spec::IdentifyController> id);
	async::result<Command::Result> identifyNamespaceList(unsigned int nsid, arch::dma_buffer_view list);
	async::result<Command::Result> identifyNamespace(unsigned int nsid, arch::dma_object_view<spec::IdentifyNamespace> id);

	async::result<void> createNamespace(unsigned int nsid);

	spec::DataTransfer dataTransferPolicy() const {
		return preferredDataTransfer_;
	}

	arch::contiguous_pool &memoryPool() {
		return pool_;
	}

	arch::contiguous_pool &contiguousPool() {
		return contiguousPool_;
	}

protected:
	spec::DataTransfer preferredDataTransfer_ = spec::DataTransfer::PRP;

	int64_t parentId_;
	std::unique_ptr<mbus_ng::EntityManager> mbusEntity_;
	uint32_t version_;
	std::string location_;
	const ControllerType type_;

	arch::dma_realm dmaRealm_;
	arch::contiguous_pool pool_{&dmaRealm_, {.addressBits = 64, .allocateContigous = false}};
	// Queues are passed to the controller as a single PRP with kQueuePhysContig.
	arch::contiguous_pool contiguousPool_{&dmaRealm_, {.addressBits = 64, .allocateContigous = true}};

	std::string serial;
	std::string model;
	std::string fw_rev;

	std::vector<std::unique_ptr<Queue>> activeQueues_;
	std::vector<std::unique_ptr<Namespace>> activeNamespaces_;
};

struct PciExpressController final : public Controller {
	PciExpressController(
	    int64_t parentId,
	    protocols::hw::Device hwDevice,
	    std::string location,
	    helix::Mapping regsMapping,
	    helix::UniqueDescriptor ioSpace,
		bool iommuActive
	);

	async::detached run(mbus_ng::EntityId subsystem) override;

	async::result<Command::Result> submitAdminCommand(std::unique_ptr<Command> cmd) override;
	async::result<Command::Result> submitIoCommand(std::unique_ptr<Command> cmd) override;

	async::result<void> ensureMapped(arch::dma_buffer_view) override;
	std::optional<uintptr_t> prpAddressOf(arch::dma_buffer_view) override;

private:
	async::result<void> setupIOQueueInterrupts(size_t queueId, size_t vector);

	static constexpr int IO_QUEUE_DEPTH = 1024;

	protocols::hw::Device hwDevice_;
	std::string location_;
	helix::Mapping regsMapping_;
	arch::mem_space regs_;

	helix::UniqueDescriptor ioSpace_;
	arch::dma_space dmaSpace_;

	unsigned int queueDepth_;
	uint32_t dbStride_;

	uint64_t irqSequence_;
	InterruptMode irqMode_;

	async::result<void> reset();

	async::result<void> waitStatus(bool enabled);
	async::result<void> enable();
	async::result<void> disable();

	async::result<Command::Result> requestIoQueues(uint16_t sqs, uint16_t cqs);
	async::result<bool> setupIoQueue(PciExpressQueue *q);
	async::result<Command::Result> createCQ(PciExpressQueue *q);
	async::result<Command::Result> createSQ(PciExpressQueue *q);

	async::detached handleIrqs(helix::UniqueDescriptor irq);
	async::detached handleMsis(helix::UniqueDescriptor irq, size_t queueId, bool isMsiX);
};
