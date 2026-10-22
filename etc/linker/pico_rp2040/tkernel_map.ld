ENTRY(Reset_Handler)

MEMORY {
	ROM (rx)	: ORIGIN = 0x10000000, LENGTH = 2048K
	RAM (xrw)	: ORIGIN = 0x20000000, LENGTH = 256K
}

SECTIONS {
	boot2 : {
		__boot2_start__ = .;
		KEEP (*(.boot2))
		__boot2_end__ = .;
	} > ROM
	ASSERT(__boot2_end__ - __boot2_start__ == 256,
		"ERROR: Pico second stage bootloader must be 256 bytes in size")

	.text : {
		__vector_org = .;
		KEEP (*(.vector))

		*(EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a:) .text*)

		__start = .;
		*(.text)
		*(.text.*)
		*(.rodata)
		*(.rodata.*)
	} >ROM

	.ARM.extab :
	{
		*(.ARM.extab* .gnu.linkonce.armextab.*)
	} > ROM

	__exidx_start = .;
	.ARM.exidx :
	{
		*(.ARM.exidx* .gnu.linkonce.armexidx.*)
	} > ROM
	__exidx_end = .;

	/* Reset_Handler copies .data from this ROM address to RAM a 32-bit word
	   at a time.  ARMv6-M has no unaligned access support, so an unaligned
	   __data_org HardFaults on the first load.  Nothing above forces the
	   location counter to a word boundary, so whether this faults depends
	   on the size of the preceding sections. */
	. = ALIGN(4);
	__data_org = .;
	vector 0x20000000 (NOLOAD) : {
		__vector_start = .;
		*(.data_vector)
		__vector_end = .;
	} >RAM
	.data : AT(__data_org) {
		__data_start = .;
		*(.data)
		*(.data.*)
		. = ALIGN(4);
		__data_end = .;
	} >RAM
	.bss ALIGN(4) (NOLOAD) : {
		__bss_start = .;
		PROVIDE (__noinit_start = .);
		*(.noinit)
		PROVIDE (__noinit_end = .);
		*(.bss)
		*(.bss.*)
		*(COMMON)
		. = ALIGN(4);
		__bss_end = .;
		__end = .;
	} >RAM
}

/* INF2004 memory guard v1: budget only; measure actual stack high water. */
__robot_heap_start = ALIGN(__bss_end, 8);
__robot_heap_limit = ORIGIN(RAM) + LENGTH(RAM) - 2048;
ASSERT(__robot_heap_start + 65536 <= __robot_heap_limit,
       "INF2004: reserve 64 KiB heap below the 2048-byte reset stack")
