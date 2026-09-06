#pragma once

#include "device.hpp"
#include "vfs.hpp"

namespace extern_fs {

smarter::shared_ptr<FsLink, LinkRc> createRoot(helix::UniqueLane sb_lane, helix::UniqueLane lane, std::shared_ptr<UnixDevice> device);

smarter::shared_ptr<File, FileHandle>
createFile(helix::UniqueLane lane, std::shared_ptr<MountView> mount, smarter::shared_ptr<FsLink, LinkRc> link);

} // namespace extern_fs
