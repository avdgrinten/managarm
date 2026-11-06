#include <fcntl.h>
#include <format>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

#include <async/oneshot-event.hpp>
#include <bragi/helpers-std.hpp>
#include <helix/memory.hpp>
#include <protocols/mbus/client.hpp>
#include <svrctl.bragi.hpp>

#include <CLI/CLI.hpp>

// ----------------------------------------------------------------------------
// I/O functions
// ----------------------------------------------------------------------------

int heloutFd;

template <typename ...Ts>
__attribute__((format(printf, 2, 0)))
void print_to(int fd, const char *format, va_list args) {
	vdprintf(heloutFd, format, args);
	vdprintf(fd, format, args);
}

template <typename ...Ts>
void log(const char *format, ...) {
	va_list args;
	va_start(args, format);
	print_to(STDOUT_FILENO, format, args);
	va_end(args);
}

template <typename ...Ts>
void err(const char *format, ...) {
	va_list args;
	va_start(args, format);
	print_to(STDERR_FILENO, format, args);
	va_end(args);
}

static std::vector<std::byte> readEntireFile(const char *path) {
	constexpr size_t bytesPerChunk = 8192;

	auto fd = open(path, O_RDONLY);
	if(fd < 0)
		throw std::runtime_error(std::format("Could not open file '{}'", path));

	std::vector<std::byte> buffer;

	struct stat st;
	if(!fstat(fd, &st)) {
		buffer.reserve(st.st_size);
	}else{
		log("runsvr: fstat() failed on %s\n", path);
	}

	off_t progress = 0;
	while(true) {
		buffer.resize(progress + bytesPerChunk);
		auto chunk = read(fd, buffer.data() + progress, bytesPerChunk);
		if(!chunk)
			break;
		if(chunk < 0)
			throw std::runtime_error("Error while reading file");
		progress += chunk;
	}

	close(fd);

	buffer.resize(progress);
	return buffer;
}

// ----------------------------------------------------------------------------
// svrctl handling.
// ----------------------------------------------------------------------------

helix::UniqueLane svrctlLane;
async::oneshot_event foundSvrctl;

async::result<void> enumerateSvrctl() {
	auto filter = mbus_ng::Conjunction({
		mbus_ng::EqualsFilter("class", "svrctl")
	});

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	auto [_, events] = (co_await enumerator.nextEvents()).unwrap();
	assert(events.size() == 1);

	auto entity = co_await mbus_ng::Instance::global().getEntity(events[0].id);
	svrctlLane = (co_await entity.getRemoteLane()).unwrap();
}

