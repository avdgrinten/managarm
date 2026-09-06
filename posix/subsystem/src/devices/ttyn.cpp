#include <asm/ioctls.h>
#include <string.h>
#include <sys/epoll.h>

#include "../common.hpp"
#include "ttyn.hpp"
#include "../process.hpp"
#include <bragi/helpers-std.hpp>
#include <core/dispatch.hpp>
#include <core/tty.hpp>

#include <linux/vt.h>

#include <termios.h>

#include <bitset>

int activeVt = 1;
int nextVt = 2;

namespace {
	constexpr bool logVTRequests = false;
}

namespace {

struct TTYNFile final : FileWithDefaults {
private:
	async::result<std::expected<size_t, Error>>	
	readSome(Process *, void *data, size_t length, async::cancellation_token) override {
		memset(data, 0, length);
		co_return length;
	}

	async::result<frg::expected<Error, size_t>> writeAll(Process *, const void *, size_t length) override {
		co_return length;
	}

	async::result<frg::expected<Error, off_t>> seek(off_t, VfsSeek) override {
		co_return 0;
	}

	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *, uint64_t sequence, int mask,
			async::cancellation_token cancellation) override {
		(void)mask;

		if(sequence > 1)
			co_return Error::illegalArguments;

		if(sequence)
			co_await async::suspend_indefinitely(cancellation);
		co_return PollWaitResult{1, EPOLLOUT};
	}

	async::result<frg::expected<Error, PollStatusResult>>
	pollStatus(Process *) override {
		co_return PollStatusResult{1, EPOLLOUT};
	}

	struct HandleIoctl {
		async::result<std::expected<void, DispatchError>> operator()(
				managarm::fs::GenericIoctlRequest &&req, helix::BorrowedDescriptor conversation,
				bragi::preamble, TTYNFile *self) {
			if(req.command() == TIOCSCTTY) {
				auto [extractCreds] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::extractCredentials()
				);
				HEL_CHECK(extractCreds.error());

				managarm::fs::GenericIoctlReply resp;
				resp.set_error(managarm::fs::Errors::SUCCESS);

				auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
				HEL_CHECK(send_resp.error());
			}else if(req.command() == VT_OPENQRY) {
				auto [extractCreds] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::extractCredentials()
				);
				HEL_CHECK(extractCreds.error());

				if(nextVt == MAX_NR_CONSOLES)
					nextVt = -1;
				else
					nextVt++;

				managarm::fs::VtOpenqryResponse resp;
				resp.set_error(managarm::fs::Errors::SUCCESS);
				resp.set_vt_nr(nextVt);

				auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
				HEL_CHECK(send_resp.error());
			}else if(req.command() == VT_GETMODE) {
				auto [extractCreds] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::extractCredentials()
				);
				HEL_CHECK(extractCreds.error());

				managarm::fs::VtGetModeResponse resp;
				resp.set_error(managarm::fs::Errors::SUCCESS);
				resp.set_vt_mode(self->_mode);
				resp.set_vt_waitv(self->_waitv);
				resp.set_vt_relsig(self->_relsig);
				resp.set_vt_acqsig(self->_acqsig);
				resp.set_vt_frsig(self->_frsig);

				auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
				HEL_CHECK(send_resp.error());
			}else if(req.command() == VT_GETSTATE) {
				auto [extractCreds] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::extractCredentials()
				);
				HEL_CHECK(extractCreds.error());
				if(logVTRequests) {
					std::println("posix: VT_GETSTATE: Active VT is {}", activeVt);
				}

				managarm::fs::VtGetStateResponse resp;
				resp.set_error(managarm::fs::Errors::SUCCESS);
				resp.set_vt_nr(activeVt);
				resp.set_vt_state(1 << (activeVt - 1));
				if(self->_mode == VT_AUTO) {
					resp.set_vt_relsig(0);
				} else {
					resp.set_vt_relsig(self->_relsig);
				}

				auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
				HEL_CHECK(send_resp.error());
			}else if(req.command() == TCGETS) {
				struct termios attrs;

				memset(&attrs, 0, sizeof(struct termios));
				ttyCopyTermios(self->_activeSettings, attrs);

				managarm::fs::GenericIoctlReply resp;
				resp.set_error(managarm::fs::Errors::SUCCESS);

				auto [send_resp, send_attrs] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
					helix_ng::sendBuffer(&attrs, sizeof(struct termios))
				);
				HEL_CHECK(send_resp.error());
				HEL_CHECK(send_attrs.error());
			}else if(req.command() == TCSETS) {
				struct termios attrs;

				auto [recv_attrs] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(&attrs, sizeof(struct termios))
				);
				HEL_CHECK(recv_attrs.error());
				ttyCopyTermios(attrs, self->_activeSettings);

				managarm::fs::GenericIoctlReply resp;
				resp.set_error(managarm::fs::Errors::SUCCESS);

				auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
				HEL_CHECK(send_resp.error());
			}else{
				std::cout << "\e[31m" "posix: Rejecting unknown VT ioctl (commonIoctl) " << req.command()
						<< "\e[39m" << std::endl;
			}
			co_return {};
		}

		async::result<std::expected<void, DispatchError>> operator()(
				managarm::fs::VtSetModeRequest &&req, helix::BorrowedDescriptor conversation,
				bragi::preamble, TTYNFile *self) {
			auto [extractCreds] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::extractCredentials()
			);
			HEL_CHECK(extractCreds.error());

			auto creds = extractCreds.credentials();
			auto maybeProcess = findProcessWithCredentials(creds);
			if(!maybeProcess) {
				std::cout << "posix: VT_SETMODE with unknown process credentials" << std::endl;
				managarm::fs::VtSetModeResponse resp;
				resp.set_error(Error::badProcessCredentials | protocols::fs::toFsProtoError | protocols::fs::toFsError);
				auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
						helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
				HEL_CHECK(send_resp.error());
				co_return {};
			}
			self->_controllingProcess = *maybeProcess;

			if(logVTRequests) {
				std::println("posix: VT_SETMODE: \tmode: {}, waitv: {}, relsig: {}, acqsig: {}, frsig: {}",
					req.vt_mode(), req.vt_waitv(), req.vt_relsig(), req.vt_acqsig(), req.vt_frsig());
			}

			self->_mode = req.vt_mode();
			self->_waitv = req.vt_waitv();
			self->_relsig = req.vt_relsig();
			self->_acqsig = req.vt_acqsig();
			self->_frsig = req.vt_frsig();
			
			managarm::fs::VtSetModeResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			co_return {};
		}

		async::result<std::expected<void, DispatchError>> operator()(
				managarm::fs::VtActivateRequest &&req, helix::BorrowedDescriptor conversation,
				bragi::preamble, TTYNFile *self) {
			auto [extractCreds] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::extractCredentials()
			);
			HEL_CHECK(extractCreds.error());
			if(req.vt_mode() >= nextVt)
				std::println("\e[35mposix: Warning: Activating VT {} which has not been opened yet\e[39m", req.vt_mode());
			activeVt = req.vt_mode();
			if(logVTRequests) {
				std::println("posix: VT_ACTIVATE/VT_WAITACTIVE: Activated VT {}", req.vt_mode());
			}

			if(self->_acqsig != 0) {
				if(logVTRequests)
					std::println("\tSending signal {} when acquiring VT", self->_acqsig);
				// Issue signal
				if(auto proc = self->_controllingProcess.lock()) {
					proc->threadGroup()->issueThreadGroupSignal(self->_acqsig, {});
				}
			}

			managarm::fs::VtActivateResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			co_return {};
		}
	};

	async::result<void>
	ioctl(Process *, uint32_t, helix_ng::RecvInlineResult msg, helix::UniqueLane conversation) override {
		auto res = co_await dispatchRequest<
			managarm::fs::GenericIoctlRequest,
			managarm::fs::VtSetModeRequest,
			managarm::fs::VtActivateRequest
		>(conversation, std::move(msg), HandleIoctl{}, this);

		if (!res) {
			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
		}
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

	// vt_mode
	char _mode = 0;
	char _waitv = 0;
	short _relsig = 0;
	short _acqsig = 0;
	short _frsig = 0;
	std::weak_ptr<Process> _controllingProcess;

	struct termios _activeSettings;

public:
	static void serve(smarter::shared_ptr<TTYNFile> file) {
		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &fileOperations, file->_cancelServe));
	}

	TTYNFile(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link)
	: FileWithDefaults{FileKind::unknown,  StructName::get("tty1-file"), std::move(mount), std::move(link), File::defaultIsTerminal | File::defaultPipeLikeSeek} {
		memset(&_activeSettings, 0, sizeof(struct termios));
		// cflag: Linux also stores a baud rate here.
		// lflag: Linux additionally sets ECHOCTL, ECHOKE (which we do not have).
		_activeSettings.c_iflag = ICRNL | IXON;
		_activeSettings.c_oflag = OPOST | ONLCR;
		_activeSettings.c_cflag = CS8 | CREAD | HUPCL;
		_activeSettings.c_lflag = TTYDEF_LFLAG | ECHOK;
		_activeSettings.c_cc[VINTR] = CINTR;
		_activeSettings.c_cc[VEOF] = CEOF;
		_activeSettings.c_cc[VKILL] = CKILL;
		_activeSettings.c_cc[VSTART] = CSTART;
		_activeSettings.c_cc[VSTOP] = CSTOP;
		_activeSettings.c_cc[VSUSP] = CSUSP;
		_activeSettings.c_cc[VQUIT] = CQUIT;
		_activeSettings.c_cc[VERASE] = CERASE; // DEL character.
		_activeSettings.c_cc[VMIN] = CMIN;
		_activeSettings.c_cc[VDISCARD] = CDISCARD;
		_activeSettings.c_cc[VLNEXT] = CLNEXT;
		_activeSettings.c_cc[VWERASE] = CWERASE;
		_activeSettings.c_cc[VREPRINT] = CRPRNT;
		cfsetispeed(&_activeSettings, B38400);
		cfsetospeed(&_activeSettings, B38400);
	}
};

struct TTYNDevice final : UnixDevice {
	TTYNDevice(int n_)
	: UnixDevice(VfsType::charDevice) {
		n = n_;
		assignId({4, n_});
	}

	std::string nodePath() override {
		return "tty" + std::to_string(n);
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags semantic_flags) override {
		if(semantic_flags & ~(semanticNonBlock | semanticRead | semanticWrite)){
			std::cout << "\e[31mposix: TTYNFile open() received illegal arguments:"
				<< std::bitset<32>(semantic_flags)
				<< "\nOnly semanticNonBlock (0x1), semanticRead (0x2) and semanticWrite(0x4) are allowed.\e[39m"
				<< std::endl;
			co_return Error::illegalArguments;
		}

		auto file = smarter::make_shared<TTYNFile>(std::move(mount), std::move(link));
		file->setupWeakFile(file);
		TTYNFile::serve(file);
		co_return File::constructHandle(std::move(file));
	}
private:
	int n;
};

} // anonymous namespace

std::shared_ptr<UnixDevice> createTTYNDevice(int n) {
	return std::make_shared<TTYNDevice>(n);
}
