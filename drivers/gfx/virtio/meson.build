src = [
	'src/main.cpp',
	'src/commands.cpp',
]

executable('gfx_virtio', src,
	dependencies : [ drm_core_dep, virtio_core_dep, svrctl_proto_dep , posix_extra_dep ],
	install : true
)

custom_target('gfx-virtio-server',
	command : [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output : 'gfx-virtio.bin',
	input : 'gfx-virtio.yml',
	install : true,
	install_dir : server
)
