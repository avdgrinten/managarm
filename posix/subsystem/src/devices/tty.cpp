#include "../process.hpp"
#include "tty.hpp"

namespace {

struct TtyDevice final : UnixDevice {
	TtyDevice()
	: UnixDevice(VfsType::charDevice) {
		assignId({5, 0});
	}

	std::string nodePath() override {
		return "tty";
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *process, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags flags) override {
		if (!process)
			co_return Error::noBackingDevice;
		auto cts = process->pgPointer()->getSession()->getControllingTerminal();
		if (!cts)
			co_return Error::noBackingDevice;

		auto controllingTerminal = cts->controllingTerminal_.lock();
		if (!controllingTerminal)
			co_return Error::noBackingDevice;

		co_return co_await openDevice(process, VfsType::charDevice, controllingTerminal->getId(), mount, link, flags);
	}
};

} // anonymous namespace

std::shared_ptr<UnixDevice> createTtyDevice() {
	return std::make_shared<TtyDevice>();
}
