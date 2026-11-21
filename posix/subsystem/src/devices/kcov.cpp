#include <coroutine>
#include <mutex>
#include <print>

#include <bragi/helpers-std.hpp>
#include <core/dispatch.hpp>
#include <helix/dispatcher-pool.hpp>
#include <protocols/fs/common.hpp>

#include "../common.hpp"
#include "../coverage.hpp"
#include "../process.hpp"
#include "kcov.hpp"

namespace {

struct KcovFile final : FileWithDefaults {
private:
	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	void handleClose() override {
		_cancelServe.cancel();
		_passthrough = {};
	}

	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

	std::mutex _mutex;
	helix::UniqueDescriptor _buffer;
	size_t _numWords = 0;

	struct HandleIoctl {
		async::result<std::expected<void, DispatchError>> operator()(
				managarm::fs::KcovSetSizeRequest &&req, helix::BorrowedDescriptor conversation,
				bragi::preamble, KcovFile *self) {
			if(!co_await callerProcess(conversation)) {
				co_await sendResponse(conversation, badCredentialsError());
				co_return {};
			}

			if(req.entries() <= 0) {
				co_await sendResponse(conversation, managarm::fs::Errors::ILLEGAL_ARGUMENT);
				co_return {};
			}

			HelHandle handle;
			auto error = helCreateKcovBuffer(
					(static_cast<size_t>(req.entries()) + 1) * sizeof(uint64_t), &handle);
			if(error == kHelErrIllegalArgs) {
				co_await sendResponse(conversation, managarm::fs::Errors::ILLEGAL_ARGUMENT);
				co_return {};
			}
			if(error == kHelErrNoMemory) {
				co_await sendResponse(conversation, managarm::fs::Errors::NO_MEMORY);
				co_return {};
			}
			if(error == kHelErrUnsupportedOperation) {
				co_await sendResponse(conversation, managarm::fs::Errors::NOT_SUPPORTED);
				co_return {};
			}
			HEL_CHECK(error);
			helix::UniqueDescriptor buffer{handle};

			bool installed = false;
			{
				std::lock_guard lock{self->_mutex};
				if(!self->_buffer) {
					self->_buffer = std::move(buffer);
					self->_numWords = static_cast<size_t>(req.entries()) + 1;
					installed = true;
				}
			}

			co_await sendResponse(conversation, installed
					? managarm::fs::Errors::SUCCESS : managarm::fs::Errors::BUSY);
			co_return {};
		}

		async::result<std::expected<void, DispatchError>> operator()(
				managarm::fs::KcovStartRequest &&req, helix::BorrowedDescriptor conversation,
				bragi::preamble, KcovFile *self) {
			auto process = co_await callerProcess(conversation);
			if(!process) {
				co_await sendResponse(conversation, badCredentialsError());
				co_return {};
			}

			HelHandle buffer;
			size_t numWords;
			{
				std::lock_guard lock{self->_mutex};
				buffer = self->_buffer.getHandle();
				numWords = self->_numWords;
			}

			auto error = helEnableCoverage(process->threadDescriptor().getHandle(),
					buffer, req.mode());
			if(error == kHelErrNoDescriptor || error == kHelErrIllegalArgs
					|| error == kHelErrThreadTerminated) {
				co_await sendResponse(conversation, managarm::fs::Errors::ILLEGAL_ARGUMENT);
				co_return {};
			}
			if(error == kHelErrIllegalState) {
				co_await sendResponse(conversation, managarm::fs::Errors::BUSY);
				co_return {};
			}
			if(error == kHelErrUnsupportedOperation) {
				co_await sendResponse(conversation, managarm::fs::Errors::NOT_SUPPORTED);
				co_return {};
			}
			HEL_CHECK(error);

			co_await pcovStartRecording(process.get(), helix::BorrowedDescriptor{buffer},
					numWords, req.mode());

			co_await sendResponse(conversation, managarm::fs::Errors::SUCCESS);
			co_return {};
		}

