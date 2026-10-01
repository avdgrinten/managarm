#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <async/result.hpp>
#include <async/cancellation.hpp>
#include <frg/expected.hpp>
#include <helix/ipc.hpp>
#include <protocols/fs/common.hpp>

namespace protocols {
namespace fs {

async::result<Error> synchronize(helix::BorrowedDescriptor lane,
		SynchronizeFlags flags = SynchronizeFlags::none);

namespace _detail {

struct File {
	File(helix::UniqueDescriptor lane);

	helix::BorrowedDescriptor getLane() {
		return _lane;
	}

	async::result<void> seekAbsolute(int64_t offset);
	async::result<int64_t> seekRelative(int64_t offset);
	async::result<int64_t> seekEof(int64_t offset);

	async::result<ReadResult> readSome(void *data, size_t max_length, async::cancellation_token);
	async::result<size_t> writeSome(const void *data, size_t max_length);

	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(uint64_t sequence, int mask, async::cancellation_token cancellation = {});

	async::result<frg::expected<Error, PollStatusResult>>
	pollStatus();

	async::result<helix::UniqueDescriptor> accessMemory();

	static async::result<frg::expected<Error, File>> createSocket(helix::BorrowedLane lane,
		int domain, int type, int proto, int flags);

	async::result<Error> connect(const struct sockaddr *addr_ptr, socklen_t addr_length);

	async::result<frg::expected<Error, size_t>>
	sendto(const void *buf, size_t len, int flags, const struct sockaddr *addr_ptr, socklen_t addr_length);

	async::result<frg::expected<Error, size_t>>
	recvfrom(void *buf, size_t len, int flags, struct sockaddr *addr_ptr, socklen_t addr_length);

private:
	helix::UniqueDescriptor _lane;
	// Sockets are served on a control and a passthrough lane; closing the former closes the socket.
	helix::UniqueDescriptor ctrlLane_;
	HelHandle credsToken_;
	uint64_t cancellationId_ = 1;
};

} // namespace _detail

using _detail::File;

} } // namespace protocols::fs
