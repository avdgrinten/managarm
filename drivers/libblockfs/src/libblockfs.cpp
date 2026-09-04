#include <async/cancellation.hpp>
#include <async/mutex.hpp>
#include <stdio.h>
#include <string.h>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <linux/cdrom.h>
#include <linux/fs.h>

#include <core/clock.hpp>
#include <core/dispatch.hpp>
#include <frg/scope_exit.hpp>
#include <helix/dispatcher-pool.hpp>
#include <helix/ipc.hpp>
#include <protocols/fs/server.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/ostrace/ostrace.hpp>

#include <blockfs.hpp>
#include "gpt.hpp"
#include "raw.hpp"
#include "trace.hpp"
#include "fs.bragi.hpp"
#include <bragi/helpers-std.hpp>

#include "ext2/ext2fs.hpp"
#include "btrfs/btrfs.hpp"

namespace blockfs {

bool clkInitialized = false;

async::mutex globalInitializationMutex;

BlockDevice::BlockDevice(size_t sector_size, int64_t parent_id, arch::contiguous_pool *pool)
: size(0), sectorSize(sector_size), sectorShift(std::countr_zero(sector_size)), parentId(parent_id), pagePool{pool} {
	assert(std::has_single_bit(sector_size));
}

struct HandlePartition {
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::CntRequest &&req, helix::BorrowedDescriptor conversation, bragi::preamble,
			gpt::Partition *, raw::RawFs *rawFs, std::unique_ptr<BaseFileSystem> *fsPtr) {
		if(req.req_type() == managarm::fs::CntReqType::SB_CREATE_REGULAR) {
			auto &fs = *fsPtr;
			auto inodeRaw = co_await fs->createRegular(req.uid(), req.gid(), 0);
			auto inode = std::static_pointer_cast<ext2fs::Inode>(inodeRaw);

			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			protocols::fs::serveNode(std::move(local_lane),
					inode, fs->nodeOps());

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_id(inode->number);
			resp.set_file_type(managarm::fs::FileType::REGULAR);

			auto ser = resp.SerializeAsString();
			auto [send_resp, push_node] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor(remote_lane, kHelRightInvoke | kHelRightManage)
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
		}else if(req.req_type() == managarm::fs::CntReqType::DEV_OPEN) {
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			auto file = smarter::make_shared<raw::OpenFile>(rawFs);
			helix::DispatcherPool::global().detach(protocols::fs::servePassthrough(std::move(local_lane),
							file,
							&raw::rawOperations));

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp, push_node] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor(remote_lane, kHelRightInvoke | kHelRightManage)
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
		}else{
			std::cout << "Unexpected request type " + std::to_string((int)req.req_type()) << std::endl;
			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
		}
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::MountRequest &&req, helix::BorrowedDescriptor conversation, bragi::preamble preamble,
			gpt::Partition *partition, raw::RawFs *, std::unique_ptr<BaseFileSystem> *fsPtr) {
		auto tailRes = co_await dispatchTail(req, conversation, preamble);
		if(!tailRes) {
			std::cout << "libblockfs: Rejecting request due to decoding failure" << std::endl;
			co_return std::unexpected(tailRes.error());
		}

		auto &fs = *fsPtr;

		// Mount the actual file system
		if (req.fs_type() == "ext2") {
			fs = std::make_unique<ext2fs::FileSystem>(partition);
			co_await static_cast<ext2fs::FileSystem *>(fs.get())->init();
			printf("ext2fs is ready!\n");
		} else if (req.fs_type() == "btrfs") {
			fs = std::make_unique<btrfs::FileSystem>(partition);
			co_await static_cast<btrfs::FileSystem *>(fs.get())->init();
		} else {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::NO_BACKING_DEVICE);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
			co_return {};
		}

		helix::UniqueLane local_lane, remote_lane;
		std::tie(local_lane, remote_lane) = helix::createStream();
		protocols::fs::serveNode(std::move(local_lane), fs->accessRoot(),
				fs->nodeOps());

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto [send_resp, push_node] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size()),
			helix_ng::pushDescriptor(remote_lane, kHelRightInvoke | kHelRightManage)
		);
		HEL_CHECK(send_resp.error());
		HEL_CHECK(push_node.error());
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::RenameRequest &&req, helix::BorrowedDescriptor conversation, bragi::preamble preamble,
			gpt::Partition *, raw::RawFs *, std::unique_ptr<BaseFileSystem> *fsPtr) {
		auto tailRes = co_await dispatchTail(req, conversation, preamble);
		if(!tailRes) {
			std::cout << "libblockfs: Rejecting request due to decoding failure" << std::endl;
			co_return std::unexpected(tailRes.error());
		}

		auto &fs = *fsPtr;

		protocols::ostrace::Timer timer;
		uint64_t lockTime = 0;
		uint64_t loopCheckTime = 0;
		uint64_t removeTime = 0;
		uint64_t linkTime = 0;
		frg::scope_exit evtOnExit{[&] {
			ostContext.emit(
				ostEvtRename,
				ostAttrTime(timer.elapsed()),
				ostAttrTimeLock(lockTime),
				ostAttrTimeLoopCheck(loopCheckTime),
				ostAttrTimeLink(linkTime),
				ostAttrTimeRemove(removeTime)
			);
		}};

		auto isInvalidName = [](std::string_view name) {
			return name.empty() || name == "." || name == "..";
		};
		if(isInvalidName(req.old_name()) || isInvalidName(req.new_name())) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()));
			HEL_CHECK(send_resp.error());
			co_return {};
		}

		auto oldInodeRaw = fs->accessInode(req.inode_source());
		auto newInodeRaw = fs->accessInode(req.inode_target());

		auto oldInode = std::static_pointer_cast<ext2fs::Inode>(oldInodeRaw);
		auto newInode = std::static_pointer_cast<ext2fs::Inode>(newInodeRaw);

		// Take topologyMutex exclusively for the whole rename.
		// This ensures that the ancestry checks below see a consistent state.
		protocols::ostrace::Timer topologyLockTimer;
		co_await fs->topologyMutex.async_lock();
		frg::unique_lock topologyLock{frg::adopt_lock, fs->topologyMutex};
		lockTime += topologyLockTimer.elapsed();

		// Lock the two parent directories in inodeMutex order.
		std::optional<frg::unique_lock<async::shared_mutex>> firstLock;
		std::optional<frg::unique_lock<async::shared_mutex>> secondLock;
		if (oldInode.get() == newInode.get()) {
			protocols::ostrace::Timer lockTimer;
			co_await oldInode->inodeMutex.async_lock();
			firstLock.emplace(frg::adopt_lock, oldInode->inodeMutex);
			lockTime += lockTimer.elapsed();
		} else {
			protocols::ostrace::Timer loopCheckTimer;
			auto oldIsSubdir = co_await oldInode->isSubdirectoryOf(newInode->number);
			auto newIsSubdir = co_await newInode->isSubdirectoryOf(oldInode->number);
			loopCheckTime += loopCheckTimer.elapsed();
			// TODO: Handle errors gracefully here.
			assert(oldIsSubdir); // isSubdirectoryOf() should not fail.
			assert(newIsSubdir); // isSubdirectoryOf() should not fail.

			bool oldBeforeNew;
			if (oldIsSubdir.value()) {
				oldBeforeNew = false;
			} else if (newIsSubdir.value()) {
				oldBeforeNew = true;
			} else {
				oldBeforeNew = oldInode->number < newInode->number;
			}

			ext2fs::Inode *firstInode = oldInode.get();
			ext2fs::Inode *secondInode = newInode.get();
			if (!oldBeforeNew)
				std::swap(firstInode, secondInode);

			protocols::ostrace::Timer lockTimer;
			co_await firstInode->inodeMutex.async_lock();
			firstLock.emplace(frg::adopt_lock, firstInode->inodeMutex);

			co_await secondInode->inodeMutex.async_lock();
			secondLock.emplace(frg::adopt_lock, secondInode->inodeMutex);
			lockTime += lockTimer.elapsed();
		}

		auto old_result = co_await oldInode->findEntry(req.old_name());
		if(!old_result) {
			managarm::fs::SvrResponse resp;
			assert(old_result.error() == protocols::fs::Error::notDirectory);
			resp.set_error(managarm::fs::Errors::NOT_DIRECTORY);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()));
			HEL_CHECK(send_resp.error());
			co_return {};
		}

		auto old_file = old_result.value();
		managarm::fs::SvrResponse resp;

		// Keep the moved and victim inodes alive.
		std::shared_ptr<ext2fs::Inode> movedInode, victimInode;
		// These locks are taken below for the moved and victim inodes.
		std::optional<frg::unique_lock<async::shared_mutex>> thirdLock, fourthLock;

		if(old_file) {
			// Reject moving a directory into itself or one of its own
			// descendants, which would detach the subtree from the tree.
			if(old_file.value().fileType == kTypeDirectory) {
				protocols::ostrace::Timer loopCheckTimer;
				auto loop = co_await newInode->isSubdirectoryOf(old_file.value().inode);
				loopCheckTime += loopCheckTimer.elapsed();
				if(!loop) {
					resp.set_error(loop.error() | protocols::fs::toFsError);

					auto ser = resp.SerializeAsString();
					auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
						helix_ng::sendBuffer(ser.data(), ser.size()));
					HEL_CHECK(send_resp.error());
					co_return {};
				}
				if(loop.value()) {
					resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);

					auto ser = resp.SerializeAsString();
					auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
						helix_ng::sendBuffer(ser.data(), ser.size()));
					HEL_CHECK(send_resp.error());
					co_return {};
				}
			}

			// If new_name names an existing directory, refuse to overwrite
			// a non-empty one rather than corrupting it via removeEntry.
			auto new_entry = co_await newInode->findEntry(req.new_name());
			if(!new_entry) {
				assert(new_entry.error() == protocols::fs::Error::notDirectory);
				resp.set_error(managarm::fs::Errors::NOT_DIRECTORY);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()));
				HEL_CHECK(send_resp.error());
				co_return {};
			}
			if(new_entry.value()) {
				// If old_name and new_name resolve to the same inode, rename is a
				// no-op; the removeEntry/link below would otherwise drop the file's
				// last link.
				if(old_file.value().inode == new_entry.value()->inode) {
					resp.set_error(managarm::fs::Errors::SUCCESS);

					auto ser = resp.SerializeAsString();
					auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
						helix_ng::sendBuffer(ser.data(), ser.size()));
					HEL_CHECK(send_resp.error());
					co_return {};
				}
				// POSIX only allows replacing a directory with a directory and a
				// non-directory with a non-directory.
				auto sourceIsDir = old_file.value().fileType == kTypeDirectory;
				auto targetIsDir = new_entry.value()->fileType == kTypeDirectory;
				if(sourceIsDir && !targetIsDir) {
					resp.set_error(managarm::fs::Errors::NOT_DIRECTORY);

					auto ser = resp.SerializeAsString();
					auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
						helix_ng::sendBuffer(ser.data(), ser.size()));
					HEL_CHECK(send_resp.error());
					co_return {};
				}
				if(!sourceIsDir && targetIsDir) {
					resp.set_error(managarm::fs::Errors::IS_DIRECTORY);

					auto ser = resp.SerializeAsString();
					auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
						helix_ng::sendBuffer(ser.data(), ser.size()));
					HEL_CHECK(send_resp.error());
					co_return {};
				}
			}

			// Lock the moved inode and victim inode in inodeMutex order.
			movedInode = std::static_pointer_cast<ext2fs::Inode>(
				fs->accessInode(old_file.value().inode)
			);
			if(new_entry.value()) {
				victimInode = std::static_pointer_cast<ext2fs::Inode>(
					fs->accessInode(new_entry.value()->inode)
				);
			}

			auto stillUnlocked = [&] (ext2fs::Inode *inode) {
				return inode != oldInode.get() && inode != newInode.get();
			};
			if(!victimInode) {
				if (stillUnlocked(movedInode.get())) {
					co_await movedInode->inodeMutex.async_lock();
					thirdLock.emplace(frg::adopt_lock, movedInode->inodeMutex);
				}
			} else {
				bool movedBeforeVictim = movedInode->number < victimInode->number;

				ext2fs::Inode *thirdInode = movedInode.get();
				ext2fs::Inode *fourthInode = victimInode.get();
				if (!movedBeforeVictim)
					std::swap(thirdInode, fourthInode);

				if (stillUnlocked(thirdInode)) {
					co_await thirdInode->inodeMutex.async_lock();
					thirdLock.emplace(frg::adopt_lock, thirdInode->inodeMutex);
				}
				if (stillUnlocked(fourthInode)) {
					co_await fourthInode->inodeMutex.async_lock();
					fourthLock.emplace(frg::adopt_lock, fourthInode->inodeMutex);
				}
			}

			if(new_entry.value() && new_entry.value()->fileType == kTypeDirectory) {
				auto isEmpty = co_await victimInode->isDirectoryEmpty();
				if(!isEmpty) {
					std::cout << "libblockfs: rename: isDirectoryEmpty failed: "
						<< (int)isEmpty.error() << std::endl;
					resp.set_error(managarm::fs::Errors::INTERNAL_ERROR);

					auto ser = resp.SerializeAsString();
					auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
						helix_ng::sendBuffer(ser.data(), ser.size()));
					HEL_CHECK(send_resp.error());
					co_return {};
				}
				if(!isEmpty.value()) {
					resp.set_error(managarm::fs::Errors::DIRECTORY_NOT_EMPTY);

					auto ser = resp.SerializeAsString();
					auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
						helix_ng::sendBuffer(ser.data(), ser.size()));
					HEL_CHECK(send_resp.error());
					co_return {};
				}
			}

			// A missing destination is the common case and not an error.
			protocols::ostrace::Timer removeTimer;
			auto result = co_await newInode->removeEntry(req.new_name());
			removeTime += removeTimer.elapsed();
			if(!result && result.error() != protocols::fs::Error::fileNotFound) {
				resp.set_error(result.error() | protocols::fs::toFsError);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()));
				HEL_CHECK(send_resp.error());
				co_return {};
			}
			protocols::ostrace::Timer linkTimer;
			auto link_result = co_await newInode->link(req.new_name(),
					old_file.value().inode, old_file.value().fileType);
			linkTime += linkTimer.elapsed();
			if(!link_result) {
				// We already mutated the directory, so report a serial.
				resp.set_serial(fs->recordMutation({oldInode.get(), newInode.get()}));
				resp.set_error(link_result.error() | protocols::fs::toFsError);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()));
				HEL_CHECK(send_resp.error());
				co_return {};
			}

			// Moving a directory to a different parent must repoint its ".."
			// entry. The link() and removeEntry() above already shifted the
			// subdirectory link between the two parents.
			if(old_file.value().fileType == kTypeDirectory
					&& oldInode->number != newInode->number) {
				auto reparent = co_await movedInode->updateDotDot(newInode->number);
				if(!reparent) {
					// We already mutated the directory, so report a serial.
					resp.set_serial(fs->recordMutation({oldInode.get(), newInode.get()}));
					resp.set_error(reparent.error() | protocols::fs::toFsError);

					auto ser = resp.SerializeAsString();
					auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
						helix_ng::sendBuffer(ser.data(), ser.size()));
					HEL_CHECK(send_resp.error());
					co_return {};
				}
			}
		} else {
			resp.set_error(managarm::fs::Errors::FILE_NOT_FOUND);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()));
			HEL_CHECK(send_resp.error());
			co_return {};
		}

		protocols::ostrace::Timer removeOldTimer;
		auto result = co_await oldInode->removeEntry(req.old_name());
		removeTime += removeOldTimer.elapsed();
		if(!result) {
			// We already mutated the directory, so report a serial.
			resp.set_serial(fs->recordMutation({oldInode.get(), newInode.get()}));
			resp.set_error(result.error() | protocols::fs::toFsError);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()));
			HEL_CHECK(send_resp.error());
			co_return {};
		}
		// Report one serial for both directories, so that the client can order this
		// rename against the mutations it observes on either directory.
		resp.set_serial(fs->recordMutation({oldInode.get(), newInode.get()}));
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
			helix_ng::sendBuffer(ser.data(), ser.size()));
		HEL_CHECK(send_resp.error());
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::GetFsStatsRequest &&, helix::BorrowedDescriptor conversation, bragi::preamble,
			gpt::Partition *, raw::RawFs *, std::unique_ptr<BaseFileSystem> *fsPtr) {
		auto &fs = *fsPtr;
		managarm::fs::GetFsStatsResponse resp;
		if(!fs) {
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);
		} else {
			auto stats = fs->getFsStats();

			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_fs_type(stats.fsType);
			resp.set_block_size(stats.blockSize);
			resp.set_fragment_size(stats.fragmentSize);
			resp.set_num_blocks(stats.numBlocks);
			resp.set_blocks_free(stats.blocksFree);
			resp.set_blocks_free_user(stats.blocksFreeUser);
			resp.set_num_inodes(stats.numInodes);
			resp.set_inodes_free(stats.inodesFree);
			resp.set_inodes_free_user(stats.inodesFreeUser);
			resp.set_max_name_length(stats.maxNameLength);
			resp.set_fsid0(stats.fsid[0]);
			resp.set_fsid1(stats.fsid[1]);
			resp.set_flags(stats.flags);
		}

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::GenericIoctlRequest &&req, helix::BorrowedDescriptor conversation, bragi::preamble,
			gpt::Partition *partition, raw::RawFs *, std::unique_ptr<BaseFileSystem> *) {
		if(req.command() == BLKGETSIZE64) {
			managarm::fs::GenericIoctlReply rsp;
			rsp.set_error(managarm::fs::Errors::SUCCESS);
			rsp.set_size(co_await partition->getSize());

			auto ser = rsp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		} else {
			std::cout << "\e[31m" "libblockfs: Unknown ioctl() message with ID "
					<< req.command() << "\e[39m" << std::endl;

			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
		}
		co_return {};
	}
};

