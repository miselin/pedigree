/* Link.ld -- Linker script for the kernel - ensure everything goes in the */
/*            Correct place.  */
OUTPUT_ARCH(arm)
ENTRY(start)
SECTIONS
{
    . = 0x80100000;

    PROVIDE(kernel_start = .);
    
    .init :
    {
        . = ALIGN(4096);
        PROVIDE(kernel_init = .);
        *(.init.text)
        *(.init.data)

        . = ALIGN(4096);
        *(.asm.bss)
        PROVIDE(kernel_init_end = .);
    }

    . = ALIGN(4096);
    
    .text :
    {
        *(.text*)
    }

    . = ALIGN(4096);

    .data :
    {
        PROVIDE(start_kernel_ctors = .);
        *(.init_array)
        PROVIDE(end_kernel_ctors = .);
        PROVIDE(start_kernel_dtors = .);
        *(.fini_array)
        PROVIDE(end_kernel_dtors = .);

        *(.data)
        *(.rodata*)

        . = ALIGN(4096); /* Needs to be on a 64 KB boundary to map properly. */
        *(.ivt)
    }

    .modinfo : {
        . = ALIGN(4096);
        PROVIDE(start_modinfo = .);
        *(.modinfo*)
        PROVIDE(end_modinfo = .);

        PROVIDE(start_module_ctors = .);
        *(.module_ctors*)
        PROVIDE(end_module_ctors = .);
    }

    PROVIDE(__exidx_start = .);
    .ARM.exidx   : { *(.ARM.exidx* .gnu.linkonce.armexidx.*) }
    PROVIDE(__exidx_end = .);

    . = ALIGN(4096);

    /DISCARD/ : { *(.comment) *(.eh_frame) }

    .bss :
    {
        *(.asm.preserve.bss)
        *(.bss*)
        *(.sbss*)
        *(COMMON)
    }

    . = ALIGN(4096);

    PROVIDE(kernel_end = .);
}
