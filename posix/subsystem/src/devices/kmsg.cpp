#include <arch/dma_pool.hpp>
#include <bitset>
#include <bragi/helpers-std.hpp>
#include <kerncfg.bragi.hpp>
#include <protocols/mbus/client.hpp>
#include <string.h>

#include "../common.hpp"
#include "kmsg.hpp"

namespace {

struct KmsgFile final : FileWithDefaults {
private:
	async::result<std::expected<size_t, Error>>
	readSome(Process *, void *data, size_t length, async::cancellation_token ct) override {
		// TODO(geert): Make this cancellable.
		(void)ct;
		std::vector<char> buffer(2048);

		uint32_t flags = managarm::kerncfg::GetBufferContentsFlags::ONE_RECORD;

		if(nonBlock_)
			flags |= managarm::kerncfg::GetBufferContentsFlags::NO_WAIT;

		managarm::kerncfg::GetBufferContentsRequest req;
		req.set_size(buffer.size());
		req.set_dequeue(offset_);
		req.set_flags(flags);

		auto [offer, sendReq, recvResp, recvBuffer] =
			co_await helix_ng::exchangeMsgs(lane_,
				helix_ng::offer(
					helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
					helix_ng::recvInline(),
					helix_ng::recvBuffer(buffer.data(), buffer.size())
				)
			);
		HEL_CHECK(offer.error());
		HEL_CHECK(sendReq.error());
		HEL_CHECK(recvResp.error());
		HEL_CHECK(recvBuffer.error());

		auto resp = *bragi::parse_head_only<managarm::kerncfg::SvrResponse>(recvResp);
		recvResp.reset();

		if(resp.error() == managarm::kerncfg::Error::WOULD_BLOCK)
			co_return std::unexpected{Error::wouldBlock};

		assert(resp.error() == managarm::kerncfg::Error::SUCCESS);

		int ret_len = snprintf(reinterpret_cast<char *>(data), length,
			"%s", reinterpret_cast<char *>(buffer.data()));

		if(ret_len < 0)
			co_return std::unexpected{Error::ioError};

		assert(offset_ == resp.effective_dequeue());
		offset_ = resp.new_dequeue();

		co_return static_cast<size_t>(ret_len);
	}

	async::result<frg::expected<Error, size_t>> writeAll(Process *, const void *data, size_t length) override {
		auto msg = reinterpret_cast<const char *>(data);

		HelLogSeverity s = kHelLogSeverityInfo;

		auto handlePrefix = [&](size_t digits) {
			assert(digits > 0 && digits <= 3);

			char *endptr = nullptr;
			auto val = std::strtoul(&msg[1], &endptr, 10);
			assert(endptr == &msg[1 + digits]);
			auto level = val & 0x7;

			if(level >= kHelLogSeverityEmergency && level <= kHelLogSeverityDebug) {
				s = HelLogSeverity(level);
			}

			msg += digits + 2;
			length -= digits + 2;
		};

		if(msg && length >= 1 && msg[0] == '<') {
			if(length >= 2 && isdigit(msg[1])) {
				if(length >= 3 && isdigit(msg[2])) {
					if(length >= 4 && isdigit(msg[3])) {
						if(length >= 5 && msg[4] == '>') {
							handlePrefix(3);
						}
					} else if(length >= 4 && msg[3] == '>') {
						handlePrefix(2);
					}
				} else if(length >= 3 && msg[2] == '>') {
					handlePrefix(1);
				}
			}
		}

		auto line_len = frg::min(length, strlen(msg));
		if (line_len && msg[line_len - 1] == '\n')
			line_len--;

		helLog(s, msg, line_len);

		co_return length;
	}

	async::result<frg::expected<Error, off_t>> seek(off_t offset, VfsSeek whence) override {
		if(whence == VfsSeek::relative)
			co_return Error::seekOnPipe;
		else if(whence == VfsSeek::absolute)
			if(!offset)
				offset_ = 0;
			else
				co_return Error::illegalArguments;
		else if(whence == VfsSeek::eof)
			// TODO: Unimplemented!
			assert(whence == VfsSeek::eof);
		co_return offset_;
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return passthrough_;
	}

	void handleClose() override {
		cancelServe_.cancel();
		passthrough_ = {};
	}

	helix::UniqueLane passthrough_;
	async::cancellation_event cancelServe_;
	helix::UniqueLane lane_;

	size_t offset_ = 0;
	bool nonBlock_;

public:
	static void serve(smarter::shared_ptr<KmsgFile> file) {
		helix::UniqueLane lane;
		std::tie(lane, file->passthrough_) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &fileOperations, file->cancelServe_));
	}

	KmsgFile(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link, helix::UniqueLane lane, bool nonblock)
	: FileWithDefaults{FileKind::unknown,  StructName::get("kmsg-file"), std::move(mount), std::move(link)},
		lane_{std::move(lane)}, nonBlock_{nonblock} {}
};

struct KmsgDevice final : UnixDevice {
	KmsgDevice()
	: UnixDevice(VfsType::charDevice) {
		assignId({1, 11});
	}

	std::string nodePath() override {
		return "kmsg";
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags flags) override {
		bool nonblock = flags & semanticNonBlock;

		if(flags & ~(semanticNonBlock | semanticRead | semanticWrite)){
			std::cout << "\e[31mposix: /dev/kmsg open() received illegal arguments:"
					<< std::bitset<32>(flags)
					<< "\nOnly semanticNonBlock (0x1), semanticRead (0x2) and semanticWrite(0x4) are allowed.\e[39m"
					<< std::endl;
			co_return Error::illegalArguments;
		}

		auto filter = mbus_ng::Conjunction{{
			mbus_ng::EqualsFilter{"class", "kerncfg-byte-ring"},
			mbus_ng::EqualsFilter{"purpose", "kernel-log"},
		}};

		auto enumerator = mbus_ng::Instance::global().enumerate(filter);
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();
		assert(events.size() == 1);

		auto entity = co_await mbus_ng::Instance::global().getEntity(events[0].id);

		auto file = smarter::make_shared<KmsgFile>(std::move(mount), std::move(link),
			(co_await entity.getRemoteLane()).unwrap(), nonblock);
		file->setupWeakFile(file);
		KmsgFile::serve(file);
		co_return File::constructHandle(std::move(file));
	}
};

} // anonymous namespace

std::shared_ptr<UnixDevice> createKmsgDevice() {
	return std::make_shared<KmsgDevice>();
}
