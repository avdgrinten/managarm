#pragma once

#include "file.hpp"

namespace signal_fd {

struct OpenFile : FileWithDefaults {
public:
	static void serve(smarter::shared_ptr<OpenFile> file);

	OpenFile(uint64_t mask, bool nonBlock);

	async::result<std::expected<size_t, Error>>
	readSome(Process *process, void *data, size_t maxLength, async::cancellation_token ct) override;

	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *process, uint64_t inSeq, int pollMask,
			async::cancellation_token cancellation) override;

	async::result<frg::expected<Error, PollStatusResult>>
	pollStatus(Process *process) override;

	void handleClose() override {
		cancelServe_.cancel();
		_passthrough = {};
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	uint64_t &mask() {
		return _mask;
	}

private:
	async::result<frg::expected<Error, PollWaitResult>>
	pollSignal_(std::shared_ptr<Process> process, uint64_t inSeq,
			async::cancellation_token cancellation);

	helix::UniqueLane _passthrough;
	async::cancellation_event cancelServe_;
	uint64_t _mask;
	std::atomic<bool> _nonBlock;
};

} // namespace signal_fd

smarter::shared_ptr<File, FileHandle> createSignalFile(uint64_t mask, bool nonBlock);
