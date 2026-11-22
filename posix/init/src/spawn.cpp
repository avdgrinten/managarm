#include <algorithm>
#include <errno.h>
#include <format>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "spawn.hpp"

namespace {

std::string serverDescription(std::string_view name) {
	return std::format("/usr/lib/managarm/server/{}.bin", name);
}

} // namespace

std::expected<Process, int>
Process::spawn(std::span<const std::string> args, std::span<const std::string> env) {
	std::vector<char *> argv;
	for(auto &arg : args)
		argv.push_back(const_cast<char *>(arg.c_str()));
	argv.push_back(nullptr);

	auto name = [] (std::string_view variable) { return variable.substr(0, variable.find('=')); };
	auto overridden = [&] (std::string_view variable) {
		return std::ranges::any_of(env, [&] (const std::string &e) {
			return name(e) == name(variable);
		});
	};

	std::vector<char *> envp;
	for(char **e = environ; *e; e++) {
		if(!overridden(*e))
			envp.push_back(*e);
	}
	for(auto &e : env)
		envp.push_back(const_cast<char *>(e.c_str()));
	envp.push_back(nullptr);

	// Unlike fork() + exec(), this also reports failure to execute the program.
	pid_t pid;
	if(int e = posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), envp.data()); e)
		return std::unexpected{e};
	return Process{pid};
}

std::expected<int, int> Process::wait() {
	while(true) {
		int status;
		if(waitpid(pid_, &status, 0) >= 0)
			return status;
		if(errno != EINTR)
			return std::unexpected{errno};
	}
}

std::expected<void, int> runServer(std::string_view name) {
	std::string args[] = {"/usr/bin/runsvr", "run", serverDescription(name)};
	auto process = Process::spawn(args, {});
	if(!process)
		return std::unexpected{process.error()};
	return {};
}

std::expected<void, int> bindServer(std::string_view name, std::string_view mbusId) {
	std::string args[] = {"/usr/bin/runsvr", "--fork", "bind", serverDescription(name)};
	std::string env[] = {std::format("MBUS_ID={}", mbusId)};
	auto process = Process::spawn(args, env);
	if(!process)
		return std::unexpected{process.error()};
	return {};
}
