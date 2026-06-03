src = [
	'src/main.cpp',
	'src/controller.cpp',
	'src/kernel_api.cpp',
]

executable('snd-hda', src,
	dependencies : [ libarch, hw_proto_dep, libsound_dep, uhda_dep , posix_extra_dep ],
	install : true
)

custom_target('snd-hda-server',
	command : [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output : 'snd-hda.bin',
	input : 'sound-hda.yml',
	install : true,
	install_dir : server
)
