
#include <linux/magic.h>
#include <string.h>
#include <sys/sysmacros.h>

#include "fs.hpp"

namespace {

struct AnonymousSuperblock : FsSuperblock {
	AnonymousSuperblock() {
		deviceMinor_ = getUnnamedDeviceIdAllocator().allocate();
	}

	FutureMaybe<smarter::shared_ptr<FsNode>> createRegular(Process *) override {
		std::cout << "posix: createRegular on AnonymousSuperblock unsupported" << std::endl;
		co_return nullptr;
	}

	async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>>
	rename(FsLink *, FsLink *, std::string) override {
		co_return Error::noSuchFile;
	}

	async::result<frg::expected<Error, FsStats>> getFsStats() override {
		FsStats stats{
			.fsType = ANON_INODE_FS_MAGIC,
		};
		co_return stats;
	}

	std::string getFsType() override {
		assert(!"posix: getFsType on AnonymousSuperblock unsupported");
		return "";
	}

	dev_t deviceNumber() override {
		return makedev(0, deviceMinor_);
	}

private:
	unsigned int deviceMinor_;
};

} // namespace

FsSuperblock *getAnonymousSuperblock() {
	static AnonymousSuperblock sb{};
	return &sb;
}

id_allocator<unsigned int> &getUnnamedDeviceIdAllocator() {
	static id_allocator<unsigned int> unnamedDeviceIdAllocator{1};
	return unnamedDeviceIdAllocator;
}

// --------------------------------------------------------
// FsLink implementation.
// --------------------------------------------------------

async::result<frg::expected<Error>> FsLink::obstruct() {
	if(auto parent = getParentNode(); parent)
		assert(!parent->hasTraverseLinks() && "Node has traverseLinks but no obstruct?");
	co_return Error::illegalOperationTarget;
}

std::optional<std::string> FsLink::getProcFsDescription() {
	return std::nullopt;
}

// --------------------------------------------------------
// FsNode implementation.
// --------------------------------------------------------

async::result<frg::expected<Error, FileStats>> FsNode::getStats() {
	std::cout << "posix: getStats() is not implemented for this FsNode" << std::endl;
	co_return Error::illegalOperationTarget;
}

void FsNode::addObserver(std::shared_ptr<FsObserver> observer) {
	if(!(_defaultOps & defaultSupportsObservers))
		std::cout << "\e[31m" "posix: FsNode does not support observers" "\e[39m" << std::endl;

	// TODO: For increased efficiency, Observers could be stored in an intrusive list.
	auto borrowed = observer.get();
	auto [it, inserted] = _observers.insert({borrowed, std::move(observer)});
	(void)it;
	assert(inserted); // Registering observers twice is an error.
}

void FsNode::removeObserver(FsObserver *observer) {
	auto it = _observers.find(observer);
	assert(it != _observers.end());
	_observers.erase(it);
}

async::result<std::expected<smarter::shared_ptr<FsLink, LinkRc>, Error>>
FsNode::getLinkOrCreate(FsLink *, Process *, std::string, mode_t, bool) {
	std::println("posix: getLink() is not implemented for this FsNode");
	co_return std::unexpected{Error::illegalOperationTarget};
}

async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> FsNode::getLink(FsLink *, std::string) {
	std::cout << "posix: getLink() is not implemented for this FsNode" << std::endl;
	co_return Error::illegalOperationTarget;
}

async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> FsNode::link(FsLink *, std::string, smarter::shared_ptr<FsNode>) {
	std::cout << "posix: link() is not implemented for this FsNode" << std::endl;
	co_return Error::illegalOperationTarget;
}

async::result<std::variant<Error, smarter::shared_ptr<FsLink, LinkRc>>>
FsNode::mkdir(FsLink *, Process *, std::string, mode_t) {
	std::cout << "posix: mkdir() is not implemented for this FsNode" << std::endl;
	co_return Error::illegalOperationTarget;
}

async::result<std::variant<Error, smarter::shared_ptr<FsLink, LinkRc>>>
FsNode::symlink(FsLink *, std::string, std::string) {
	std::cout << "posix: symlink() is not implemented for this FsNode" << std::endl;
	co_return Error::illegalOperationTarget;
}

async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> FsNode::mkdev(FsLink *, std::string, VfsType, DeviceId) {
	std::cout << "posix: mkdev() is not implemented for this FsNode" << std::endl;
	co_return Error::illegalOperationTarget;
}

async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> FsNode::mkfifo(FsLink *, std::string, mode_t) {
	std::cout << "posix: mkfifo() is not implemented for this FsNode" << std::endl;
	co_return Error::illegalOperationTarget;
}

async::result<frg::expected<Error>> FsNode::unlink(std::string) {
	std::cout << "posix: unlink() is not implemented for this FsNode" << std::endl;
	co_return Error::illegalOperationTarget;

}

async::result<frg::expected<Error>> FsNode::rmdir(std::string) {
	std::cout << "posix: rmdir() is not implemented for this FsNode" << std::endl;
	co_return Error::illegalOperationTarget;
}

async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
FsNode::open(Process *, std::shared_ptr<MountView>, smarter::shared_ptr<FsLink, LinkRc>, SemanticFlags) {
	std::cout << "posix: open() is not implemented for this FsNode" << std::endl;
	co_return Error::illegalOperationTarget;
}

expected<std::string> FsNode::readSymlink(FsLink *, Process *) {
	co_return Error::illegalOperationTarget;
}

DeviceId FsNode::readDevice() {
	throw std::runtime_error("readDevice() is not implemented for this FsNode");
}

bool FsNode::hasTraverseLinks() {
	return false;
}

async::result<frg::expected<Error, std::pair<smarter::shared_ptr<FsLink, LinkRc>, size_t>>> FsNode::traverseLinks(FsLink *, std::deque<std::string>) {
	std::cout << "posix: traverseLinks() is not implemented for this FsNode" << std::endl;
	co_return Error::illegalOperationTarget;
}

async::result<Error> FsNode::chmod(int mode) {
	(void) mode;
	std::cout << "\e[31m" "posix: chmod() is not implemented for this FsNode" "\e[39m" << std::endl;
	co_return Error::accessDenied;
}

async::result<std::expected<void, Error>> FsNode::chown(std::optional<uid_t> uid, std::optional<gid_t> gid) {
	(void) uid;
	(void) gid;
	std::cout << "\e[31m" "posix: chown() is not implemented for this FsNode" "\e[39m" << std::endl;
	co_return std::unexpected{Error::insufficientPermissions};
}

async::result<Error> FsNode::utimensat(std::optional<timespec> atime, std::optional<timespec> mtime, timespec ctime) {
	(void) atime;
	(void) mtime;
	(void) ctime;

	std::cout << "\e[31m" "posix: utimensat() is not implemented for this FsNode" "\e[39m" << std::endl;
	co_return Error::accessDenied;
}

async::result<frg::expected<Error, smarter::shared_ptr<FsLink, LinkRc>>> FsNode::mksocket(FsLink *, std::string name, mode_t mode, uid_t uid, gid_t gid) {
	(void) name;
	(void) mode;
	(void) uid;
	(void) gid;

	std::cout << "\e[31m" "posix: mksocket() is not implemented for this FsNode" "\e[39m" << std::endl;
	co_return Error::illegalOperationTarget;
}

void FsNode::notifyObservers(uint32_t events, const std::string &name, uint32_t cookie, bool isDir) {
	for(const auto &[borrowed, observer] : _observers) {
		borrowed->observeNotification(events, name, cookie, isDir);
		(void)observer;
	}
}

