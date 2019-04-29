/* Link.ld -- Linker script for the bootloader - ensure everything goes in the */
/*            Correct place.  */
OUTPUT_ARCH(arm)
ENTRY(start)
SECTIONS
{
    . = 0x87000000;

    .text :
    {
        *(.init)
        *(.text*)
    }

    . = ALIGN(4096);

    .data :
    {
        start_ctors = .;
        *(.init_array)
        end_ctors = .;
        start_dtors = .;
        *(.fini_array)
        end_dtors = .;

        *(.data*)
        *(.rodata*)
	    *(.strtab*)
    }

    . = ALIGN(4096);

    .bss :
    {
        *(.bss*)
        *(.sbss*)
        *(COMMON)
    }

    /DISCARD/ : { *(.comment) *(.eh_frame) }

    . = ALIGN(4096);
}
