src = [
	'src/main.cpp',
	'src/controller.cpp',
	'src/queue.cpp',
	'src/command.cpp',
	'src/namespace.cpp',
	'src/subsystem.cpp',
	'src/fabric/tcp.cpp',
]

executable('block-nvme', src,
	dependencies : [ libarch, hw_proto_dep, mbus_proto_dep, libblockfs_dep, core_dep, svrctl_proto_dep , posix_extra_dep ],
	install : true
)

custom_target('block-nvme-server',
	command : [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output : 'block-nvme.bin',
	input : 'block-nvme.yml',
	install : true,
	install_dir : server
)
