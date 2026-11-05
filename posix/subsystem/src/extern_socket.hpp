#include "file.hpp"

namespace extern_socket {
async::result<std::expected<smarter::shared_ptr<File, FileHandle>, Error>> createSocket(helix::BorrowedLane lane,
		int domain, int type, int proto, int flags);
}
