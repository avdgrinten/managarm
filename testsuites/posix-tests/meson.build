src = [
	'src/main.cpp',
	'src/badfd.cpp',
	'src/epoll.cpp',
	'src/faults.cpp',
	'src/inotify.cpp',
	'src/parent-dead-signal.cpp',
	'src/pipes.cpp',
	'src/processgroups.cpp',
	'src/signal.cpp',
	'src/signalfd.cpp',
	'src/stat.cpp',
	'src/sync.cpp',
	'src/unixnames.cpp',
	'src/sigaltstack.cpp',
	'src/mmap.cpp',
	'src/memfd.cpp',
	'src/pidfd.cpp',
	'src/socket.cpp',
	'src/timerfd.cpp',
	'src/posix-timers.cpp',
	'src/eventfd.cpp',
	'src/pty.cpp',
	'src/procfs.cpp',
	'src/path-resolution.cpp',
	'src/path-errors.cpp',
	'src/rename.cpp',
	'src/trailing-slash-create.cpp',
	'src/pthread-fork-wait.cpp',
	'src/thread.cpp',
	'src/poll.cpp',
	'src/segfault.cpp',
	'src/pthread-timeouts.cpp',
	'src/split-mappings.cpp',
	'src/uid-gid.cpp',
]

deps = [cli11_dep, frigg]

# These tests inspect hel handles.
if host_machine.system() == 'managarm'
	src += 'src/client-handles.cpp'
	deps += [hel_dep, posix_extra_dep]
endif

executable('posix-tests', src, dependencies: deps, install : true)
