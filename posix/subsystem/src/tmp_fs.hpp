#pragma once

#include "vfs.hpp"

namespace tmp_fs {

smarter::shared_ptr<FsNode> createMemoryNode(std::string path);

std::expected<smarter::shared_ptr<FsLink, LinkRc>, Error> createRoot(Process *p, std::string options);
smarter::shared_ptr<FsLink, LinkRc> createDevTmpFsRoot();

} // namespace tmp_fs
