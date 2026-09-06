#include <string.h>

#include "../common.hpp"
#include "null.hpp"

#include <bitset>

namespace {

struct NullFile final : FileWithDefaults {
private:
	async::result<std::expected<size_t, Error>>
	readSome(Process *, void *, size_t, async::cancellation_token) override {
		co_return std::unexpected{Error::eof};
	}

	async::result<frg::expected<Error, size_t>> writeAll(Process *, const void *, size_t length) override {
		co_return length;
	}

	async::result<frg::expected<Error, off_t>> seek(off_t, VfsSeek) override {
		co_return 0;
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	void handleClose() override {
		_cancelServe.cancel();
		_passthrough = {};
	}

	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

public:
	static void serve(smarter::shared_ptr<NullFile> file) {
		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &fileOperations, file->_cancelServe));
	}

	NullFile(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link)
	: FileWithDefaults{FileKind::unknown,  StructName::get("null-file"), std::move(mount), std::move(link)} { }
};

struct NullDevice final : UnixDevice {
	NullDevice()
	: UnixDevice(VfsType::charDevice) {
		assignId({1, 3});
	}

	std::string nodePath() override {
		return "null";
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags semantic_flags) override {
		if(semantic_flags & ~(semanticRead | semanticWrite)){
			std::cout << "\e[31mposix: NullFile open() received illegal arguments:"
				<< std::bitset<32>(semantic_flags)
				<< "\nOnly semanticRead (0x2) and semanticWrite(0x4) are allowed.\e[39m"
				<< std::endl;
			co_return Error::illegalArguments;
		}

		auto file = smarter::make_shared<NullFile>(std::move(mount), std::move(link));
		file->setupWeakFile(file);
		NullFile::serve(file);
		co_return File::constructHandle(std::move(file));
	}
};

} // anonymous namespace

std::shared_ptr<UnixDevice> createNullDevice() {
	return std::make_shared<NullDevice>();
}
