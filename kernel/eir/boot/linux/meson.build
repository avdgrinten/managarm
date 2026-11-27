if host_machine.cpu_family() not in ['aarch64', 'riscv64']
	subdir_done()
endif

eir_linux_sources = files(
	host_machine.cpu_family() + '.S',
	'entry.cpp',
	'../../generic/elf-relocate.cpp',
)

if host_machine.cpu_family() == 'aarch64'
	eir_linux_sources += '../../generic/generic-platform.cpp'
elif host_machine.cpu_family() == 'riscv64'
	eir_linux_sources += '../../arch/riscv/sbi-platform.cpp'
endif

objcopy = find_program(host_machine.cpu_family() + '-managarm-objcopy')

eir_linux_zstd_lib = static_library('eir-linux-zstd', zstd_decompress_sources,
	include_directories : eir_includes,
	c_args : [eir_c_args, eir_zstd_c_args],
	pic : true,
)

exe = executable('eir-linux', eir_sources, eir_linux_sources,
	include_directories : eir_includes,
	c_args : [eir_c_args, '-DEIR_PIE'], # Assembly is treated as C.
	cpp_args : [eir_cpp_args, '-DEIR_PIE'],
	link_with : eir_linux_zstd_lib,
	link_args : [eir_link_args, '-Wl,-T,' + meson.current_source_dir() + '/link.x', '-static-pie'],
	dependencies : eir_dependencies,
	link_depends : files('link.x'),
	pie : true,
	install : true
)

rawbin = custom_target('eir-linux.bin',
	command : [ objcopy, '-O', 'binary', '@INPUT@', '@OUTPUT@' ],
	input : exe,
	output : 'eir-linux.bin',
	build_by_default : true,
	install : true,
	install_dir : get_option('bindir'),
)
