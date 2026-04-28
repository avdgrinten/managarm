if build_uefi
	subdir_done()
endif

fs = import('fs')

link_args = [ '-nostdlib' ]

args = [
	'-DLIBASYNC_CUSTOM_PLATFORM',
	'-DFRG_DONT_USE_LONG_DOUBLE',
	'-fno-threadsafe-statics'
]
c_args = [ ]
cpp_args = [ ]

cxx = meson.get_compiler('cpp')
cxx_link_args = get_option('cpp_link_args')

libgcc_path = run_command(cxx.cmd_array(), cxx_link_args, '-print-libgcc-file-name',
				capture: true, check: true).stdout().strip()

if not fs.is_file(libgcc_path)
	error('libgcc/compiler-rt library (' + libgcc_path + ') is missing')
endif

libgcc = declare_dependency(link_args: libgcc_path)

link_depends = []
inc = [ cralgo_includes ]
link_with = []
want_acpi = false
want_dtb = false

src = files(
	'../common/libc.cpp',
	'../common/font-8x16.cpp',
	'../common/uart/ns16550.cpp',
	'../common/uart/pl011.cpp',
	'../common/uart/samsung.cpp',
	'generic/address-space.cpp',
	'generic/cancel.cpp',
	'generic/credentials.cpp',
	'generic/debug.cpp',
	'generic/event.cpp',
	'generic/fiber.cpp',
	'generic/gdbserver.cpp',
	'generic/hel.cpp',
	'generic/hierarchy.cpp',
	'generic/int-call.cpp',
	'generic/irq.cpp',
	'generic/io.cpp',
	'generic/ipc-queue.cpp',
	'generic/ipl.cpp',
	'generic/kasan.cpp',
	'generic/kerncfg.cpp',
	'generic/kernlet.cpp',
	'generic/kernel-heap.cpp',
	'generic/kernel-io.cpp',
	'generic/kernel-log.cpp',
	'generic/kernel-stack.cpp',
	'generic/load-balancing.cpp',
	'generic/main.cpp',
	'generic/mbus.cpp',
	'generic/memory-view.cpp',
	'generic/ostrace.cpp',
	'generic/pfn-db.cpp',
	'generic/physical.cpp',
	'generic/profile.cpp',
	'generic/random.cpp',
	'generic/rcu.cpp',
	'generic/service.cpp',
	'generic/schedule.cpp',
	'generic/stream.cpp',
	'generic/thread.cpp',
	'generic/timer.cpp',
	'generic/traps.cpp',
	'generic/servers.cpp',
	'generic/ubsan.cpp',
	'generic/universe.cpp',
	'generic/work-queue.cpp',
	'generic/asid.cpp',
	'generic/cpu-data.cpp',
	'system/framebuffer/boot-screen.cpp',
	'system/framebuffer/fb.cpp',
	'system/pci/dmalog.cpp',
	'system/pci/pci_discover.cpp',
	'system/pci/pci_legacy.cpp',
	'system/pci/pci_quirks.cpp',
	'system/pci/pci_quirks_rpi4.cpp',
	'system/pci/pcie_ecam.cpp',
	'system/pci/pcie_brcmstb.cpp',
	'system/smbios/smbios3.cpp'
)

src += frgbragi.process([
	protos/'fs/fs.bragi',
	protos/'hw/hw.bragi',
	protos/'mbus/mbus.bragi',
	protos/'ostrace/ostrace.bragi',
	protos/'posix/posix.bragi',
	protos/'clock/clock.bragi',
	protos/'kernlet/kernlet.bragi',
	protos/'kerncfg/kerncfg.bragi',
	protos/'svrctl/svrctl.bragi'
])

klibc_inc = include_directories('../klibc')

inc += klibc_inc
inc += include_directories(
	'generic',
	'system/acpi',
	'system/dtb',
	'system/framebuffer',
	'system/legacy-pc',
	'system/pci',
	'system/smbios',
	'../common',
	'../../protocols/posix/include',
)

# For now, we only use static initializers for initgraph stages.
# If we decide to use them for something more complicated, we have to remove this.

if ubsan
	# -fsanitize=function requires RTTI.
	args += [ '-fsanitize=undefined', '-fno-sanitize=function' ]
endif

if kasan
	# The shadow memory offset is calculated as:
	#     ShadowMemoryBase - (StartOfHigherHalf/ShadowScale).
	# This ensures that the first shadow byte of the higher half ends up at SharedMemoryBase.
	# Lower half addresses become non-canonical by design
	# (and KASAN should not instrument lower-half access anyway).
	args += [
		'-DTHOR_KASAN',
		'-fsanitize=kernel-address',
		'-mllvm', '-asan-mapping-offset=0xdfffe00000000000',
		'-mllvm', '-asan-globals=false',
	]
endif

if log_alloc
	args += [ '-fno-omit-frame-pointer', '-DKERNEL_LOG_ALLOCATIONS', '-DTHOR_HAS_FRAME_POINTERS' ]
endif

if frame_pointers
	args += [ '-fno-omit-frame-pointer', '-DTHOR_HAS_FRAME_POINTERS' ]
endif

if arch == 'aarch64'
	subdir('arch/arm')
elif arch == 'riscv64'
	subdir('arch/riscv')
elif arch == 'x86_64'
	subdir('arch/x86')
endif

if want_acpi
	src += files(
		'system/acpi/acpi.cpp',
		'system/acpi/glue.cpp',
		'system/acpi/madt.cpp',
		'system/acpi/ec.cpp',
		'system/acpi/pm-interface.cpp',
		'system/acpi/battery.cpp',
		'system/acpi/ps2.cpp',
		'system/pci/pci_acpi.cpp'
	)

	args += [ '-DTHOR_HAS_ACPI_SUPPORT=1' ]

	uacpi_args = [ '-DUACPI_OVERRIDE_LIBC' ]

	uacpi_lib = static_library('uacpi', uacpi_sources,
		include_directories : [uacpi_includes, 'system/acpi'],
		c_args : c_args + args,
		pic : false
	)

	inc += uacpi_includes
	link_with += uacpi_lib
endif

if want_dtb
	src += files(
		'system/dtb/dtb.cpp',
		'system/dtb/dtb_discover.cpp',
		'system/pci/pci_dtb.cpp'
	)

	args += [ '-DTHOR_HAS_DTB_SUPPORT=1' ]
endif

link_with += static_library('cralgo', cralgo_sources,
	include_directories : [klibc_inc, cralgo_includes],
	cpp_args : cpp_args + args,
	pic : false
)

deps = [ libgcc, frigg, libsmarter, bragi_dep, libasync_dep, libarch_base, hel_dep ]

link_script = meson.get_compiler('cpp').preprocess(
	'link.x',
	compile_args : cpp_args + args + [
		# Work around a Meson bug that passes -c to the preprocessor here.
		'-Wno-error=unused-command-line-argument'
	],
	include_directories : inc,
)[0]

link_args += [ '-Wl,-T,' + link_script.full_path() ]
link_depends += link_script

executable('thor', src,
	include_directories : inc,
	dependencies : deps,
	cpp_args : cpp_args + args,
	link_with : link_with,
	link_args : link_args,
	link_depends : link_depends,
	install : true
)
