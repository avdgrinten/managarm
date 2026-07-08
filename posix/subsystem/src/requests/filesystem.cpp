#include "common.hpp"
#include "../vfs.hpp"
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/limits.h>
#include <iostream>

namespace requests {

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::ChrootRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();

	auto tailRes = co_await dispatchTail(req, conversation, preamble);
	if(!tailRes)
		co_return std::unexpected(tailRes.error());
	logBragiRequest(req);

	logRequest(logRequests, self, "CHROOT");

	auto pathResult = co_await resolve(self->fsContext()->getRoot(),
			self->fsContext()->getWorkingDirectory(), req.path(), self.get());
	if(!pathResult) {
		if(pathResult.error() == protocols::fs::Error::fileNotFound) {
			co_await sendErrorResponse<managarm::posix::ChrootResponse>(conversation, managarm::posix::Errors::FILE_NOT_FOUND);
			co_return {};
		} else if(pathResult.error() == protocols::fs::Error::notDirectory) {
			co_await sendErrorResponse<managarm::posix::ChrootResponse>(conversation, managarm::posix::Errors::NOT_A_DIRECTORY);
			co_return {};
		} else {
			std::cout << "posix: Unexpected failure from resolve()" << std::endl;
			co_return {};
		}
	}
	auto path = pathResult.value();
	auto rootResult = self->fsContext()->changeRoot(path);
	if(!rootResult) {
		co_await sendErrorResponse<managarm::posix::ChrootResponse>(conversation, rootResult.error() | toPosixProtoError);
		co_return {};
	}

	managarm::posix::ChrootResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::ChdirRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();

	auto tailRes = co_await dispatchTail(req, conversation, preamble);
	if(!tailRes)
		co_return std::unexpected(tailRes.error());
	logBragiRequest(req);

	logRequest(logRequests, self, "CHDIR");

	auto pathResult = co_await resolve(self->fsContext()->getRoot(),
			self->fsContext()->getWorkingDirectory(), req.path(), self.get());
	if(!pathResult) {
		if(pathResult.error() == protocols::fs::Error::fileNotFound) {
			co_await sendErrorResponse<managarm::posix::ChdirResponse>(conversation, managarm::posix::Errors::FILE_NOT_FOUND);
			co_return {};
		} else if(pathResult.error() == protocols::fs::Error::notDirectory) {
			co_await sendErrorResponse<managarm::posix::ChdirResponse>(conversation, managarm::posix::Errors::NOT_A_DIRECTORY);
			co_return {};
		} else {
			std::cout << "posix: Unexpected failure from resolve()" << std::endl;
			co_return {};
		}
	}
	auto path = pathResult.value();
	auto cwdResult = self->fsContext()->changeWorkingDirectory(path);
	if(!cwdResult) {
		co_await sendErrorResponse<managarm::posix::ChdirResponse>(conversation, cwdResult.error() | toPosixProtoError);
		co_return {};
	}

