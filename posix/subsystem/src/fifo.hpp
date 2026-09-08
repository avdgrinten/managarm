#pragma once

#include "file.hpp"
#include "fs.hpp"

namespace fifo {

void createNamedChannel(FsNode *node);
void unlinkNamedChannel(FsNode *node);
async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
openNamedChannel(std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link, FsNode *node, SemanticFlags flags);

std::array<smarter::shared_ptr<File, FileHandle>, 2> createPair(bool nonBlock);

} // namespace fifo

