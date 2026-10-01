
#include <bragi/helpers-std.hpp>
#include <iostream>

#include "fs.bragi.hpp"
#include "protocols/fs/client.hpp"

namespace protocols {
namespace fs {

async::result<Error> synchronize(helix::BorrowedDescriptor lane, SynchronizeFlags flags) {
	managarm::fs::SynchronizeRequest req;
	if(flags == SynchronizeFlags::dataOnly)
		req.set_flags(managarm::fs::SynchronizeFlags::DATA_ONLY);
	else
		req.set_flags(0);

	auto [offer, sendReq, recvResp] = co_await helix_ng::exchangeMsgs(
		lane,
		helix_ng::offer(
			helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
			helix_ng::recvInline()
		)
	);
	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	// Servers that do not implement synchronization dismiss the request.
	if(recvResp.error() == kHelErrDismissed)
		co_return Error::notSupported;
	HEL_CHECK(recvResp.error());

	auto resp = *bragi::parse_head_only<managarm::fs::SynchronizeResponse>(recvResp);
	co_return resp.error() | toFsProtoError;
}

File::File(helix::UniqueDescriptor lane)
: _lane(std::move(lane)) {
	helCreateToken(&credsToken_);
}

async::result<void> File::seekAbsolute(int64_t offset) {
	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::SEEK_ABS);
	req.set_rel_offset(offset);

	auto ser = req.SerializeAsString();
	uint8_t buffer[128];

	auto [offer, send_req, recv_resp] =
		co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::recvBuffer(buffer, 128)
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::fs::Errors::SUCCESS);
}

async::result<int64_t> File::seekRelative(int64_t offset) {
	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::SEEK_REL);
	req.set_rel_offset(offset);

	auto ser = req.SerializeAsString();

	auto [offer, send_req, recv_resp] =
		co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());

	auto resp = *bragi::parse_head_only<managarm::fs::SvrResponse>(recv_resp);
	assert(resp.error() == managarm::fs::Errors::SUCCESS);
	co_return resp.offset();
}

async::result<int64_t> File::seekEof(int64_t offset) {
	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::SEEK_EOF);
	req.set_rel_offset(offset);

	auto ser = req.SerializeAsString();

	auto [offer, send_req, recv_resp] =
		co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());

	auto resp = *bragi::parse_head_only<managarm::fs::SvrResponse>(recv_resp);
	assert(resp.error() == managarm::fs::Errors::SUCCESS);
	co_return resp.offset();
}

async::result<ReadResult>
File::readSome(void *data, size_t max_length, async::cancellation_token ct) {
	auto cancelId = cancellationId_++;

	managarm::fs::ReadRequest req;
	req.set_size(max_length);
	req.set_cancellation_id(cancelId);

	uint8_t buffer[128];
	managarm::fs::SvrResponse resp;
	size_t actualLength = 0;

	co_await async::race_and_cancel(
		async::lambda([&](auto) -> async::result<void> {
			auto [offer, send_req, imbue_creds, recv_resp, recv_data] =
				co_await helix_ng::exchangeMsgs(
					_lane,
					helix_ng::offer(
						helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
						helix_ng::imbueCredentials(credsToken_),
						helix_ng::recvBuffer(buffer, 128),
						helix_ng::recvBuffer(data, max_length)
					)
				);

			HEL_CHECK(offer.error());
			HEL_CHECK(send_req.error());
			HEL_CHECK(imbue_creds.error());
			HEL_CHECK(recv_resp.error());
			HEL_CHECK(recv_data.error());

			resp.ParseFromArray(buffer, recv_resp.actualLength());
			actualLength = recv_data.actualLength();
		}),
		async::lambda([&, ct, this](auto c) -> async::result<void> {
			co_await async::suspend_indefinitely(c, ct);

			if (!ct.is_cancellation_requested())
				co_return;

			managarm::fs::CancelOperation req;
			req.set_cancellation_id(cancelId);

			auto [offer, send_req, imbue_creds] =
			co_await helix_ng::exchangeMsgs(
				_lane,
				helix_ng::offer(
					helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
					helix_ng::imbueCredentials(credsToken_)
				)
			);

			HEL_CHECK(offer.error());
			HEL_CHECK(send_req.error());
		})
	);

	if (resp.error() == managarm::fs::Errors::INTERRUPTED)
		co_return std::unexpected{Error::interrupted};

	if (resp.error() == managarm::fs::Errors::END_OF_FILE || (max_length && actualLength == 0))
		co_return std::unexpected{Error::endOfFile};

	assert(resp.error() == managarm::fs::Errors::SUCCESS);
	co_return actualLength;
}

