OUTPUT_ARCH("riscv")
OUTPUT_FORMAT("elf64-littleriscv")
ENTRY(_start)
SECTIONS {
	/* R-X segment. */
	. = 0x45000000;

	.text : { *(.text) *(.text.*) }

	/* R-- segment. */
	. = ALIGN(0x1000) + (. & (0xFFF));
	/* For some reason, ld does not emit a read-only segment without an additional gap. */
	. += 0x1000;

	.rodata : { *(.rodata) *(.rodata.*) }
	.note.gnu.build-id : { *(.note.gnu.build-id) }

	/* RW- segment. */
	. = ALIGN(0x1000) + (. & (0xFFF));

	.init_array : {
		PROVIDE_HIDDEN (__init_array_start = .);
		KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
		KEEP (*(.init_array .ctors))
		PROVIDE_HIDDEN (__init_array_end = .);
	}

	.data : { *(.data) *(.data.*) }
	.got : { *(.got) }
	.got.plt : { *(.got.plt) }
	.bss : { *(.bss) *(.bss.*) }
}
