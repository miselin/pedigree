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
        *(.ctors)
        PROVIDE(end_kernel_ctors = .);
        PROVIDE(start_kernel_dtors = .);
        *(.dtors)
        PROVIDE(end_kernel_dtors = .);

        *(.data)
        *(.rodata*)

        . = ALIGN(4096);
        PROVIDE(start_modinfo = .);
        *(.modinfo)
        PROVIDE(end_modinfo = .);

        PROVIDE(start_module_ctors = .);
        *(.module_ctors)
        PROVIDE(end_module_ctors = .);
    }

    . = ALIGN(4096);

    .bss :
    {
        *(.asm.preserve.bss)
        *(.bss*)
        *(.sbss*)
        *(COMMON)
        *(.stacks*)
    }

    PROVIDE(__exidx_start = .);
    .ARM.exidx   : { *(.ARM.exidx* .gnu.linkonce.armexidx.*) }
    PROVIDE(__exidx_end = .);
    
    . = ALIGN(65536); /* Needs to be on a 64 KB boundary to map properly. */

    .ivt :
    {
        *(.ivt)
    }

    /DISCARD/ : { *(.comment) *(.eh_frame) }

    . = ALIGN(4096);

    PROVIDE(kernel_end = .);
}