async::result<size_t> File::writeSome(const void *data, size_t maxLength) {
	managarm::fs::WriteRequest req;
	req.set_size(maxLength);

	auto [offer, sendReq, imbueCreds, sendData, recvResp] =
		co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::imbueCredentials(),
				helix_ng::sendBuffer(data, maxLength),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(imbueCreds.error());
	HEL_CHECK(sendData.error());
	HEL_CHECK(recvResp.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(recvResp.data(), recvResp.length());
	recvResp.reset();
	if(resp.error() == managarm::fs::Errors::END_OF_FILE)
		co_return 0;
	assert(resp.error() == managarm::fs::Errors::SUCCESS);
	co_return resp.size();
}

async::result<frg::expected<Error, PollWaitResult>> File::pollWait(uint64_t sequence, int mask,
		async::cancellation_token ct) {
	auto cancelId = cancellationId_++;

	managarm::fs::FilePollRequest req;
	req.set_sequence(sequence);
	req.set_event_mask(mask);
	req.set_cancellation_id(cancelId);

	managarm::fs::FilePollResponse resp;

	co_await async::race_and_cancel(
		async::lambda([&](auto) -> async::result<void> {
			auto [offer, send_req, imbue_creds, recv_resp] = co_await helix_ng::exchangeMsgs(
				_lane,
				helix_ng::offer(
					helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
					helix_ng::imbueCredentials(credsToken_),
					helix_ng::recvInline()
				)
			);

			HEL_CHECK(offer.error());
			HEL_CHECK(send_req.error());
			HEL_CHECK(imbue_creds.error());
			HEL_CHECK(recv_resp.error());

			resp.ParseFromArray(recv_resp.data(), recv_resp.size());
		}),
		async::lambda([&, ct, this](auto c) -> async::result<void> {
			co_await async::suspend_indefinitely(c, ct);

			if (!ct.is_cancellation_requested())
				co_return;

			managarm::fs::CancelOperation req;
			req.set_cancellation_id(cancelId);

			auto [offer, send_req, imbue_creds] =
			co_await helix_ng::exchangeMsgs(
				_lane,
				helix_ng::offer(
					helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
					helix_ng::imbueCredentials(credsToken_)
				)
			);

			HEL_CHECK(offer.error());
			HEL_CHECK(send_req.error());
		})
	);

	if(resp.error() != managarm::fs::Errors::SUCCESS)
		co_return static_cast<Error>(resp.error());
	co_return PollWaitResult(resp.sequence(), resp.edges());
}

async::result<frg::expected<Error, PollStatusResult>> File::pollStatus() {
	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::FILE_POLL_STATUS);

	auto ser = req.SerializeAsString();
	uint8_t buffer[128];

	auto [offer, send_req, recv_resp] =
		co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::recvBuffer(buffer, 128)
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());

	if(resp.error() != managarm::fs::Errors::SUCCESS)
		co_return static_cast<Error>(resp.error());
	co_return PollStatusResult(resp.sequence(), resp.status());
}

async::result<helix::UniqueDescriptor> File::accessMemory() {
	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::MMAP);

	auto ser = req.SerializeAsString();
	uint8_t buffer[128];

	auto [offer, send_req, recv_resp, recv_memory] =
		co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::recvBuffer(buffer, 128),
				// We do not require any RWX bits at the protocol level.
				// The caller needs to know or determine the RWX bits (e.g., before mapping the memory view).
				helix_ng::pullDescriptor(kHelRightAssign)
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(recv_memory.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::fs::Errors::SUCCESS);
	co_return recv_memory.descriptor();
}

