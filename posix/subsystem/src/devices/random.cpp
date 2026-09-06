#include <string.h>
#include <sys/random.h>

#include "../common.hpp"
#include "random.hpp"

#include <bitset>

namespace {

struct RandomFile final : FileWithDefaults {
private:
	async::result<std::expected<size_t, Error>>
	readSome(Process *, void *data, size_t length, async::cancellation_token ct) override {
		auto p = reinterpret_cast<char *>(data);
		size_t n = 0;
		while(n < length) {
			if(ct.is_cancellation_requested()) {
				if(!n)
					co_return std::unexpected{Error::interrupted};

				break;
			}

			size_t chunk;
			HEL_CHECK(helGetRandomBytes(p + n, length - n, &chunk));
			n += chunk;
		}

		co_return n;
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

	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

public:
	static void serve(smarter::shared_ptr<RandomFile> file) {
		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &fileOperations, file->_cancelServe));
	}

	RandomFile(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link)
	: FileWithDefaults{FileKind::unknown,  StructName::get("random-file"), std::move(mount), std::move(link)} { }
};

struct RandomDevice final : UnixDevice {
	RandomDevice()
	: UnixDevice(VfsType::charDevice) {
		assignId({1, 8});
	}

	std::string nodePath() override {
		return "random";
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link,
			SemanticFlags semantic_flags) override {
		if(semantic_flags & ~(semanticRead | semanticWrite)){
			std::cout << "\e[31mposix: RandomFile open() received illegal arguments:"
				<< std::bitset<32>(semantic_flags)
				<< "\nOnly semanticRead (0x2) and semanticWrite(0x4) are allowed.\e[39m"
				<< std::endl;
			co_return Error::illegalArguments;
		}

		auto file = smarter::make_shared<RandomFile>(std::move(mount), std::move(link));
		file->setupWeakFile(file);
		RandomFile::serve(file);
		co_return File::constructHandle(std::move(file));
	}
};

} // anonymous namespace

std::shared_ptr<UnixDevice> createRandomDevice() {
	return std::make_shared<RandomDevice>();
}
