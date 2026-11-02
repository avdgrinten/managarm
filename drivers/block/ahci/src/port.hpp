#pragma once

#include <arch/mem_space.hpp>
#include <arch/dma_structs.hpp>
#include <arch/dma_pool.hpp>
#include <async/recurring-event.hpp>
#include <async/result.hpp>
#include <async/queue.hpp>
#include <frg/std_compat.hpp>

#include <blockfs.hpp>

#include "spec.hpp"
#include "command.hpp"

class Controller;

class Port : public blockfs::BlockDevice {
public:
	Port(Controller *controller, int64_t parentId, int index, size_t numCommandSlots, bool staggeredSpinUp,
			arch::mem_space regs);

public:
	async::result<bool> init();
	async::result<bool> run();
	void handleIrq();
	void dumpState();
	void checkErrors();

	async::result<std::expected<void, blockfs::Error>> flush() override;
	async::result<size_t> getSize() override;

	int getIndex() const { return portIndex_; }

protected:
	async::result<std::expected<void, blockfs::Error>> issueRead(unsigned int, uint64_t sector, arch::dma_buffer_view view) override;
	async::result<std::expected<void, blockfs::Error>> issueWrite(unsigned int, uint64_t sector, arch::dma_buffer_view view) override;

private:
	async::result<size_t> findFreeSlot_();
	async::detached submitPendingLoop_();
	async::result<void> submitCommand_(Command *cmd);
	void start_();
	void stop_();

private:
	Controller *controller_;

	// Mapping is owned by Controller
	arch::mem_space regs_;

	async::queue<Command *, frg::stl_allocator> pendingCmdQueue_{frg::stl_allocator{}};

	std::array<Command *, limits::maxCmdSlots> submittedCmds_{};
	async::recurring_event freeSlotDoorbell_;

	uint64_t deviceSize_;
	size_t numCommandSlots_;
	size_t commandsInFlight_;
	int portIndex_;
	bool staggeredSpinUp_;
	bool writeCacheKnownDisabled_ = false;
	bool supportsFlush_ = false;
	bool supportsFlushExt_ = false;

	arch::dma_object<commandList> commandList_;
	arch::dma_array<commandTable> commandTables_;
	arch::dma_object<receivedFis> receivedFis_;
};
