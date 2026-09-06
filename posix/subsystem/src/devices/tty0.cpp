#include <sys/epoll.h>

#include "tty0.hpp"

namespace {

struct TTY0Device final : UnixDevice {
	TTY0Device()
	: UnixDevice(VfsType::charDevice) {
		assignId({4, 0});
	}

	std::string nodePath() override {
		return "tty0";
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *process, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags semantic_flags) override {
		co_return co_await openDevice(process, VfsType::charDevice, {4, 1}, mount, link, semantic_flags);
	}
};

} // anonymous namespace

std::shared_ptr<UnixDevice> createTTY0Device() {
	return std::make_shared<TTY0Device>();
}
