#pragma once

#include <mutex>

#include <async/algorithm.hpp>
#include <core/clock.hpp>
#include <frg/scope_exit.hpp>
#include <helix/dispatcher-pool.hpp>
#include "fs.hpp"
#include "trace.hpp"


namespace blockfs {

template <FileSystem T>
async::result<protocols::fs::SeekResult> doSeekAbs(void *object, int64_t offset) {
	using File = typename T::File;
	auto self = static_cast<File *>(object);

	co_await self->mutex.async_lock();
	frg::unique_lock lock{frg::adopt_lock, self->mutex};

	self->offset = offset;

	co_return static_cast<ssize_t>(self->offset);
}

template <FileSystem T>
async::result<protocols::fs::SeekResult> doSeekRel(void *object, int64_t offset) {
	using File = typename T::File;
	auto self = static_cast<File *>(object);

	co_await self->mutex.async_lock();
	frg::unique_lock lock{frg::adopt_lock, self->mutex};

	self->offset += offset;

	co_return static_cast<ssize_t>(self->offset);
}

template <FileSystem T>
async::result<protocols::fs::SeekResult> doSeekEof(void *object, int64_t offset) {
	using File = typename T::File;
	using Inode = typename T::Inode;
	auto self = static_cast<File *>(object);
	auto inode = std::static_pointer_cast<Inode>(self->inode);

	co_await self->mutex.async_lock();
	frg::unique_lock lock{frg::adopt_lock, self->mutex};

	co_await inode->readyEvent.wait();

	co_await inode->inodeMutex.async_lock_shared();
	frg::shared_lock inodeLock{frg::adopt_lock, inode->inodeMutex};

	self->offset = offset + inode->fileSize();
	co_return static_cast<ssize_t>(self->offset);
}

template <FileSystem T>
async::result<protocols::fs::Error> doFlock(void *object, int flags) {
	using File = typename T::File;
	using Inode = typename T::Inode;

	auto self = static_cast<File *>(object);
	auto inode = std::static_pointer_cast<Inode>(self->inode);

	co_await inode->readyEvent.wait();

	auto result = co_await inode->flockManager.lock(&self->flock, flags);
	co_return result;
}

namespace detail {

template <Inode T>
async::result<protocols::fs::ReadResult> doReadImpl(T *inode, void *buffer, size_t length, auto &offset) {
	protocols::ostrace::Timer timer;
	size_t numBytes = 0;
	uint64_t timeReady = 0;
	uint64_t timeLock = 0;
	uint64_t timeCopy = 0;
	frg::scope_exit evtOnExit{[&] {
		ostContext.emit(
			ostEvtRead,
			ostAttrNumBytes(numBytes),
			ostAttrNumRequested(length),
			ostAttrTime(timer.elapsed()),
			ostAttrTimeReady(timeReady),
			ostAttrTimeLock(timeLock),
			ostAttrTimeCopy(timeCopy)
		);
	}};

	if (!length)
		co_return size_t{0};

	// TODO(geert): Pass cancellation token
	co_await inode->readyEvent.wait();
	timeReady = timer.elapsed();

	co_await inode->inodeMutex.async_lock_shared();
	frg::shared_lock inodeLock{frg::adopt_lock, inode->inodeMutex};
	timeLock = timer.elapsed() - timeReady;

	if (inode->fileType == FileType::kTypeDirectory)
		co_return std::unexpected{protocols::fs::Error::isDirectory};
	if (offset >= inode->fileSize())
		co_return std::unexpected{protocols::fs::Error::endOfFile};

	auto remaining = inode->fileSize() - offset;
	auto chunkSize = std::min(length, remaining);
	if (!chunkSize)
		co_return std::unexpected{protocols::fs::Error::endOfFile};

	auto chunkOffset = offset;
	offset += chunkSize;

	// TODO: Add a sendFromMemory action to exchangeMsgs to avoid
	// having to copy this data twice.
	protocols::ostrace::Timer copyTimer;
	auto readMemory = co_await helix_ng::readMemory(
		inode->accessMemory(),
		chunkOffset, chunkSize, buffer);
	HEL_CHECK(readMemory.error());
	timeCopy = copyTimer.elapsed();

	numBytes = chunkSize;
	co_return chunkSize;
}

template <Inode T>
async::result<frg::expected<protocols::fs::Error, size_t>>
doWriteImpl(T *inode, const void *buffer, size_t length, bool append, auto &offset) {
	protocols::ostrace::Timer timer;
	size_t numBytes = 0;
	uint64_t timeReady = 0;
	uint64_t timeLock = 0;
	uint64_t timeResize = 0;
	uint64_t timeCopy = 0;
	frg::scope_exit evtOnExit{[&] {
		ostContext.emit(
			ostEvtWrite,
			ostAttrNumBytes(numBytes),
			ostAttrNumRequested(length),
			ostAttrTime(timer.elapsed()),
			ostAttrTimeReady(timeReady),
			ostAttrTimeLock(timeLock),
			ostAttrTimeResize(timeResize),
			ostAttrTimeCopy(timeCopy)
		);
	}};

	if (!length)
		co_return size_t{0};

	co_await inode->readyEvent.wait();
	timeReady = timer.elapsed();

	co_await inode->inodeMutex.async_lock();
	frg::unique_lock inodeLock{frg::adopt_lock, inode->inodeMutex};
	timeLock = timer.elapsed() - timeReady;

	if (inode->fileType == FileType::kTypeDirectory)
		co_return protocols::fs::Error::isDirectory;

	if (append)
		offset = inode->fileSize();
	auto requiredSize = offset + length;
	if (requiredSize > inode->fileSize()) {
		protocols::ostrace::Timer resizeTimer;
		FRG_CO_TRY(co_await inode->resizeFile(requiredSize));
		timeResize = resizeTimer.elapsed();
	}

	// TODO: Add a recvToMemory action to exchangeMsgs to avoid
	// having to copy this data twice.
	protocols::ostrace::Timer copyTimer;
	auto writeMemory = co_await helix_ng::writeMemory(
		inode->accessMemory(),
		offset, length, buffer);
	HEL_CHECK(writeMemory.error());
	timeCopy = copyTimer.elapsed();

	offset += length;

	numBytes = length;
	co_return length;
}


} // namespace detail


template <FileSystem T>
async::result<protocols::fs::ReadResult> doRead(void *object, helix_ng::CredentialsView,
		void *buffer, size_t length, async::cancellation_token) {
	using File = typename T::File;
	using Inode = typename T::Inode;

	auto self = static_cast<File *>(object);
	auto inode = std::static_pointer_cast<Inode>(self->inode);

	co_await self->mutex.async_lock();
	frg::unique_lock lock{frg::adopt_lock, self->mutex};

	co_return co_await detail::doReadImpl(inode.get(), buffer, length, self->offset);
}


template <FileSystem T>
async::result<protocols::fs::ReadResult> doPread(void *object, int64_t offset, helix_ng::CredentialsView,
		void *buffer, size_t length) {
	using File = typename T::File;
	using Inode = typename T::Inode;

	if (offset < 0)
		co_return std::unexpected{protocols::fs::Error::illegalArguments};
	size_t unsignedOffset = offset;

	auto self = static_cast<File *>(object);
	auto inode = std::static_pointer_cast<Inode>(self->inode);

	co_await self->mutex.async_lock_shared();
	frg::shared_lock lock{frg::adopt_lock, self->mutex};

	co_return co_await detail::doReadImpl(inode.get(), buffer, length, unsignedOffset);
}


template <FileSystem T>
async::result<frg::expected<protocols::fs::Error, size_t>> doWrite(void *object, helix_ng::CredentialsView,
		const void *buffer, size_t length) {
	using File = typename T::File;
	using Inode = typename T::Inode;

	auto self = static_cast<File *>(object);
	auto inode = std::static_pointer_cast<Inode>(self->inode);

	if (!self->write)
		co_return protocols::fs::Error::badFileDescriptor;

	co_await self->mutex.async_lock();
	frg::unique_lock lock{frg::adopt_lock, self->mutex};

	co_return co_await detail::doWriteImpl(inode.get(), buffer, length, self->append, self->offset);
}

template <FileSystem T>
async::result<frg::expected<protocols::fs::Error, size_t>> doPwrite(void *object, int64_t offset, helix_ng::CredentialsView,
		const void *buffer, size_t length) {
	using File = typename T::File;
	using Inode = typename T::Inode;

	if (offset < 0)
		co_return protocols::fs::Error::illegalArguments;
	size_t unsignedOffset = offset;

	auto self = static_cast<File *>(object);
	auto inode = std::static_pointer_cast<Inode>(self->inode);

	co_await self->mutex.async_lock_shared();
	frg::shared_lock lock{frg::adopt_lock, self->mutex};

	co_return co_await detail::doWriteImpl(inode.get(), buffer, length, false, unsignedOffset);
}

template <FileSystem T>
async::result<frg::expected<protocols::fs::Error>> doTruncate(void *object, size_t size) {
	using File = typename T::File;
	using Inode = typename T::Inode;

	auto self = static_cast<File *>(object);
	auto inode = std::static_pointer_cast<Inode>(self->inode);

	protocols::ostrace::Timer timer;
	uint64_t timeReady = 0;
	uint64_t timeLock = 0;
	frg::scope_exit evtOnExit{[&] {
		ostContext.emit(
			ostEvtTruncate,
			ostAttrTime(timer.elapsed()),
			ostAttrNewSize(size),
			ostAttrTimeReady(timeReady),
			ostAttrTimeLock(timeLock)
		);
	}};

	co_await self->mutex.async_lock_shared();
	frg::shared_lock lock{frg::adopt_lock, self->mutex};
	timeLock += timer.split();

	co_await inode->readyEvent.wait();
	timeReady = timer.split();

	// Directories cannot be truncated.
	if (inode->fileType == FileType::kTypeDirectory)
		co_return protocols::fs::Error::isDirectory;

	co_await inode->inodeMutex.async_lock();
	frg::unique_lock inodeLock{frg::adopt_lock, inode->inodeMutex};
	timeLock += timer.split();

	FRG_CO_TRY(co_await inode->resizeFile(size));

	co_return frg::success;
}

template <FileSystem T>
async::result<helix::BorrowedDescriptor>
doAccessMemory(void *object) {
	using File = typename T::File;
	using Inode = typename T::Inode;

	auto self = static_cast<File *>(object);
	auto inode = std::static_pointer_cast<Inode>(self->inode);

	co_await inode->readyEvent.wait();
	co_return inode->accessMemory();
}

template <FileSystem T>
async::result<void> doObstructLink(std::shared_ptr<void> object, std::string name) {
	using Inode = typename T::Inode;

	auto self = std::static_pointer_cast<Inode>(object);

	{
		std::lock_guard obstructedLinksLock{self->obstructedLinksMutex};
		self->obstructedLinks.insert(name);
	}
	co_return;
}

template <FileSystem T>
async::result<protocols::fs::TraverseLinksResult>
doTraverseLinks(std::shared_ptr<void> object, std::deque<std::string> components) {
	using Inode = typename T::Inode;
	using DirEntry = typename T::DirEntry;

	auto self = std::static_pointer_cast<Inode>(object);

	std::optional<DirEntry> entry;
	size_t allComponents = components.size();

	std::vector<protocols::fs::TraversedLink> nodes;

	protocols::ostrace::Timer timer;
	uint64_t timeLock = 0;
	uint64_t timeFind = 0;
	frg::scope_exit evtOnExit{[&] {
		ostContext.emit(
			ostEvtTraverseLinks,
			ostAttrTime(timer.elapsed()),
			ostAttrNumComponents(allComponents),
			ostAttrNumResolved(nodes.size()),
			ostAttrTimeLock(timeLock),
			ostAttrTimeFind(timeFind)
		);
	}};

	// Stack of directories that we have entered.
	// Differs from the nodes vector: directories are popped here when resolving "..".
	std::vector<std::pair<std::shared_ptr<Inode>, int64_t>> dirStack;
	dirStack.push_back({self, static_cast<int64_t>(self->number)});

	while (!components.empty()) {
		auto component = components.front();

		if (component == ".") {
			components.pop_front();
			// "." and ".." are not entries that a client may cache, so they carry no serial.
			nodes.push_back({dirStack.back().first, dirStack.back().second, 0});
		} else if (component == "..") {
			// Break before popping the component: we must not ascend past the initial directory.
			if (dirStack.size() == 1)
				break;
			// Pop the directory: its parent will be pushed to the nodes vector below.
			dirStack.pop_back();

			components.pop_front();
			nodes.push_back({dirStack.back().first, dirStack.back().second, 0});
		} else {
			auto parent = dirStack.back().first;
			uint64_t serial;
			{
				protocols::ostrace::Timer lockTimer;
				co_await parent->inodeMutex.async_lock_shared();
				frg::shared_lock inodeLock{frg::adopt_lock, parent->inodeMutex};
				timeLock += lockTimer.elapsed();

				protocols::ostrace::Timer findTimer;
				auto found = co_await parent->findEntry(component);
				timeFind += findTimer.elapsed();
				if (!found)
					co_return std::unexpected{protocols::fs::TraverseLinksError{found.error()}};
				entry = found.value();
				serial = parent->dirSerial;
			}

			if (!entry) {
				co_return std::unexpected{protocols::fs::TraverseLinksError{
						protocols::fs::Error::fileNotFound, serial}};
			}
			assert(entry->inode);

			components.pop_front();
			nodes.push_back({self->fs.accessInode(entry->inode), entry->inode, serial});

			if (!components.empty()) {
				bool obstructed;
				{
					std::lock_guard obstructedLinksLock{parent->obstructedLinksMutex};
					obstructed = parent->obstructedLinks.find(component)
							!= parent->obstructedLinks.end();
				}
				if (obstructed) {
					break;
				}

				auto ino = self->fs.accessInode(entry->inode);
				if (entry->fileType == kTypeSymlink)
					break;

				if (entry->fileType != kTypeDirectory)
					co_return std::unexpected{protocols::fs::TraverseLinksError{
							protocols::fs::Error::notDirectory}};

				// Push the directory that we just entered.
				dirStack.push_back({std::static_pointer_cast<Inode>(ino), entry->inode});
			}
		}
	}

	if (!entry)
		co_return std::unexpected{protocols::fs::TraverseLinksError{protocols::fs::Error::fileNotFound}};

	protocols::fs::FileType type;
	switch (entry->fileType) {
		case kTypeDirectory:
			type = protocols::fs::FileType::directory;
			break;
		case kTypeRegular:
			type = protocols::fs::FileType::regular;
			break;
		case kTypeSymlink:
			type = protocols::fs::FileType::symlink;
			break;
		default:
			throw std::runtime_error("Unexpected file type");
	}

	co_return std::make_tuple(nodes, type, allComponents - components.size());
}

template <FileSystem T>
async::result<protocols::fs::OpenResult>
doOpen(std::shared_ptr<void> object, bool write, bool read, bool append) {
	using File = typename T::File;
	using Inode = typename T::Inode;

	auto self = std::static_pointer_cast<Inode>(object);

	protocols::ostrace::Timer timer;
	uint64_t timeReady = 0;
	uint64_t timeLock = 0;
	frg::scope_exit evtOnExit{[&] {
		ostContext.emit(
			ostEvtOpen,
			ostAttrTime(timer.elapsed()),
			ostAttrTimeReady(timeReady),
			ostAttrTimeLock(timeLock)
		);
	}};

	auto file = smarter::make_shared<File>(self, write, read, append);
	co_await self->readyEvent.wait();
	timeReady = timer.split();

	auto [localCtrl, remoteCtrl] = helix::createStream();
	auto [localPt, remotePt] = helix::createStream();

	{
		protocols::ostrace::Timer lockTimer;
		co_await self->inodeMutex.async_lock();
		frg::unique_lock inodeLock{frg::adopt_lock, self->inodeMutex};
		timeLock = lockTimer.elapsed();
		co_await self->updateTimes(clk::getRealtime(), std::nullopt, std::nullopt);
	}

	helix::DispatcherPool::global().detach([] (smarter::shared_ptr<File> file, BaseFileSystem &fs,
			helix::UniqueLane localCtrl, helix::UniqueLane localPt) -> async::result<void> {
		auto fileOps = fs.fileOps();

		co_await async::race_and_cancel(
			[&](async::cancellation_token) {
				return protocols::fs::serveFile(std::move(localCtrl),
						file.get(), fileOps);
			},
			[&](async::cancellation_token ct) {
				return protocols::fs::servePassthrough(std::move(localPt),
						file, fileOps, ct);
			}
		);
	}(file, self->fs, std::move(localCtrl), std::move(localPt)));

	co_return protocols::fs::OpenResult{std::move(remoteCtrl), std::move(remotePt)};
}

template <FileSystem T>
async::result<protocols::fs::Error> doUtimensat(std::shared_ptr<void> object,
		std::optional<timespec> atime, std::optional<timespec> mtime, timespec ctime) {
	using Inode = typename T::Inode;

	auto self = std::static_pointer_cast<Inode>(object);
	co_await self->readyEvent.wait();

	co_await self->inodeMutex.async_lock();
	frg::unique_lock inodeLock{frg::adopt_lock, self->inodeMutex};

	co_return co_await self->updateTimes(atime, mtime, ctime);
}

} // namespace blockfs
