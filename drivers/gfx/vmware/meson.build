if host_machine.cpu_family() != 'x86_64'
	subdir_done()
endif

executable('gfx_vmware', 'src/main.cpp',
	dependencies : [ drm_core_dep, svrctl_proto_dep , posix_extra_dep ],
	install : true
)

custom_target('gfx-vmware-server',
	command : [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output : 'gfx-vmware.bin',
	input : 'gfx-vmware.yml',
	install : true,
	install_dir : server
)