async::result<frg::expected<Error, File>> File::createSocket(helix::BorrowedLane lane,
		int domain, int type, int proto, int flags) {
	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::CREATE_SOCKET);
	req.set_domain(domain);
	req.set_type(type);
	req.set_protocol(proto);
	req.set_flags(flags);

	auto [offer, send_req, recv_resp, pull_ctrl, pull_pt] = co_await helix_ng::exchangeMsgs(
		lane,
		helix_ng::offer(
			helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
			helix_ng::recvInline(),
			helix_ng::pullDescriptor(kHelRightInvoke | kHelRightManage),
			helix_ng::pullDescriptor(kHelRightInvoke | kHelRightManage)
		)
	);
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	recv_resp.reset();
	if(resp.error() != managarm::fs::Errors::SUCCESS)
		co_return static_cast<Error>(resp.error());

	HEL_CHECK(pull_ctrl.error());
	HEL_CHECK(pull_pt.error());

	File file{pull_pt.descriptor()};
	file.ctrlLane_ = pull_ctrl.descriptor();
	co_return std::move(file);
}

async::result<Error> File::connect(const struct sockaddr *addr_ptr, socklen_t addr_length) {
	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::PT_CONNECT);

	auto [offer, send_req, imbue_creds, send_addr, recv_resp] = co_await helix_ng::exchangeMsgs(
	    _lane,
	    helix_ng::offer(
	        helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
	        helix_ng::imbueCredentials(),
	        helix_ng::sendBuffer(const_cast<struct sockaddr *>(addr_ptr), addr_length),
	        helix_ng::recvInline()
	    )
	);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(imbue_creds.error());
	HEL_CHECK(send_addr.error());
	HEL_CHECK(recv_resp.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	recv_resp.reset();

	co_return static_cast<Error>(resp.error());
}

async::result<frg::expected<Error, size_t>>
File::sendto(const void *buf, size_t len, int flags, const struct sockaddr *addr_ptr, socklen_t addr_length) {
	managarm::fs::SendMsgRequest req;
	req.set_flags(flags);
	req.set_size(len);
	req.set_has_cmsg_creds(false);
	req.set_has_cmsg_rights(false);

	auto [offer, send_head, send_tail, send_data, imbue_creds, send_addr, recv_resp] =
	    co_await helix_ng::exchangeMsgs(
	        _lane,
	        helix_ng::offer(
	            helix_ng::sendBragiHeadTail(req, frg::stl_allocator{}),
	            helix_ng::sendBuffer(buf, len),
	            helix_ng::imbueCredentials(),
	            helix_ng::sendBuffer(addr_ptr, addr_length),
	            helix_ng::recvInline()
	        )
	    );

	HEL_CHECK(offer.error());
	HEL_CHECK(send_head.error());
	HEL_CHECK(send_tail.error());
	HEL_CHECK(send_data.error());
	HEL_CHECK(imbue_creds.error());
	HEL_CHECK(send_addr.error());
	HEL_CHECK(recv_resp.error());

	managarm::fs::SendMsgReply resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	recv_resp.reset();

	if(resp.error() != managarm::fs::Errors::SUCCESS)
		co_return static_cast<Error>(resp.error());

	co_return resp.size();
}

async::result<frg::expected<Error, size_t>>
File::recvfrom(void *buf, size_t len, int flags, struct sockaddr *addr_ptr, socklen_t addr_length) {
	managarm::fs::RecvMsgRequest req;
	req.set_flags(flags);
	req.set_size(len);
	req.set_addr_size(addr_length);
	req.set_ctrl_size(0);

	auto [offer, send_req, imbue_creds, recv_resp, recv_addr, recv_data, recv_ctrl] =
	    co_await helix_ng::exchangeMsgs(
	        _lane,
	        helix_ng::offer(
	            helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
	            helix_ng::imbueCredentials(),
	            helix_ng::recvInline(),
	            helix_ng::recvBuffer(reinterpret_cast<void *>(addr_ptr), addr_length),
	            helix_ng::recvBuffer(buf, len),
	            helix_ng::recvBuffer(nullptr, 0)
	        )
	    );

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(imbue_creds.error());
	HEL_CHECK(recv_resp.error());

	managarm::fs::RecvMsgReply resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	recv_resp.reset();

	if(resp.error() != managarm::fs::Errors::SUCCESS)
		co_return static_cast<Error>(resp.error());

	co_return resp.ret_val();
}

} } // namespace protocol::fs