async::detached servePartition(helix::UniqueLane lane, gpt::Partition *partition, std::unique_ptr<raw::RawFs> rawFs) {
	std::cout << "unix device: Connection" << std::endl;

	// TODO(qookie): Generic file system type
	std::unique_ptr<BaseFileSystem> fs;

	while(true) {
		auto res = co_await dispatchRequest<
			managarm::fs::CntRequest,
			managarm::fs::MountRequest,
			managarm::fs::RenameRequest,
			managarm::fs::GetFsStatsRequest,
			managarm::fs::GenericIoctlRequest
		>(lane, HandlePartition{}, partition, rawFs.get(), &fs);
		if(!res) {
			if(res.error() == DispatchError::shutdown)
				co_return;
			std::cout << "libblockfs: dispatch error on partition lane" << std::endl;
			continue;
		}
	}
}

struct HandleDevice {
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::CntRequest &&req, helix::BorrowedDescriptor conversation, bragi::preamble, raw::RawFs *rawFs) {
		if(req.req_type() == managarm::fs::CntReqType::DEV_OPEN) {
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			auto file = smarter::make_shared<raw::OpenFile>(rawFs);
			helix::DispatcherPool::global().detach(protocols::fs::servePassthrough(std::move(local_lane),
							file,
							&raw::rawOperations));

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp, push_node] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor(remote_lane, kHelRightInvoke | kHelRightManage)
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
		}else{
			std::cout << "Unexpected request type " + std::to_string((int)req.req_type()) << " to device" << std::endl;
			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
		}
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::MountRequest &&req, helix::BorrowedDescriptor conversation, bragi::preamble preamble, raw::RawFs *) {
		auto tailRes = co_await dispatchTail(req, conversation, preamble);
		if(!tailRes)
			co_return std::unexpected(tailRes.error());

		managarm::fs::SvrResponse resp;
		resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::fs::GenericIoctlRequest &&req, helix::BorrowedDescriptor conversation, bragi::preamble, raw::RawFs *rawFs) {
		if(req.command() == BLKGETSIZE64) {
			managarm::fs::GenericIoctlReply rsp;
			rsp.set_error(managarm::fs::Errors::SUCCESS);
			rsp.set_size(co_await rawFs->device->getSize());

			auto ser = rsp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		} else {
			std::cout << "\e[31m" "libblockfs: Unknown ioctl() message with ID "
					<< req.command() << "\e[39m" << std::endl;

			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
		}
		co_return {};
	}
};

