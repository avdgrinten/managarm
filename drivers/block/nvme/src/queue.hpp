#pragma once

#include <memory>

#include <arch/mem_space.hpp>
#include <async/recurring-event.hpp>
#include <async/queue.hpp>
#include <frg/std_compat.hpp>

#include "command.hpp"
#include "spec.hpp"

struct Queue {
	Queue(Controller *controller, unsigned int index, unsigned int depth)
	: controller_{controller},
	  qid_{index},
	  depth_{depth} {
		queuedCmds_.resize(depth);
	};

	virtual ~Queue() = default;

	virtual async::result<void> init() = 0;
	virtual async::detached run() = 0;

	virtual async::result<Command::Result> submitCommand(std::unique_ptr<Command> cmd) = 0;

	unsigned int getQueueId() const {
		return qid_;
	}
	unsigned int getQueueDepth() const {
		return depth_;
	}
	// Commands that the queue holds at once: a submission queue of N entries is full at N - 1 entries.
	unsigned int capacity() const {
		return depth_ - 1;
	}

	async::result<size_t> findFreeSlot();

protected:
	Controller *controller_;

	unsigned int qid_;
	unsigned int depth_;

	async::queue<std::unique_ptr<Command>, frg::stl_allocator> pendingCmdQueue_;
	std::vector<std::unique_ptr<Command>> queuedCmds_;
	async::recurring_event freeSlotDoorbell_;
	size_t commandsInFlight_ = 0;
};

struct PciExpressController;

struct PciExpressQueue final : Queue {
	PciExpressQueue(PciExpressController *controller, unsigned int index, unsigned int depth, arch::mem_space doorbells, size_t interruptVector = 0);

	async::result<void> init() override;
	async::detached run() override;

	arch::dma_buffer &getCq() {
		return cq_;
	}
	arch::dma_buffer &getSq() {
		return sq_;
	}

	size_t interruptVector() const {
		return interruptVector_;
	}

	int handleIrq();

private:
	arch::mem_space doorbells_;
	spec::CompletionEntry *cqes_;
	void *sqCmds_;

	arch::dma_buffer cq_;
	arch::dma_buffer sq_;

	uint16_t sqTail_;
	uint16_t cqHead_;
	uint8_t cqPhase_;
	size_t interruptVector_;

	async::detached submitPendingLoop();

	async::result<Command::Result> submitCommand(std::unique_ptr<Command> cmd) override;
	async::result<void> submitCommandToDevice(std::unique_ptr<Command> cmd);
};