	managarm::posix::ChdirResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::FchdirRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);
	logRequest(logRequests, self, "FCHDIR");

	managarm::posix::FchdirResponse resp;

	auto file = self->fileContext()->getFile(req.fd());

	if(!file) {
		co_await sendErrorResponse<managarm::posix::FchdirResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
		co_return {};
	}

	auto cwdResult = self->fsContext()->changeWorkingDirectory({file->associatedMount(),
			file->associatedLink()});
	if(!cwdResult) {
		co_await sendErrorResponse<managarm::posix::FchdirResponse>(conversation, cwdResult.error() | toPosixProtoError);
		co_return {};
	}

	resp.set_error(managarm::posix::Errors::SUCCESS);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::AccessAtRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();

	auto tailRes = co_await dispatchTail(req, conversation, preamble);
	if(!tailRes)
		co_return std::unexpected(tailRes.error());
	logBragiRequest(req);

	ViewPath relative_to;
	smarter::shared_ptr<File, FileHandle> file;

	ResolveFlags resolveFlags = {};

	if(req.flags() & AT_SYMLINK_NOFOLLOW)
		resolveFlags |= resolveDontFollow;

	if(req.flags() & ~(AT_SYMLINK_NOFOLLOW)) {
		if(req.flags() & AT_EACCESS) {
			std::cout << "posix: ACCESSAT flag handling AT_EACCESS is unimplemented" << std::endl;
		} else {
			std::cout << "posix: ACCESSAT unknown flag is unimplemented: " << req.flags() << std::endl;
			co_await sendErrorResponse<managarm::posix::AccessAtResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			co_return {};
		}
	}

	if(req.fd() == AT_FDCWD) {
		relative_to = self->fsContext()->getWorkingDirectory();
	} else {
		file = self->fileContext()->getFile(req.fd());

		if(!file) {
			co_await sendErrorResponse<managarm::posix::AccessAtResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
			co_return {};
		}

		relative_to = {file->associatedMount(), file->associatedLink()};
	}

	auto pathResult = co_await resolve(self->fsContext()->getRoot(),
			relative_to, req.path(), self.get(), resolveFlags);
	if(!pathResult) {
		if(pathResult.error() == protocols::fs::Error::fileNotFound) {
			co_await sendErrorResponse<managarm::posix::AccessAtResponse>(conversation, managarm::posix::Errors::FILE_NOT_FOUND);
			co_return {};
		} else if(pathResult.error() == protocols::fs::Error::notDirectory) {
			co_await sendErrorResponse<managarm::posix::AccessAtResponse>(conversation, managarm::posix::Errors::NOT_A_DIRECTORY);
			co_return {};
		} else {
			std::cout << "posix: Unexpected failure from resolve()" << std::endl;
			co_return {};
		}
	}

	logRequest(logRequests || logPaths, self, "ACCESSAT", "'{}'", pathResult.value().getPath(self->fsContext()->getRoot()));

	co_await sendErrorResponse<managarm::posix::AccessAtResponse>(conversation, managarm::posix::Errors::SUCCESS);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::MkdirAtRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();

	auto tailRes = co_await dispatchTail(req, conversation, preamble);
	if(!tailRes)
		co_return std::unexpected(tailRes.error());
	logBragiRequest(req);

	logRequest(logRequests || logPaths, self, "MKDIRAT", "path='{}'", req.path());

	if(!req.path().size()) {
		co_await sendErrorResponse<managarm::posix::MkdirAtResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	ViewPath relative_to;
	smarter::shared_ptr<File, FileHandle> file;

	if(req.fd() == AT_FDCWD) {
		relative_to = self->fsContext()->getWorkingDirectory();
	} else {
		file = self->fileContext()->getFile(req.fd());

		if (!file) {
			co_await sendErrorResponse<managarm::posix::MkdirAtResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
			co_return {};
		}

		relative_to = {file->associatedMount(), file->associatedLink()};
	}

	PathResolver resolver;
	resolver.setup(self->fsContext()->getRoot(),
			relative_to, req.path(), self.get());
	auto resolveResult = co_await resolver.resolve(resolvePrefix);
	if(!resolveResult) {
		if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
			co_await sendErrorResponse<managarm::posix::MkdirAtResponse>(conversation, managarm::posix::Errors::FILE_NOT_FOUND);
			co_return {};
		} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
			co_await sendErrorResponse<managarm::posix::MkdirAtResponse>(conversation, managarm::posix::Errors::NOT_A_DIRECTORY);
			co_return {};
		} else {
			std::cout << "posix: Unexpected failure from resolve()" << std::endl;
			co_return {};
		}
	}

	if(!resolver.hasComponent()) {
		co_await sendErrorResponse<managarm::posix::MkdirAtResponse>(conversation, managarm::posix::Errors::ALREADY_EXISTS);
		co_return {};
	}

	auto parent = resolver.currentLink()->getTarget();
	auto existsResult = co_await parent->getLink(resolver.nextComponent());
	if (existsResult) {
		co_await sendErrorResponse<managarm::posix::MkdirAtResponse>(conversation, managarm::posix::Errors::ALREADY_EXISTS);
		co_return {};
	}

	auto result = co_await parent->mkdir(
	    self.get(),
	    resolver.nextComponent(),
	    req.mode()
	);
	if(auto error = std::get_if<Error>(&result); error) {
		co_await sendErrorResponse<managarm::posix::MkdirAtResponse>(conversation, *error | toPosixProtoError);
		co_return {};
	}

	co_await sendErrorResponse<managarm::posix::MkdirAtResponse>(conversation, managarm::posix::Errors::SUCCESS);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::MkfifoAtRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();

	auto tailRes = co_await dispatchTail(req, conversation, preamble);
	if(!tailRes)
		co_return std::unexpected(tailRes.error());
	logBragiRequest(req);

	logRequest(logRequests || logPaths, self, "MKFIFOAT", "path='{}'", req.path());

	if (!req.path().size()) {
		co_await sendErrorResponse<managarm::posix::MkfifoAtResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	ViewPath relative_to;
	smarter::shared_ptr<File, FileHandle> file;
	std::shared_ptr<FsLink> target_link;

	if (req.fd() == AT_FDCWD) {
		relative_to = self->fsContext()->getWorkingDirectory();
	} else {
		file = self->fileContext()->getFile(req.fd());

		if (!file) {
			co_await sendErrorResponse<managarm::posix::MkfifoAtResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
			co_return {};
		}

		relative_to = {file->associatedMount(), file->associatedLink()};
	}

	PathResolver resolver;
	resolver.setup(self->fsContext()->getRoot(),
			relative_to, req.path(), self.get());
	auto resolveResult = co_await resolver.resolve(resolvePrefix | resolveCreatesNonDirectory);
	if(!resolveResult) {
		co_await sendErrorResponse<managarm::posix::MkfifoAtResponse>(conversation,
				resolveResult.error() | toPosixError | toPosixProtoError);
		co_return {};
	}

	auto parent = resolver.currentLink()->getTarget();
	if(co_await parent->getLink(resolver.nextComponent())) {
		co_await sendErrorResponse<managarm::posix::MkfifoAtResponse>(conversation, managarm::posix::Errors::ALREADY_EXISTS);
		co_return {};
	}

	auto result = co_await parent->mkfifo(
		resolver.nextComponent(),
		req.mode() & ~self->fsContext()->getUmask()
	);
	if(!result) {
		std::cout << "posix: Unexpected failure from mkfifo()" << std::endl;
		co_return {};
	}

	co_await sendErrorResponse<managarm::posix::MkfifoAtResponse>(conversation, managarm::posix::Errors::SUCCESS);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::LinkAtRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();

	auto tailRes = co_await dispatchTail(req, conversation, preamble);
	if(!tailRes)
		co_return std::unexpected(tailRes.error());
	logBragiRequest(req);

	logRequest(logRequests, self, "LINKAT");

	if(req.flags() & ~(AT_EMPTY_PATH | AT_SYMLINK_FOLLOW)) {
		co_await sendErrorResponse<managarm::posix::LinkAtResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	if(req.flags() & AT_EMPTY_PATH) {
		std::cout << "posix: AT_EMPTY_PATH is unimplemented for linkat" << std::endl;
	}

	if(req.flags() & AT_SYMLINK_FOLLOW) {
		std::cout << "posix: AT_SYMLINK_FOLLOW is unimplemented for linkat" << std::endl;
	}

	ViewPath relative_to;
	smarter::shared_ptr<File, FileHandle> file;

	if(req.fd() == AT_FDCWD) {
		relative_to = self->fsContext()->getWorkingDirectory();
	} else {
		file = self->fileContext()->getFile(req.fd());

		if(!file) {
			co_await sendErrorResponse<managarm::posix::LinkAtResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
			co_return {};
		}

		relative_to = {file->associatedMount(), file->associatedLink()};
	}

	PathResolver resolver;
	resolver.setup(self->fsContext()->getRoot(),
			relative_to, req.path(), self.get());
	auto resolveResult = co_await resolver.resolve();
	if(!resolveResult) {
		if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
			co_await sendErrorResponse<managarm::posix::LinkAtResponse>(conversation, managarm::posix::Errors::FILE_NOT_FOUND);
			co_return {};
		} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
			co_await sendErrorResponse<managarm::posix::LinkAtResponse>(conversation, managarm::posix::Errors::NOT_A_DIRECTORY);
			co_return {};
		} else {
			std::cout << "posix: Unexpected failure from resolve()" << std::endl;
			co_return {};
		}
	}

	if (req.newfd() == AT_FDCWD) {
		relative_to = self->fsContext()->getWorkingDirectory();
	} else {
		file = self->fileContext()->getFile(req.newfd());

		if(!file) {
			co_await sendErrorResponse<managarm::posix::LinkAtResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
			co_return {};
		}

		relative_to = {file->associatedMount(), file->associatedLink()};
	}

	PathResolver new_resolver;
	new_resolver.setup(self->fsContext()->getRoot(),
			relative_to, req.target_path(), self.get());
	auto new_resolveResult = co_await new_resolver.resolve(
			resolvePrefix | resolveCreatesNonDirectory);
	if(!new_resolveResult) {
		co_await sendErrorResponse<managarm::posix::LinkAtResponse>(conversation,
				new_resolveResult.error() | toPosixError | toPosixProtoError);
		co_return {};
	}

	auto target = resolver.currentLink()->getTarget();
	auto directory = new_resolver.currentLink()->getTarget();
	assert(target->superblock() == directory->superblock()); // Hard links across mount points are not allowed, return EXDEV
	auto result = co_await directory->link(new_resolver.nextComponent(), target);
	if(!result) {
		co_await sendErrorResponse<managarm::posix::LinkAtResponse>(conversation, result.error() | toPosixProtoError);
		co_return {};
	}

	co_await sendErrorResponse<managarm::posix::LinkAtResponse>(conversation, managarm::posix::Errors::SUCCESS);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::SymlinkAtRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();

	auto tailRes = co_await dispatchTail(req, conversation, preamble);
	if(!tailRes)
		co_return std::unexpected(tailRes.error());
	logBragiRequest(req);

	ViewPath relativeTo;
	smarter::shared_ptr<File, FileHandle> file;

	if (!req.path().size()) {
		co_await sendErrorResponse<managarm::posix::SymlinkAtResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	if(req.fd() == AT_FDCWD) {
		relativeTo = self->fsContext()->getWorkingDirectory();
	} else {
		file = self->fileContext()->getFile(req.fd());
		if (!file) {
			co_await sendErrorResponse<managarm::posix::SymlinkAtResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
			co_return {};
		}

		relativeTo = {file->associatedMount(), file->associatedLink()};
	}

	PathResolver resolver;
	resolver.setup(self->fsContext()->getRoot(),
			relativeTo, req.path(), self.get());
	auto resolveResult = co_await resolver.resolve(
			resolvePrefix | resolveCreatesNonDirectory);
	if(!resolveResult) {
		co_await sendErrorResponse<managarm::posix::SymlinkAtResponse>(conversation,
				resolveResult.error() | toPosixError | toPosixProtoError);
		co_return {};
	}

	logRequest(logRequests || logPaths, self, "SYMLINK", "'{}{}' -> '{}'",
		ViewPath{resolver.currentView(), resolver.currentLink()}
			.getPath(self->fsContext()->getRoot()),
		resolver.nextComponent(),
		req.target_path());

	auto parent = resolver.currentLink()->getTarget();
	auto result = co_await parent->symlink(resolver.nextComponent(), req.target_path());
	if(auto error = std::get_if<Error>(&result); error) {
		if(*error == Error::alreadyExists) {
			co_await sendErrorResponse<managarm::posix::SymlinkAtResponse>(conversation, managarm::posix::Errors::ALREADY_EXISTS);
			co_return {};
		} else {
			assert(*error == Error::illegalOperationTarget);
			co_await sendErrorResponse<managarm::posix::SymlinkAtResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			co_return {};
		}
	}

	managarm::posix::SymlinkAtResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	auto [sendResp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(sendResp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::ReadlinkAtRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();

	auto tailRes = co_await dispatchTail(req, conversation, preamble);
	if(!tailRes)
		co_return std::unexpected(tailRes.error());
	logBragiRequest(req);

	ViewPath relative_to;
	smarter::shared_ptr<File, FileHandle> file;

	if(req.fd() == AT_FDCWD) {
		relative_to = self->fsContext()->getWorkingDirectory();
	} else {
		file = self->fileContext()->getFile(req.fd());

		if (!file) {
			co_await sendErrorResponse<managarm::posix::ReadlinkAtResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
			co_return {};
		}

		relative_to = {file->associatedMount(), file->associatedLink()};
	}

	auto pathResult = co_await resolve(self->fsContext()->getRoot(),
			relative_to, req.path(), self.get(), resolveDontFollow);
	if(!pathResult) {
		if(pathResult.error() == protocols::fs::Error::fileNotFound) {
			managarm::posix::ReadlinkAtResponse resp;
			resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);

			auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
				helix_ng::sendBuffer(nullptr, 0)
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
			co_return {};
		} else if(pathResult.error() == protocols::fs::Error::notDirectory) {
			managarm::posix::ReadlinkAtResponse resp;
			resp.set_error(managarm::posix::Errors::NOT_A_DIRECTORY);

			auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
				helix_ng::sendBuffer(nullptr, 0)
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
			co_return {};
		} else {
			std::cout << "posix: Unexpected failure from resolve()" << std::endl;
			co_return {};
		}
	}
	auto path = pathResult.value();

	auto result = co_await path.second->getTarget()->readSymlink(path.second.get(), self.get());
	if(auto error = std::get_if<Error>(&result); error) {
		managarm::posix::ReadlinkAtResponse resp;
		resp.set_error(*error | toPosixProtoError);

		auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
			helix_ng::sendBuffer(nullptr, 0)
		);
		HEL_CHECK(send_resp.error());
		logBragiReply(resp);
	}else{
		auto &target = std::get<std::string>(result);

		logRequest(logRequests || logPaths, self, "READLINKAT", "'{}' -> '{}'",
			path.getPath(self->fsContext()->getRoot()),
			target);

		managarm::posix::ReadlinkAtResponse resp;
		resp.set_error(managarm::posix::Errors::SUCCESS);

		auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
			helix_ng::sendBuffer(target.data(), target.size())
		);
		HEL_CHECK(send_resp.error());
		logBragiReply(resp);
	}
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::RenameAtRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();

	auto tailRes = co_await dispatchTail(req, conversation, preamble);
	if(!tailRes)
		co_return std::unexpected(tailRes.error());
	logBragiRequest(req);

	if(req.flags())
		std::cout << "posix: RENAMEAT flags are ignored" << std::endl;

	ViewPath relative_to;
	smarter::shared_ptr<File, FileHandle> file;

	if (req.fd() == AT_FDCWD) {
		relative_to = self->fsContext()->getWorkingDirectory();
	} else {
		file = self->fileContext()->getFile(req.fd());

		if (!file) {
			co_await sendErrorResponse<managarm::posix::RenameAtResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
			co_return {};
		}

		relative_to = {file->associatedMount(), file->associatedLink()};
	}

	PathResolver resolver;
	resolver.setup(self->fsContext()->getRoot(),
			relative_to, req.path(), self.get());
	auto resolveResult = co_await resolver.resolve(resolveDontFollow);
	if(!resolveResult) {
		co_await sendErrorResponse<managarm::posix::RenameAtResponse>(conversation,
				resolveResult.error() | toPosixError | toPosixProtoError);
		co_return {};
	}

	if (req.newfd() == AT_FDCWD) {
		relative_to = self->fsContext()->getWorkingDirectory();
	} else {
		file = self->fileContext()->getFile(req.newfd());

		if (!file) {
			co_await sendErrorResponse<managarm::posix::RenameAtResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
			co_return {};
		}

		relative_to = {file->associatedMount(), file->associatedLink()};
	}

	// A directory may be renamed onto a trailing-slash path; for any other
	// source the trailing slash is always fatal (ENOTDIR).
	ResolveFlags newFlags = resolvePrefix;
	if(resolver.currentLink()->getTarget()->getType() != VfsType::directory)
		newFlags |= resolveNoTrailingSlash;

	PathResolver new_resolver;
	new_resolver.setup(self->fsContext()->getRoot(),
			relative_to, req.target_path(), self.get());
	auto new_resolveResult = co_await new_resolver.resolve(newFlags);
	if(!new_resolveResult) {
		co_await sendErrorResponse<managarm::posix::RenameAtResponse>(conversation,
				new_resolveResult.error() | toPosixError | toPosixProtoError);
		co_return {};
	}

	logRequest(logRequests || logPaths, self, "RENAMEAT", "'{}' -> '{}{}'",
		ViewPath(resolver.currentView(), resolver.currentLink())
		.getPath(self->fsContext()->getRoot()),
		ViewPath(new_resolver.currentView(), new_resolver.currentLink())
		.getPath(self->fsContext()->getRoot()),
		new_resolver.nextComponent());

	auto superblock = resolver.currentLink()->getTarget()->superblock();
	auto directory = new_resolver.currentLink()->getTarget();
	if(superblock != directory->superblock()) {
		co_await sendErrorResponse<managarm::posix::RenameAtResponse>(conversation, managarm::posix::Errors::CROSS_DEVICE_LINK);
		co_return {};
	}
	auto result = co_await superblock->rename(resolver.currentLink().get(),
			directory.get(), new_resolver.nextComponent());
	if(!result) {
		co_await sendErrorResponse<managarm::posix::RenameAtResponse>(conversation, result.error() | toPosixProtoError);
		co_return {};
	}

	co_await sendErrorResponse<managarm::posix::RenameAtResponse>(conversation, managarm::posix::Errors::SUCCESS);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::UnlinkAtRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();

	auto tailRes = co_await dispatchTail(req, conversation, preamble);
	if(!tailRes)
		co_return std::unexpected(tailRes.error());
	logBragiRequest(req);

	ViewPath relative_to;
	smarter::shared_ptr<File, FileHandle> file;
	std::shared_ptr<FsLink> target_link;

	if(req.flags() & ~AT_REMOVEDIR) {
		std::cout << "posix: UNLINKAT flag handling unimplemented with unknown flag: " << req.flags() << std::endl;
		co_await sendErrorResponse<managarm::posix::UnlinkAtResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
	}

	if(req.fd() == AT_FDCWD) {
		relative_to = self->fsContext()->getWorkingDirectory();
	} else {
		file = self->fileContext()->getFile(req.fd());

		if (!file) {
			co_await sendErrorResponse<managarm::posix::UnlinkAtResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
			co_return {};
		}

		relative_to = {file->associatedMount(), file->associatedLink()};
	}

	PathResolver resolver;
	resolver.setup(self->fsContext()->getRoot(),
			relative_to, req.path(), self.get());

	auto resolveResult = co_await resolver.resolve(resolveDontFollow);
	if(!resolveResult) {
		if(resolveResult.error() == protocols::fs::Error::isDirectory) {
			// TODO: Only when AT_REMOVEDIR is not specified, fix this when flag handling is implemented.
			co_await sendErrorResponse<managarm::posix::UnlinkAtResponse>(conversation, managarm::posix::Errors::IS_DIRECTORY);
			co_return {};
		} else if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
			co_await sendErrorResponse<managarm::posix::UnlinkAtResponse>(conversation, managarm::posix::Errors::FILE_NOT_FOUND);
			co_return {};
		} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
			co_await sendErrorResponse<managarm::posix::UnlinkAtResponse>(conversation, managarm::posix::Errors::NOT_A_DIRECTORY);
			co_return {};
		} else {
			std::cout << "posix: Unexpected failure from resolve()" << std::endl;
			co_return {};
		}
	}

	logRequest(logRequests || logPaths, self, "UNLINKAT", "path='{}'",
		ViewPath(resolver.currentView(), resolver.currentLink())
		.getPath(self->fsContext()->getRoot()));

	target_link = resolver.currentLink();

	auto owner = target_link->getOwner();
	if(!owner) {
		co_await sendErrorResponse<managarm::posix::UnlinkAtResponse>(conversation, managarm::posix::Errors::RESOURCE_IN_USE);
		co_return {};
	}
	if (req.flags() & AT_REMOVEDIR) {
		auto result = co_await owner->rmdir(target_link->getName());
		if(!result) {
			co_await sendErrorResponse<managarm::posix::UnlinkAtResponse>(conversation, result.error() | toPosixProtoError);
			co_return {};
		}
	} else {
		auto result = co_await owner->unlink(target_link->getName());
		if(!result) {
			co_await sendErrorResponse<managarm::posix::UnlinkAtResponse>(conversation, result.error() | toPosixProtoError);
			co_return {};
		}
	}

	co_await sendErrorResponse<managarm::posix::UnlinkAtResponse>(conversation, managarm::posix::Errors::SUCCESS);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::RmdirRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();

	auto tailRes = co_await dispatchTail(req, conversation, preamble);
	if(!tailRes)
		co_return std::unexpected(tailRes.error());
	logBragiRequest(req);

	std::shared_ptr<FsLink> target_link;

	PathResolver resolver;
	resolver.setup(self->fsContext()->getRoot(), self->fsContext()->getWorkingDirectory(),
			req.path(), self.get());

	auto resolveResult = co_await resolver.resolve();
	if(!resolveResult) {
		if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
			co_await sendErrorResponse<managarm::posix::RmdirResponse>(conversation, managarm::posix::Errors::FILE_NOT_FOUND);
			co_return {};
		} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
			co_await sendErrorResponse<managarm::posix::RmdirResponse>(conversation, managarm::posix::Errors::NOT_A_DIRECTORY);
			co_return {};
		} else {
			std::cout << "posix: Unexpected failure from resolve()" << std::endl;
			co_return {};
		}
	}

	logRequest(logRequests || logPaths, self, "RMDIR", "path='{}'",
		ViewPath(resolver.currentView(), resolver.currentLink())
		.getPath(self->fsContext()->getRoot()));

	target_link = resolver.currentLink();

	auto owner = target_link->getOwner();
	auto result = co_await owner->rmdir(target_link->getName());
	if(!result) {
		co_await sendErrorResponse<managarm::posix::RmdirResponse>(conversation, result.error() | toPosixProtoError);
		co_return {};
	}

	co_await sendErrorResponse<managarm::posix::RmdirResponse>(conversation, managarm::posix::Errors::SUCCESS);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::FstatAtRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();

	auto tailRes = co_await dispatchTail(req, conversation, preamble);
	if(!tailRes)
		co_return std::unexpected(tailRes.error());
	logBragiRequest(req);

	logRequest(logRequests, self, "FSTATAT");

	if (req.flags() & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH | AT_NO_AUTOMOUNT | AT_STATX_DONT_SYNC)) {
		std::cout << std::format("posix: unsupported flags {:#x} given to FSTATAT request", req.flags()) << std::endl;
		co_await sendErrorResponse<managarm::posix::FstatAtResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	ViewPath relative_to;
	smarter::shared_ptr<File, FileHandle> file;
	std::shared_ptr<FsLink> target_link;
	std::shared_ptr<MountView> target_mount;

	if (req.fd() == AT_FDCWD) {
		relative_to = self->fsContext()->getWorkingDirectory();
	} else {
		file = self->fileContext()->getFile(req.fd());

		if (!file) {
			co_await sendErrorResponse<managarm::posix::FstatAtResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
			co_return {};
		}

		relative_to = {file->associatedMount(), file->associatedLink()};
	}

	if (req.flags() & AT_EMPTY_PATH) {
		target_link = file->associatedLink();
		target_mount = file->associatedMount();
	} else {
		PathResolver resolver;
		resolver.setup(self->fsContext()->getRoot(),
				relative_to, req.path(), self.get());

		ResolveFlags resolveFlags = 0;
		if (req.flags() & AT_SYMLINK_NOFOLLOW)
		    resolveFlags |= resolveDontFollow;

		auto resolveResult = co_await resolver.resolve(resolveFlags);
		if(!resolveResult) {
			if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
				co_await sendErrorResponse<managarm::posix::FstatAtResponse>(conversation, managarm::posix::Errors::FILE_NOT_FOUND);
				co_return {};
			} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
				co_await sendErrorResponse<managarm::posix::FstatAtResponse>(conversation, managarm::posix::Errors::NOT_A_DIRECTORY);
				co_return {};
			} else {
				std::cout << "posix: Unexpected failure from resolve()" << std::endl;
				co_return {};
			}
		}

		target_mount = resolver.currentView();
		target_link = resolver.currentLink();
	}

	// This catches cases where associatedLink is called on a file, but the file doesn't implement that.
	// Instead of blowing up, return ENOENT.
	if(target_link == nullptr) {
		co_await sendErrorResponse<managarm::posix::FstatAtResponse>(conversation, managarm::posix::Errors::FILE_NOT_FOUND);
		co_return {};
	}

	auto statsResult = co_await target_link->getTarget()->getStats();
	managarm::posix::FstatAtResponse resp;

	if (statsResult) {
		constexpr int statxAttrMask = STATX_ATTR_MOUNT_ROOT;
		int attr = 0;
		if(target_mount && target_link == target_mount->getOrigin())
			attr |= STATX_ATTR_MOUNT_ROOT;
		auto stats = statsResult.value();
		assert((attr & ~statxAttrMask) == 0);

		resp.set_error(managarm::posix::Errors::SUCCESS);

		DeviceId devnum;
		switch(target_link->getTarget()->getType()) {
		case VfsType::regular:
			resp.set_file_type(managarm::posix::FileType::FT_REGULAR);
			break;
		case VfsType::directory:
			resp.set_file_type(managarm::posix::FileType::FT_DIRECTORY);
			break;
		case VfsType::symlink:
			resp.set_file_type(managarm::posix::FileType::FT_SYMLINK);
			break;
		case VfsType::charDevice:
			resp.set_file_type(managarm::posix::FileType::FT_CHAR_DEVICE);
			devnum = target_link->getTarget()->readDevice();
			resp.set_ref_devnum(makedev(devnum.first, devnum.second));
			break;
		case VfsType::blockDevice:
			resp.set_file_type(managarm::posix::FileType::FT_BLOCK_DEVICE);
			devnum = target_link->getTarget()->readDevice();
			resp.set_ref_devnum(makedev(devnum.first, devnum.second));
			break;
		case VfsType::socket:
			resp.set_file_type(managarm::posix::FileType::FT_SOCKET);
			break;
		case VfsType::fifo:
			resp.set_file_type(managarm::posix::FileType::FT_FIFO);
			break;
		default:
			assert(target_link->getTarget()->getType() == VfsType::null);
		}

		if(stats.mode & ~0xFFFu)
			std::cout << "\e[31m" "posix: FsNode::getStats() returned illegal mode of "
					<< stats.mode << "\e[39m" << std::endl;

		resp.set_fs_inode(stats.inodeNumber);
		resp.set_mode(stats.mode);
		resp.set_num_links(stats.numLinks);
		resp.set_uid(stats.uid);
		resp.set_gid(stats.gid);
		resp.set_file_size(stats.fileSize);
		resp.set_atime_secs(stats.atimeSecs);
		resp.set_atime_nanos(stats.atimeNanos);
		resp.set_mtime_secs(stats.mtimeSecs);
		resp.set_mtime_nanos(stats.mtimeNanos);
		resp.set_ctime_secs(stats.ctimeSecs);
		resp.set_ctime_nanos(stats.ctimeNanos);
		resp.set_mount_id(target_mount ? target_mount->mountId() : 0);
		resp.set_stat_dev(target_link->getTarget()->superblock()->deviceNumber());
		resp.set_statx_attr(attr);
		resp.set_statx_attr_mask(statxAttrMask);
	} else {
		resp.set_error(statsResult.error() | toPosixProtoError);
	}

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);

	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::FstatfsRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();

	auto tailRes = co_await dispatchTail(req, conversation, preamble);
	if(!tailRes)
		co_return std::unexpected(tailRes.error());
	logBragiRequest(req);

	logRequest(logRequests, self, "FSTATFS");

	smarter::shared_ptr<File, FileHandle> file;
	std::shared_ptr<FsLink> targetLink;
	managarm::posix::FstatfsResponse resp;

	if(req.fd() >= 0) {
		file = self->fileContext()->getFile(req.fd());

		if (!file) {
			co_await sendErrorResponse<managarm::posix::FstatfsResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
			co_return {};
		}

		targetLink = file->associatedLink();

		// This catches cases where associatedLink is called on a file, but the file doesn't implement that.
		// Instead of blowing up, return ENOENT.
		// TODO: fstatfs can't return ENOENT, verify this is needed
		if(targetLink == nullptr) {
			co_await sendErrorResponse<managarm::posix::FstatfsResponse>(conversation, managarm::posix::Errors::FILE_NOT_FOUND);
			co_return {};
		}
	} else {
		PathResolver resolver;
		resolver.setup(self->fsContext()->getRoot(), self->fsContext()->getWorkingDirectory(),
				req.path(), self.get());
		auto resolveResult = co_await resolver.resolve();
		if(!resolveResult) {
			if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
				co_await sendErrorResponse<managarm::posix::FstatfsResponse>(conversation, managarm::posix::Errors::FILE_NOT_FOUND);
				co_return {};
			} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
				co_await sendErrorResponse<managarm::posix::FstatfsResponse>(conversation, managarm::posix::Errors::NOT_A_DIRECTORY);
				co_return {};
			} else {
				std::cout << "posix: Unexpected failure from resolve()" << std::endl;
				co_return {};
			}
		}

		targetLink = resolver.currentLink();
	}

	auto fsStatsResult = co_await targetLink->getTarget()->superblock()->getFsStats();
	if(!fsStatsResult) {
		co_await sendErrorResponse<managarm::posix::FstatfsResponse>(conversation, fsStatsResult.error() | toPosixProtoError);
		co_return {};
	}
	auto fsStats = fsStatsResult.value();

	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_fstype(fsStats.fsType);
	resp.set_block_size(fsStats.blockSize);
	resp.set_fragment_size(fsStats.fragmentSize);
	resp.set_num_blocks(fsStats.numBlocks);
	resp.set_blocks_free(fsStats.blocksFree);
	resp.set_blocks_free_user(fsStats.blocksFreeUser);
	resp.set_num_inodes(fsStats.numInodes);
	resp.set_inodes_free(fsStats.inodesFree);
	resp.set_inodes_free_user(fsStats.inodesFreeUser);
	resp.set_max_name_length(fsStats.maxNameLength);
	resp.set_fsid0(fsStats.fsid[0]);
	resp.set_fsid1(fsStats.fsid[1]);
	resp.set_flags(fsStats.flags);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);

	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::FchmodAtRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();

	auto tailRes = co_await dispatchTail(req, conversation, preamble);
	if(!tailRes)
		co_return std::unexpected(tailRes.error());
	logBragiRequest(req);

	logRequest(logRequests, self, "FCHMODAT");

	ViewPath relative_to;
	smarter::shared_ptr<File, FileHandle> file;
	std::shared_ptr<FsLink> target_link;

	if(req.fd() == AT_FDCWD) {
		relative_to = self->fsContext()->getWorkingDirectory();
	} else {
		file = self->fileContext()->getFile(req.fd());

		if (!file) {
			co_await sendErrorResponse<managarm::posix::FchmodAtResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
			co_return {};
		}

		relative_to = {file->associatedMount(), file->associatedLink()};
	}

	if(req.flags() & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW)) {
		std::println("posix: unexpected flags {:#x} in fchmodat request", req.flags() & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW));
		co_await sendErrorResponse<managarm::posix::FchmodAtResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	if(req.flags() & AT_EMPTY_PATH) {
		target_link = file->associatedLink();
	} else {
		PathResolver resolver;
		resolver.setup(self->fsContext()->getRoot(),
			relative_to, req.path(), self.get());

		ResolveFlags resolveFlags = 0;
		if(req.flags() & AT_SYMLINK_NOFOLLOW)
			resolveFlags |= resolveDontFollow;

		auto resolveResult = co_await resolver.resolve(resolveFlags);

		if(!resolveResult) {
			if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
				co_await sendErrorResponse<managarm::posix::FchmodAtResponse>(conversation, managarm::posix::Errors::FILE_NOT_FOUND);
				co_return {};
			} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
				co_await sendErrorResponse<managarm::posix::FchmodAtResponse>(conversation, managarm::posix::Errors::NOT_A_DIRECTORY);
				co_return {};
			} else {
				std::cout << "posix: Unexpected failure from resolve()" << std::endl;
				co_return {};
			}
		}

		target_link = resolver.currentLink();
	}

	co_await target_link->getTarget()->chmod(req.mode());

	co_await sendErrorResponse<managarm::posix::FchmodAtResponse>(conversation, managarm::posix::Errors::SUCCESS);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::FchownAtRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();

	auto tailRes = co_await dispatchTail(req, conversation, preamble);
	if(!tailRes)
		co_return std::unexpected(tailRes.error());
	logBragiRequest(req);

	logRequest(logRequests, self, "FCHOWNAT");

	ViewPath relativeTo;
	smarter::shared_ptr<File, FileHandle> file;
	std::shared_ptr<FsLink> targetLink;

	if(req.fd() == AT_FDCWD) {
		relativeTo = self->fsContext()->getWorkingDirectory();
	} else {
		file = self->fileContext()->getFile(req.fd());

		if (!file) {
			co_await sendErrorResponse<managarm::posix::FchownAtResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
			co_return {};
		}

		relativeTo = {file->associatedMount(), file->associatedLink()};
	}

	if(req.flags() & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW)) {
		std::println("posix: unexpected flags {:#x} in fchownat request", req.flags() & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW));
		co_await sendErrorResponse<managarm::posix::FchownAtResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	if(req.flags() & AT_EMPTY_PATH) {
		targetLink = file->associatedLink();
	} else {
		PathResolver resolver;
		resolver.setup(self->fsContext()->getRoot(),
			relativeTo, req.path(), self.get());

		ResolveFlags resolveFlags = 0;
		if(req.flags() & AT_SYMLINK_NOFOLLOW)
			resolveFlags |= resolveDontFollow;

		auto resolveResult = co_await resolver.resolve(resolveFlags);

		if(!resolveResult) {
			co_await sendErrorResponse<managarm::posix::FchownAtResponse>(conversation, resolveResult.error() | toPosixError | toPosixProtoError);
			co_return {};
		}

		targetLink = resolver.currentLink();
	}

	std::optional<uid_t> uid;
	std::optional<gid_t> gid;
	if(req.uid() != -1)
		uid = req.uid();
	if(req.gid() != -1)
		gid = req.gid();

	auto result = co_await targetLink->getTarget()->chown(uid, gid);
	if(!result) {
		co_await sendErrorResponse<managarm::posix::FchownAtResponse>(conversation, result.error() | toPosixProtoError);
		co_return {};
	}

	co_await sendErrorResponse<managarm::posix::FchownAtResponse>(conversation, managarm::posix::Errors::SUCCESS);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::UtimensAtRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();

	auto tailRes = co_await dispatchTail(req, conversation, preamble);
	if(!tailRes)
		co_return std::unexpected(tailRes.error());
	logBragiRequest(req);

	logRequest(logRequests || logPaths, self, "UTIMENSAT");

	ViewPath relativeTo;
	smarter::shared_ptr<File, FileHandle> file;
	std::shared_ptr<FsNode> target = nullptr;

	if(!req.path().size() && (req.flags() & AT_EMPTY_PATH)) {
		target = self->fileContext()->getFile(req.fd())->associatedLink()->getTarget();
	} else {
		if(req.flags() & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) {
			co_await sendErrorResponse<managarm::posix::UtimensAtResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			co_return {};
		}

		ResolveFlags resolveFlags = 0;
		if(req.flags() & AT_SYMLINK_NOFOLLOW) {
			resolveFlags |= resolveDontFollow;
		}

		if(req.fd() == AT_FDCWD) {
			relativeTo = self->fsContext()->getWorkingDirectory();
		} else {
			file = self->fileContext()->getFile(req.fd());
			if (!file) {
				co_await sendErrorResponse<managarm::posix::UtimensAtResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
				co_return {};
			}

			relativeTo = {file->associatedMount(), file->associatedLink()};
		}

		PathResolver resolver;
		resolver.setup(self->fsContext()->getRoot(),
				relativeTo, req.path(), self.get());
		auto resolveResult = co_await resolver.resolve(resolveFlags);
		if(!resolveResult) {
			if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
				co_await sendErrorResponse<managarm::posix::UtimensAtResponse>(conversation, managarm::posix::Errors::FILE_NOT_FOUND);
				co_return {};
			} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
				co_await sendErrorResponse<managarm::posix::UtimensAtResponse>(conversation, managarm::posix::Errors::NOT_A_DIRECTORY);
				co_return {};
			} else {
				std::cout << "posix: Unexpected failure from resolve()" << std::endl;
				co_return {};
			}
		}

		target = resolver.currentLink()->getTarget();
	}

	std::optional<timespec> atime = std::nullopt;
	std::optional<timespec> mtime = std::nullopt;

	auto time = clk::getRealtime();
	if(req.atimeNsec() == UTIME_NOW) {
		atime = {time.tv_sec, time.tv_nsec};
	} else if(req.atimeNsec() != UTIME_OMIT) {
		if(req.atimeNsec() > 999'999'999) {
			co_await sendErrorResponse<managarm::posix::UtimensAtResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			co_return {};
		}
		atime = {static_cast<time_t>(req.atimeSec()), static_cast<long>(req.atimeNsec())};
	}

	if(req.mtimeNsec() == UTIME_NOW) {
		mtime = {time.tv_sec, time.tv_nsec};
	} else if(req.mtimeNsec() != UTIME_OMIT) {
		if(req.mtimeNsec() > 999'999'999) {
			co_await sendErrorResponse<managarm::posix::UtimensAtResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			co_return {};
		}
		mtime = {static_cast<time_t>(req.mtimeSec()), static_cast<long>(req.mtimeNsec())};
	}

	co_await target->utimensat(atime, mtime, time);

	co_await sendErrorResponse<managarm::posix::UtimensAtResponse>(conversation, managarm::posix::Errors::SUCCESS);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::OpenAtRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();

	auto tailRes = co_await dispatchTail(req, conversation, preamble);
	if(!tailRes)
		co_return std::unexpected(tailRes.error());
	logBragiRequest(req);

	if((req.flags() & ~(managarm::posix::OpenFlags::OF_CREATE
			| managarm::posix::OpenFlags::OF_EXCLUSIVE
			| managarm::posix::OpenFlags::OF_NONBLOCK
			| managarm::posix::OpenFlags::OF_CLOEXEC
			| managarm::posix::OpenFlags::OF_TRUNC
			| managarm::posix::OpenFlags::OF_RDONLY
			| managarm::posix::OpenFlags::OF_WRONLY
			| managarm::posix::OpenFlags::OF_RDWR
			| managarm::posix::OpenFlags::OF_PATH
			| managarm::posix::OpenFlags::OF_NOCTTY
			| managarm::posix::OpenFlags::OF_APPEND
			| managarm::posix::OpenFlags::OF_NOFOLLOW
			| managarm::posix::OpenFlags::OF_DIRECTORY))) {
		std::cout << "posix: OPENAT flags not recognized: " << req.flags() << std::endl;
		co_await sendErrorResponse<managarm::posix::OpenAtResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	if (req.path().length() > PATH_MAX) {
		co_await sendErrorResponse<managarm::posix::OpenAtResponse>(conversation, managarm::posix::Errors::NAME_TOO_LONG);
		co_return {};
	}

	SemanticFlags semantic_flags = 0;
	if(req.flags() & managarm::posix::OpenFlags::OF_NONBLOCK)
		semantic_flags |= semanticNonBlock;

	if (req.flags() & managarm::posix::OpenFlags::OF_RDONLY)
		semantic_flags |= semanticRead;
	else if (req.flags() & managarm::posix::OpenFlags::OF_WRONLY)
		semantic_flags |= semanticWrite;
	else if (req.flags() & managarm::posix::OpenFlags::OF_RDWR)
		semantic_flags |= semanticRead | semanticWrite;

	if(req.flags() & managarm::posix::OpenFlags::OF_APPEND)
		semantic_flags |= semanticAppend;

	ViewPath relative_to;
	smarter::shared_ptr<File, FileHandle> file;
	std::shared_ptr<FsLink> target_link;

	if(req.fd() == AT_FDCWD) {
		relative_to = self->fsContext()->getWorkingDirectory();
	} else {
		file = self->fileContext()->getFile(req.fd());

		if (!file) {
			co_await sendErrorResponse<managarm::posix::OpenAtResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
			co_return {};
		}

		relative_to = {file->associatedMount(), file->associatedLink()};
	}

	PathResolver resolver;
	resolver.setup(self->fsContext()->getRoot(),
			relative_to, req.path(), self.get());
	if(req.flags() & managarm::posix::OpenFlags::OF_CREATE) {
		auto resolveResult = co_await resolver.resolve(
				resolvePrefix | resolveOpenCreate);
		if(!resolveResult) {
			co_await sendErrorResponse<managarm::posix::OpenAtResponse>(conversation,
					resolveResult.error() | toPosixError | toPosixProtoError);
			co_return {};
		}

		logRequest(logRequests || logPaths, self, "OPENAT", "create '{}'",
			ViewPath{resolver.currentView(), resolver.currentLink()}
			.getPath(self->fsContext()->getRoot()));

		if (!resolver.hasComponent()) {
			if (semantic_flags & semanticWrite)
				co_await sendErrorResponse<managarm::posix::OpenAtResponse>(conversation, managarm::posix::Errors::IS_DIRECTORY);
			else
				co_await sendErrorResponse<managarm::posix::OpenAtResponse>(conversation, managarm::posix::Errors::ALREADY_EXISTS);
			co_return {};
		}

		auto directory = resolver.currentLink()->getTarget();

		auto linkResult = co_await directory->getLinkOrCreate(self.get(), resolver.nextComponent(),
			req.mode() & ~self->fsContext()->getUmask(), req.flags() & managarm::posix::OpenFlags::OF_EXCLUSIVE);
		if (!linkResult) {
			co_await sendErrorResponse<managarm::posix::OpenAtResponse>(conversation, linkResult.error() | toPosixProtoError);
			co_return {};
		}
		auto link = linkResult.value();
		assert(link);
		auto node = link->getTarget();
		if (node->getType() == VfsType::directory) {
			co_await sendErrorResponse<managarm::posix::OpenAtResponse>(conversation, managarm::posix::Errors::IS_DIRECTORY);
			co_return {};
		}

		auto fileResult = co_await node->open(self.get(), resolver.currentView(), std::move(link),
							semantic_flags);
		assert(fileResult);
		file = fileResult.value();
		assert(file);
	}else{
		ResolveFlags resolveFlags = 0;

		if(req.flags() & managarm::posix::OpenFlags::OF_NOFOLLOW)
			resolveFlags |= resolveDontFollow;

		auto resolveResult = co_await resolver.resolve(resolveFlags);
		if(!resolveResult) {
			if(resolveResult.error() == protocols::fs::Error::isDirectory) {
				// TODO: Verify additional constraints for sending EISDIR.
				co_await sendErrorResponse<managarm::posix::OpenAtResponse>(conversation, managarm::posix::Errors::IS_DIRECTORY);
				co_return {};
			} else if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
				co_await sendErrorResponse<managarm::posix::OpenAtResponse>(conversation, managarm::posix::Errors::FILE_NOT_FOUND);
				co_return {};
			} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
				co_await sendErrorResponse<managarm::posix::OpenAtResponse>(conversation, managarm::posix::Errors::NOT_A_DIRECTORY);
				co_return {};
			} else {
				std::cout << "posix: Unexpected failure from resolve()" << std::endl;
				co_return {};
			}
		}

		logRequest(logRequests || logPaths, self, "OPENAT", "open '{}'",
			ViewPath{resolver.currentView(), resolver.currentLink()}
			.getPath(self->fsContext()->getRoot()));

		auto target = resolver.currentLink()->getTarget();
		if (target->getType() == VfsType::directory && (semantic_flags & semanticWrite)) {
			co_await sendErrorResponse<managarm::posix::OpenAtResponse>(conversation, managarm::posix::Errors::IS_DIRECTORY);
			co_return {};
		}

		if(req.flags() & managarm::posix::OpenFlags::OF_DIRECTORY) {
			if(target->getType() != VfsType::directory) {
				co_await sendErrorResponse<managarm::posix::OpenAtResponse>(conversation, managarm::posix::Errors::NOT_A_DIRECTORY);
				co_return {};
			}
		}

		if(req.flags() & managarm::posix::OpenFlags::OF_PATH) {
			auto dummyFile = smarter::make_shared<DummyFile>(resolver.currentView(), resolver.currentLink());
			dummyFile->setupWeakFile(dummyFile);
			DummyFile::serve(dummyFile);
			file = File::constructHandle(std::move(dummyFile));
		} else {
			// this can only be a symlink if O_NOFOLLOW has been passed
			if(target->getType() == VfsType::symlink) {
				co_await sendErrorResponse<managarm::posix::OpenAtResponse>(conversation, managarm::posix::Errors::SYMBOLIC_LINK_LOOP);
				co_return {};
			}

			auto fileResult = co_await target->open(self.get(), resolver.currentView(), resolver.currentLink(), semantic_flags);
			if(!fileResult) {
				if(fileResult.error() == Error::noBackingDevice) {
					co_await sendErrorResponse<managarm::posix::OpenAtResponse>(conversation, managarm::posix::Errors::NO_BACKING_DEVICE);
					co_return {};
				} else if(fileResult.error() == Error::illegalArguments) {
					co_await sendErrorResponse<managarm::posix::OpenAtResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
					co_return {};
				} else {
					std::cout << "posix: Unexpected failure from open()" << std::endl;
					co_return {};
				}
			}
			assert(fileResult);
			file = fileResult.value();
		}
	}

	if(!file) {
		co_await sendErrorResponse<managarm::posix::OpenAtResponse>(conversation, managarm::posix::Errors::FILE_NOT_FOUND);
		co_return {};
	}

	if(file->isTerminal() &&
		!(req.flags() & managarm::posix::OpenFlags::OF_NOCTTY) &&
        self->pgPointer() &&
		self->pgPointer()->getSession()->getSessionId() == (pid_t)self->pid() &&
		self->pgPointer()->getSession()->getControllingTerminal() == nullptr) {
		// POSIX 1003.1-2017 11.1.3
		auto cts = co_await file->getControllingTerminal();
		if(!cts) {
			std::cout << "posix: Unable to get controlling terminal (" << (int)cts.error() << ")" << std::endl;
		} else {
			cts.value()->assignSessionOf(self.get());
		}
	}

	if(req.flags() & managarm::posix::OpenFlags::OF_TRUNC) {
		auto result = co_await file->truncate(0);
		// Objects that do not support truncation ignore O_TRUNC.
		// TODO: This is better handled by forwarding O_TRUNC in semantic_flags.
		if(!result && result.error() != protocols::fs::Error::illegalOperationTarget) {
			co_await sendErrorResponse<managarm::posix::OpenAtResponse>(conversation, result.error() | toPosixError | toPosixProtoError);
			co_return {};
		}
	}
	auto fd = self->fileContext()->attachFile(file,
			req.flags() & managarm::posix::OpenFlags::OF_CLOEXEC);

	managarm::posix::OpenAtResponse resp;
	if (fd) {
		resp.set_error(managarm::posix::Errors::SUCCESS);
		resp.set_fd(fd.value());
	} else {
		resp.set_error(fd.error() | toPosixProtoError);
	}

	auto [sendResp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
	HEL_CHECK(sendResp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::MknodAtRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();

	auto tailRes = co_await dispatchTail(req, conversation, preamble);
	if(!tailRes)
		co_return std::unexpected(tailRes.error());
	logBragiRequest(req);

	logRequest(logRequests || logPaths, self, "MKNODAT", "path='{}' mode={:o} device={:#x}", req.path(), req.mode(), req.device());

	managarm::posix::MknodAtResponse resp;

	ViewPath relative_to;
	smarter::shared_ptr<File, FileHandle> file;

	if(!req.path().size()) {
		co_await sendErrorResponse<managarm::posix::MknodAtResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}

	if(req.dirfd() == AT_FDCWD) {
		relative_to = self->fsContext()->getWorkingDirectory();
	} else {
		file = self->fileContext()->getFile(req.dirfd());

		if(!file) {
			co_await sendErrorResponse<managarm::posix::MknodAtResponse>(conversation, managarm::posix::Errors::NO_SUCH_FD);
			co_return {};
		}

		relative_to = {file->associatedMount(), file->associatedLink()};
	}

	// mknod never creates a directory, so a trailing slash suppresses creation.
	PathResolver resolver;
	resolver.setup(self->fsContext()->getRoot(),
			relative_to, req.path(), self.get());
	auto resolveResult = co_await resolver.resolve(resolvePrefix | resolveCreatesNonDirectory);
	if(!resolveResult) {
		co_await sendErrorResponse<managarm::posix::MknodAtResponse>(conversation,
				resolveResult.error() | toPosixError | toPosixProtoError);
		co_return {};
	}

	auto parent = resolver.currentLink()->getTarget();
	auto existsResult = co_await parent->getLink(resolver.nextComponent());
	if (existsResult) {
		co_await sendErrorResponse<managarm::posix::MknodAtResponse>(conversation, managarm::posix::Errors::ALREADY_EXISTS);
		co_return {};
	}

	VfsType type;
	DeviceId dev;
	if(S_ISDIR(req.mode())) {
		type = VfsType::directory;
	} else if(S_ISCHR(req.mode())) {
		type = VfsType::charDevice;
	} else if(S_ISBLK(req.mode())) {
		type = VfsType::blockDevice;
	} else if(S_ISREG(req.mode())) {
		type = VfsType::regular;
	} else if(S_ISFIFO(req.mode())) {
		type = VfsType::fifo;
	} else if(S_ISLNK(req.mode())) {
		type = VfsType::symlink;
	} else if(S_ISSOCK(req.mode())) {
		type = VfsType::socket;
	} else {
		type = VfsType::null;
	}

	// TODO: Verify the proper error return here.
	if(type == VfsType::charDevice || type == VfsType::blockDevice) {
		dev.first = major(req.device());
		dev.second = minor(req.device());

		auto result = co_await parent->mkdev(resolver.nextComponent(), type, dev);
		if(!result) {
			if(result.error() == Error::illegalOperationTarget) {
				co_await sendErrorResponse<managarm::posix::MknodAtResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				co_return {};
			} else {
				std::cout << "posix: Unexpected failure from mkdev()" << std::endl;
				co_return {};
			}
		}
	} else if(type == VfsType::fifo) {
		auto result = co_await parent->mkfifo(
			resolver.nextComponent(),
			req.mode() & ~self->fsContext()->getUmask()
		);
		if(!result) {
			if(result.error() == Error::illegalOperationTarget) {
				co_await sendErrorResponse<managarm::posix::MknodAtResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				co_return {};
			} else {
				std::cout << "posix: Unexpected failure from mkfifo()" << std::endl;
				co_return {};
			}
		}
	} else if(type == VfsType::socket) {
		auto result = co_await parent->mksocket(resolver.nextComponent(), (req.mode() & ~self->fsContext()->getUmask()), self->threadGroup()->uid(), self->threadGroup()->gid());
		if(!result) {
			if(result.error() == Error::illegalOperationTarget) {
				co_await sendErrorResponse<managarm::posix::MknodAtResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				co_return {};
			} else {
				std::cout << "posix: Unexpected failure from mksocket()" << std::endl;
				co_return {};
			}
		}
	} else {
		// TODO: Handle regular files.
		std::cout << "\e[31mposix: Creating regular files with mknod is not supported.\e[39m" << std::endl;
		co_await sendErrorResponse<managarm::posix::MknodAtResponse>(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return {};
	}
	resp.set_error(managarm::posix::Errors::SUCCESS);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);

	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::UmaskRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "UMASK", "newmask={:o}", req.newmask());

	managarm::posix::UmaskResponse resp;
	mode_t oldmask = self->fsContext()->setUmask(req.newmask());
	resp.set_oldmask(oldmask);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::GetCwdRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);
	std::string path =
	    self->fsContext()->getWorkingDirectory().getPath(self->fsContext()->getRoot());

	logRequest(logRequests, self, "GETCWD", "path={}", path);

	managarm::posix::GetCwdResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_size(path.size());

	auto [send_resp, send_path] = co_await helix_ng::exchangeMsgs(
	    conversation,
	    helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
	    helix_ng::sendBuffer(
	        path.data(), std::min(static_cast<size_t>(req.size()), path.size() + 1)
	    )
	);
	HEL_CHECK(send_resp.error());
	HEL_CHECK(send_path.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::TtyNameRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);
	logRequest(logRequests, self, "TTY_NAME", "fd={}", req.fd());

	managarm::posix::TtyNameResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	auto file = self->fileContext()->getFile(req.fd());
	if(!file) {
		resp.set_error(managarm::posix::Errors::NO_SUCH_FD);
	} else if(auto ttynameResult = co_await file->ttyname(); !ttynameResult) {
		resp.set_error(ttynameResult.error() | toPosixProtoError);
	} else {
		resp.set_path(ttynameResult.value());
	}

	auto [send_resp, send_tail] = co_await helix_ng::exchangeMsgs(conversation,
		helix_ng::sendBragiHeadTail(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	HEL_CHECK(send_tail.error());
	logBragiReply(resp);
	co_return {};
}

} // namespace requests