async::result<helix::UniqueLane> runServer(const managarm::svrctl::Description &desc) {
	managarm::svrctl::RunServerRequest req;
	req.set_description(desc);

	auto [offer, send_req, send_tail, recv_resp, pull_server] = co_await helix_ng::exchangeMsgs(
		svrctlLane,
		helix_ng::offer(
			helix_ng::sendBragiHeadTail(req, frg::stl_allocator{}),
			helix_ng::recvInline(),
			helix_ng::pullDescriptor(kHelRightInvoke))
	);
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(send_tail.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(pull_server.error());

	auto resp = bragi::parse_head_only<managarm::svrctl::RunServerResponse>(recv_resp);
	recv_resp.reset();
	assert(resp);
	assert(resp->error() == managarm::svrctl::Errors::SUCCESS);

	co_return pull_server.descriptor();
}

async::result<void> uploadFile(const char *name) {
	managarm::svrctl::FileUploadRequest req;
	req.set_name(name);
	req.set_with_data(false); // Avoid reading file data at first

	auto [offer, send_req, send_tail, recv_resp] = co_await helix_ng::exchangeMsgs(
		svrctlLane,
		helix_ng::offer(
			helix_ng::sendBragiHeadTail(req, frg::stl_allocator{}),
			helix_ng::recvInline())
	);
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(send_tail.error());
	HEL_CHECK(recv_resp.error());

	auto resp = bragi::parse_head_only<managarm::svrctl::FileUploadResponse>(recv_resp);
	recv_resp.reset();
	assert(resp);

	if(resp->error() == managarm::svrctl::Errors::DATA_REQUIRED) {
		// The kernel does not know the file, we have to read its contents.
		req.set_with_data(true);

		auto buffer = readEntireFile(name);
		auto [offer, send_req, send_tail, send_data, recv_resp] = co_await helix_ng::exchangeMsgs(
			svrctlLane,
			helix_ng::offer(
				helix_ng::sendBragiHeadTail(req, frg::stl_allocator{}),
				helix_ng::sendBuffer(buffer.data(), buffer.size()),
				helix_ng::recvInline())
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(send_tail.error());
		HEL_CHECK(send_data.error());
		HEL_CHECK(recv_resp.error());

		resp = bragi::parse_head_only<managarm::svrctl::FileUploadResponse>(recv_resp);
		recv_resp.reset();
		assert(resp);
	}

	assert(resp->error() == managarm::svrctl::Errors::SUCCESS);
}

async::result<int> bindServer(helix::UniqueLane &lane, int mbusId) {
	managarm::svrctl::DeviceBindRequest req;
	req.set_mbus_id(mbusId);

	auto [offer, send_req, recv_resp] = co_await helix_ng::exchangeMsgs(
		lane,
		helix_ng::offer(
			helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
			helix_ng::recvInline())
	);
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());

	auto resp = bragi::parse_head_only<managarm::svrctl::DeviceBindResponse>(recv_resp);
	recv_resp.reset();
	assert(resp);

	if(resp->error() == managarm::svrctl::Errors::SUCCESS)
		co_return 0;
	else
		co_return 1;
}

// ----------------------------------------------------------------
// Freestanding mbus functions.
// ----------------------------------------------------------------

enum class action {
	run, bind, upload
};

async::result<int> asyncMain(action act, std::string path) {
	co_await enumerateSvrctl();

	switch (act) {
		case action::run: {
			auto buffer = readEntireFile(path.c_str());

			managarm::svrctl::Description desc;
			bragi::limited_reader rd{buffer.data(), buffer.size()};
			auto deser = bragi::deserializer{};
			desc.decode_body(rd, deser);

			log("runsvr: Running %s\n", desc.name().c_str());

			for(auto &file : desc.files())
				co_await uploadFile(file.path().c_str());

			co_await runServer(desc);

			break;
		}

		case action::bind: {
			auto buffer = readEntireFile(path.c_str());

			managarm::svrctl::Description desc;
			bragi::limited_reader rd{buffer.data(), buffer.size()};
			auto deser = bragi::deserializer{};
			desc.decode_body(rd, deser);

			auto id_str = getenv("MBUS_ID");
			log("runsvr: Binding driver %s to mbus ID %s\n", desc.name().c_str(), id_str);

			for(auto &file : desc.files())
				co_await uploadFile(file.path().c_str());

			auto lane = co_await runServer(desc);
			co_await bindServer(lane, std::stoi(id_str));

			break;
		}

		case action::upload: {
			log("runsvr: Uploading %s\n", path.c_str());
			co_await uploadFile(path.c_str());

			break;
		}

		default: {
			err("runsvr: Invalid action (this should be unreachable)\n");
			abort();
		}
	}

	co_return 0;
}

int main(int argc, const char **argv) {
	heloutFd = open("/dev/helout", O_RDWR);

	bool do_fork = false;
	std::string path;
	action act;

	CLI::App app{"runsvr"};
	app.add_flag("-f,--fork", do_fork, "Fork off before continuing");

	CLI::App *sub_run = app.add_subcommand("run", "Run a server (used in conjunction with bind)");
	sub_run->add_option("path", path, "Path to description")->required();

	CLI::App *sub_bind = app.add_subcommand("bind", "Bind an mbus ID to a server");
	sub_bind->add_option("path", path, "Path to description")->required();

	CLI::App *sub_upload = app.add_subcommand("upload", "Upload a file");
	sub_upload->add_option("path", path, "Path to file")->required();

	app.require_subcommand(1);

	CLI11_PARSE(app, argc, argv);

	if (*sub_run)
		act = action::run;
	else if (*sub_bind)
		act = action::bind;
	else if (*sub_upload)
		act = action::upload;
	else {
		err("runsvr: No subcommand specified\n");
		return 1;
	}

	if (do_fork) {
		auto pid = fork();
		if (pid < 0) {
			err("runsvr: Failed to fork: %s\n", strerror(errno));
			return 2;
		} else if (pid) {
			log("runsvr: Forking off to %d\n", pid);
			return 0;
		}
		mbus_ng::recreateInstance();
	}

	return async::run(asyncMain(act, std::move(path)), helix::currentDispatcher);
}
