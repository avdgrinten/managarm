executable('vsockserver', [ 'src/main.cpp', 'src/device.cpp', 'src/socket.cpp', 'src/stack.cpp' ],
	dependencies : [ fs_proto_dep, mbus_proto_dep, svrctl_proto_dep, virtio_core_dep, core_dep ],
	install : true
)

custom_target('vsockserver-server',
	command : [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output : 'vsockserver.bin',
	input : 'vsockserver.yml',
	install : true,
	install_dir : server
)
