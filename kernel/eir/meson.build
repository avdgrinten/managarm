fs = import('fs')

eir_generic_sources = files(
	'../common/libc.cpp',
	'../common/font-8x16.cpp',
	'../common/uart/ns16550.cpp',
	'../common/uart/pl011.cpp',
	'../common/uart/samsung.cpp',
	'generic/cmdline.cpp',
	'generic/log-ring.cpp',
	'generic/main.cpp',
	'generic/memory-layout.cpp',
	'generic/debug.cpp',
	'generic/framebuffer.cpp',
	'generic/initrd.cpp',
	'system/acpi/acpi.cpp',
	'system/acpi/console.cpp',
	'system/acpi/cpu-count.cpp',
	'system/acpi/glue.cpp',
	'system/dtb/cpu-count.cpp',
	'system/dtb/discovery.cpp',
	'system/uart/uart.cpp',
)

eir_generic_sources += files('generic/global-constructors.cpp')

eir_generic_main = files(
	'generic/main-function.cpp'
)

eir_includes = [include_directories(
	'../klibc',
	'../common',
	'generic',
	'system/acpi',
	'system/dtb',
	'system/uart',
)]

eir_c_args = []
eir_cpp_args = ['-fno-threadsafe-statics', '-DFRG_DONT_USE_LONG_DOUBLE']
eir_link_args = ['-nostdlib']
eir_linker_script = files()
eir_dependencies = [frigg, libarch_base]

if not build_uefi
	cpp = meson.get_compiler('cpp')
	cpp_link_args = get_option('cpp_link_args')
	libgcc_path = run_command(cxx.cmd_array(), cpp_link_args, '-print-libgcc-file-name',
					capture: true, check: true).stdout().strip()

	message(libgcc_path)
	if not fs.is_file(libgcc_path)
		error('libgcc/compiler-rt library (' + libgcc_path + ') is missing')
	endif

	libgcc = declare_dependency(link_args: libgcc_path)
	eir_dependencies += [libgcc]
endif

# Use uACPI in barebones (= table only mode).
eir_c_args += ['-DUACPI_BAREBONES_MODE', '-DUACPI_OVERRIDE_LIBC']
eir_cpp_args += ['-DUACPI_BAREBONES_MODE', '-DUACPI_OVERRIDE_LIBC']

if get_option('kernel_log_allocations')
	eir_cpp_args += '-DKERNEL_LOG_ALLOCATIONS'
endif

# TODO: Using a static_library() would be nicer here but it is not possible
#       since we need to compile it both into 32-bit and 64-bit Multiboot2 code.
eir_generic_sources += uacpi_sources
eir_includes += uacpi_includes

eir_includes += zstd_includes
eir_zstd_c_args = [
	'-include', meson.current_source_dir() / 'generic/zstd-config.h',
	'-DZSTD_DISABLE_ASM',
	'-DZSTD_NO_INTRINSICS',
	'-DZSTD_LEGACY_SUPPORT=0',
	'-DZSTD_TRACE=0',
	'-DDYNAMIC_BMI2=0',
	'-DXXH_NO_STDLIB',
]

# Installed as share/eir/<binary>.json for each eir binary.
# update-boot.py reads it to decide how it may compress
# the initrd, since it can run against a sysroot with an older eir.
eir_features = files('features.json')
eir_features_dir = get_option('datadir') / 'eir'

if arch == 'aarch64'
	subdir('arch/arm')
elif arch == 'riscv64'
	subdir('arch/riscv')
elif arch == 'x86_64'
	subdir('arch/x86')
endif

if build_uefi
	subdir('boot/uefi')
else
	subdir('boot/limine')
	subdir('boot/linux')
	subdir('boot/multiboot2')
endif
