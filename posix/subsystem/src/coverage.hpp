#pragma once

#include <stddef.h>
#include <memory>

#include <async/result.hpp>
#include <helix/ipc.hpp>

struct Process;

void detachServeOnPcovThread(std::shared_ptr<Process> process, async::result<void> serveTree);

async::result<void> pcovStartRecording(Process *process, helix::BorrowedDescriptor buffer,
		size_t numWords, unsigned int mode);
async::result<void> pcovStopRecording(Process *process);