async::detached serveDevice(helix::UniqueLane lane, std::unique_ptr<raw::RawFs> rawFs) {
	std::cout << "unix device: Connection" << std::endl;

	while(true) {
		auto res = co_await dispatchRequest<
			managarm::fs::CntRequest,
			managarm::fs::MountRequest,
			managarm::fs::GenericIoctlRequest
		>(lane, HandleDevice{}, rawFs.get());
		if(!res) {
			if(res.error() == DispatchError::shutdown)
				co_return;
			std::cout << "libblockfs: dispatch error on device lane" << std::endl;
			continue;
		}
	}
}

async::detached runDevice(BlockDevice *device) {
	co_await ostContext.create();

	{
		co_await globalInitializationMutex.async_lock();
		frg::unique_lock lock{frg::adopt_lock, globalInitializationMutex};

		if(!clkInitialized) {
			co_await clk::enumerateTracker();
			clkInitialized = true;
		}
	}

	// TODO(qookie): Don't leak the table.
	// Currently it should be fine to leak it since neither it nor
	// the device gets deleted anyway.
	auto table = new gpt::Table(device);
	co_await table->parse();

	int64_t diskId = 0;
	{
		mbus_ng::Properties descriptor {
			{"unix.devtype", mbus_ng::StringItem{"block"}},
			{"unix.blocktype", mbus_ng::StringItem{"disk"}},
			{"unix.diskname-prefix", mbus_ng::StringItem{device->diskNamePrefix}},
			{"unix.diskname-suffix", mbus_ng::StringItem{device->diskNameSuffix}},
			{"drvcore.mbus-parent", mbus_ng::StringItem{std::to_string(device->parentId)}}
		};

		auto entity = (co_await mbus_ng::Instance::global().createEntity(
					"disk", descriptor)).unwrap();
		diskId = entity.id();

		auto rawFs = std::make_unique<raw::RawFs>(device);
		co_await rawFs->init();

		// See comment in mbus_ng::~EntityManager as to why this is necessary.
		[] (mbus_ng::EntityManager entity, std::unique_ptr<raw::RawFs> rawFs) -> async::detached {
			while (true) {
				auto [localLane, remoteLane] = helix::createStream();

				// If this fails, too bad!
				(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));

				serveDevice(std::move(localLane), std::move(rawFs));
			}
		}(std::move(entity), std::move(rawFs));
	}

	int partId = 0;
	for(size_t i = 0; i < table->numPartitions(); ++i) {
		auto type = table->getPartition(i).type();
		printf("Partition %lu, type: %.8X-%.4X-%.4X-%.2X%.2X-%.2X%.2X%.2X%.2X%.2X%.2X\n",
				i, type.a, type.b, type.c, type.d[0], type.d[1],
				type.e[0], type.e[1], type.e[2], type.e[3], type.e[4], type.e[5]);

		if(type == gpt::type_guids::managarmRootPartition)
			printf("  It's a Managarm root partition!\n");

		auto paritition = &table->getPartition(i);

		auto rawFs = std::make_unique<raw::RawFs>(paritition);
		co_await rawFs->init();

		// Create an mbus object for the partition.
		mbus_ng::Properties descriptor{
			{"unix.devtype", mbus_ng::StringItem{"block"}},
			{"unix.blocktype", mbus_ng::StringItem{"partition"}},
			{"unix.partid", mbus_ng::StringItem{std::to_string(partId++)}},
			{"unix.diskid", mbus_ng::StringItem{std::to_string(diskId)}},
			{"unix.partname-suffix", mbus_ng::StringItem{device->partNameSuffix}},
			{"drvcore.mbus-parent", mbus_ng::StringItem{std::to_string(paritition->parentId)}},
			{"unix.is-managarm-root", mbus_ng::StringItem{std::to_string(type == gpt::type_guids::managarmRootPartition)}}
		};

		auto entity = (co_await mbus_ng::Instance::global().createEntity(
					"partition", descriptor)).unwrap();

		[] (mbus_ng::EntityManager entity, gpt::Partition *partition, std::unique_ptr<raw::RawFs> rawFs) -> async::detached {
			while (true) {
				auto [localLane, remoteLane] = helix::createStream();

				// If this fails, too bad!
				(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));

				servePartition(std::move(localLane), partition, std::move(rawFs));
			}
		}(std::move(entity), paritition, std::move(rawFs));
	}
}

} // namespace blockfs
