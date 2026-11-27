if host_machine.cpu_family() not in ['x86_64', 'aarch64', 'riscv64']
	subdir_done()
endif

eir_limine_sources = [
	eir_generic_main,
	'entry.cpp',
]

eir_limine_c_args = eir_c_args
eir_limine_cpp_args = eir_cpp_args
eir_limine_sources += [eir_sources]

if host_machine.cpu_family() == 'x86_64'
	eir_limine_c_args += ['-mcmodel=kernel']
	eir_limine_cpp_args += ['-mcmodel=kernel']
elif host_machine.cpu_family() == 'aarch64'
	eir_limine_sources += '../../generic/generic-platform.cpp'
elif host_machine.cpu_family() == 'riscv64'
	eir_limine_sources += '../../arch/riscv/sbi-platform.cpp'
endif

eir_limine_zstd_lib = static_library('eir-limine-zstd', zstd_decompress_sources,
	include_directories: [eir_includes],
	dependencies: [freestnd_c_hdrs_dep],
	c_args: [eir_limine_c_args, eir_zstd_c_args],
	pic: false,
)

executable(
	'eir-limine',
	eir_limine_sources,
	include_directories: [eir_includes],
	dependencies: [freestnd_cxx_hdrs_dep, eir_dependencies],
	c_args: eir_limine_c_args,
	cpp_args: eir_limine_cpp_args,
	link_with: eir_limine_zstd_lib,
	link_args: [eir_link_args, '-Wl,-T,' + meson.current_source_dir() + '/limine.x'],
	link_depends: files('limine.x'),
	install: true,
)

install_data(eir_features, install_dir: eir_features_dir, rename: 'eir-limine.json')
