objcopy = find_program('riscv64-managarm-objcopy')

exe = executable('eir-d1', eir_sources, 'early-log.cpp',
	include_directories : eir_includes,
	cpp_args : eir_cpp_args,
	link_args : [eir_link_args, '-Wl,-T,' + meson.current_source_dir() + '/link.x'],
	dependencies : eir_dependencies,
	link_depends : files('link.x'),
	install : true
)

flat = custom_target('eir-d1.bin',
	command : [ objcopy, '-O', 'binary', '@INPUT@', '@OUTPUT@' ],
	input : exe,
	output : 'eir-d1.bin'
)

uboot_img = custom_target('eir-d1.uboot',
	command : [
		'mkimage',
		'-d', '@INPUT@',
		'-A', 'riscv',
		'-O', 'Linux',
		'-a', '0x45000000',
		'-e', '0x45000000',
		'-C', 'none',
		'@OUTPUT@'
	],
	input : flat,
	output : 'eir-d1.uboot'
)

fastboot_img = custom_target('eir-d1.fastboot',
	command : [
		'mkbootimg',
		'--kernel', '@INPUT@',
		'--base', '0x45000000',
		'--kernel_offset', '0',
		'-o', '@OUTPUT@'
	],
	input : uboot_img,
	output : 'eir-d1.fastboot',
	install : true,
	install_dir : get_option('bindir')
)