		async::result<std::expected<void, DispatchError>> operator()(
				managarm::fs::KcovStopRequest &&, helix::BorrowedDescriptor conversation,
				bragi::preamble, KcovFile *) {
			auto process = co_await callerProcess(conversation);
			if(!process) {
				co_await sendResponse(conversation, badCredentialsError());
				co_return {};
			}

			co_await pcovStopRecording(process.get());

			auto error = helDisableCoverage(process->threadDescriptor().getHandle());
			if(error == kHelErrIllegalState) {
				co_await sendResponse(conversation, managarm::fs::Errors::ILLEGAL_ARGUMENT);
				co_return {};
			}
			if(error == kHelErrUnsupportedOperation) {
				co_await sendResponse(conversation, managarm::fs::Errors::NOT_SUPPORTED);
				co_return {};
			}
			HEL_CHECK(error);

			co_await sendResponse(conversation, managarm::fs::Errors::SUCCESS);
			co_return {};
		}

	private:
		static async::result<std::shared_ptr<Process>> callerProcess(
				helix::BorrowedDescriptor conversation) {
			auto [extractCreds] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::extractCredentials()
			);
			HEL_CHECK(extractCreds.error());

			auto maybeProcess = findProcessWithCredentials(extractCreds.credentials());
			if(!maybeProcess) {
				std::println("posix: kcov ioctl with unknown process credentials");
				co_return nullptr;
			}
			co_return *maybeProcess;
		}

		static managarm::fs::Errors badCredentialsError() {
			return Error::badProcessCredentials
					| protocols::fs::toFsProtoError | protocols::fs::toFsError;
		}

		static async::result<void> sendResponse(helix::BorrowedDescriptor conversation,
				managarm::fs::Errors error) {
			managarm::fs::KcovResponse resp;
			resp.set_error(error);
			auto [sendResp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(sendResp.error());
		}
	};

public:
	static void serve(smarter::shared_ptr<KcovFile> file) {
		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		helix::DispatcherPool::global().detach(protocols::fs::servePassthrough(std::move(lane),
				file, &fileOperations, file->_cancelServe));
	}

	KcovFile(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link)
	: FileWithDefaults{FileKind::unknown, StructName::get("kcov-file"),
			std::move(mount), std::move(link)} { }

	FutureMaybe<helix::UniqueDescriptor> accessMemory() override {
		std::lock_guard lock{_mutex};
		co_return _buffer.dup();
	}

	async::result<void> ioctl(Process *, uint32_t, helix_ng::RecvInlineResult msg,
			helix::UniqueLane conversation) override {
		auto res = co_await dispatchRequest<
			managarm::fs::KcovSetSizeRequest,
			managarm::fs::KcovStartRequest,
			managarm::fs::KcovStopRequest
		>(conversation, std::move(msg), HandleIoctl{}, this);

		if(!res) {
			auto [dismiss] = co_await helix_ng::exchangeMsgs(conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
		}
	}
};

struct KcovDevice final : UnixDevice {
	KcovDevice()
	: UnixDevice(VfsType::charDevice) {
		assignId({1, 254}); // This minor is not used by Linux.
	}

	std::string nodePath() override {
		return "kcov";
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *process, std::shared_ptr<MountView> mount,
			smarter::shared_ptr<FsLink, LinkRc> link, SemanticFlags semantic_flags) override {
		if(semantic_flags & ~(semanticRead | semanticWrite))
			co_return Error::illegalArguments;

		// From now on, threads created by this will use a special coverage thread.
		if(process)
			process->threadGroup()->setUsesKcov();

		auto file = smarter::make_shared<KcovFile>(std::move(mount), std::move(link));
		file->setupWeakFile(file);
		KcovFile::serve(file);
		co_return File::constructHandle(std::move(file));
	}
};

} // anonymous namespace

std::shared_ptr<UnixDevice> createKcovDevice() {
	return std::make_shared<KcovDevice>();
}
