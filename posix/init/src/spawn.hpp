#pragma once

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>

// Handle to a child process; the child is only reaped by wait().
// Errors are reported as errno values.
class Process {
public:
	// Runs the program args[0]. The variables in env are passed in addition to our own
	// environment and take precedence over it.
	static std::expected<Process, int>
	spawn(std::span<const std::string> args, std::span<const std::string> env);

	Process(Process &&) = default;
	Process &operator=(Process &&) = default;

	// Blocks until the process terminates and returns its wait status.
	std::expected<int, int> wait();

private:
	explicit Process(pid_t pid) : pid_{pid} {}

	pid_t pid_;
};

// Launches a server from its server description.
std::expected<void, int> runServer(std::string_view name);

// Launches a server and binds it to the device with the given mbus ID.
std::expected<void, int> bindServer(std::string_view name, std::string_view mbusId);
