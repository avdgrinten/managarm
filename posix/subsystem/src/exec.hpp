#pragma once

#include "process.hpp"

struct ExecuteResult {
	helix::UniqueDescriptor thread;
	void *auxBegin = nullptr;
	void *auxEnd = nullptr;
	uid_t effectiveUid = 0;
	gid_t effectiveGid = 0;
	uid_t savedUid = 0;
	gid_t savedGid = 0;
};

async::result<frg::expected<Error, ExecuteResult>>
execute(ViewPath root, ViewPath workdir,
		std::string path,
		std::vector<std::string> args, std::vector<std::string> env,
		std::shared_ptr<VmContext> vm_context, Process *self);
