#include "extern_socket.hpp"

#include "fs.bragi.hpp"
#include "protocols/fs/client.hpp"

namespace {
struct Socket : File {
	Socket(helix::UniqueLane ctrlLane, helix::UniqueLane ptLane)
	: File{FileKind::unknown, StructName::get("extern-socket")},
		_control{std::move(ctrlLane)},
		_file{std::move(ptLane)} { }

	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *, uint64_t sequence, int mask,
			async::cancellation_token cancellation) override {
		auto resultOrError = co_await _file.pollWait(sequence, mask, cancellation);
		if(!resultOrError)
			co_return resultOrError.error() | toPosixError;
		co_return resultOrError.value();
	}

	async::result<frg::expected<Error, PollStatusResult>>
	pollStatus(Process *) override {
		auto resultOrError = co_await _file.pollStatus();
		assert(resultOrError);
		co_return resultOrError.value();
	}

	async::result<protocols::fs::Error> listen() override {
		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::PT_LISTEN);

		auto [offer, sendReq, recvResp] =
			co_await helix_ng::exchangeMsgs(
				getPassthroughLane(),
				helix_ng::offer(
					helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
					helix_ng::recvInline()
				)
			);

		HEL_CHECK(offer.error());
		HEL_CHECK(sendReq.error());
		HEL_CHECK(recvResp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recvResp.data(), recvResp.length());

		co_return resp.error() | protocols::fs::toFsProtoError;
	}

	async::result<frg::expected<Error, AcceptResult>> accept(Process *) override {
		managarm::fs::AcceptRequest req;

		auto [offer, sendReq, recvResp, pullCtrl, pullPt] = co_await helix_ng::exchangeMsgs(
			_file.getLane(),
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline(),
				helix_ng::pullDescriptor(kHelRightInvoke | kHelRightManage),
				helix_ng::pullDescriptor(kHelRightInvoke | kHelRightManage)
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(sendReq.error());
		HEL_CHECK(recvResp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recvResp.data(), recvResp.length());
		recvResp.reset();

		if (resp.error() != managarm::fs::Errors::SUCCESS)
			co_return resp.error() | toPosixError;
		HEL_CHECK(pullCtrl.error());
		HEL_CHECK(pullPt.error());

		auto file = smarter::make_shared<Socket>(pullCtrl.descriptor(), pullPt.descriptor());
		file->setupWeakFile(file);
		co_return File::constructHandle(file);
	}

	void handleClose() override {
		// Close the control lane to inform the server that we closed the file.
		_control = helix::UniqueLane{};
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _file.getLane();
	}

private:
	helix::UniqueLane _control;
	protocols::fs::File _file;
};
}

namespace extern_socket {

async::result<std::expected<smarter::shared_ptr<File, FileHandle>, Error>> createSocket(helix::BorrowedLane lane,
		int domain, int type, int proto, int flags) {
	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::CREATE_SOCKET);
	req.set_domain(domain);
	req.set_type(type);
	req.set_protocol(proto);
	req.set_flags(flags);

	auto [offer, sendReq, recvResp, pullCtrl, pullPt] = co_await helix_ng::exchangeMsgs(
		lane,
		helix_ng::offer(
			helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
			helix_ng::recvInline(),
			helix_ng::pullDescriptor(kHelRightInvoke | kHelRightManage),
			helix_ng::pullDescriptor(kHelRightInvoke | kHelRightManage)
		)
	);
	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(recvResp.data(), recvResp.length());
	recvResp.reset();

	if (resp.error() != managarm::fs::Errors::SUCCESS)
		co_return std::unexpected{resp.error() | toPosixError};
	HEL_CHECK(pullCtrl.error());
	HEL_CHECK(pullPt.error());

	auto file = smarter::make_shared<Socket>(pullCtrl.descriptor(), pullPt.descriptor());
	file->setupWeakFile(file);
	co_return File::constructHandle(file);
}

} // namespace extern_socket
