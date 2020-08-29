
SECTIONS
{
    . = ALIGN(4096);

    .cdtors :
    {
        PROVIDE(start_kernel_ctors = .);
        *(.ctors)
        *(.init_array*)
        PROVIDE(end_kernel_ctors = .);

        PROVIDE(start_kernel_dtors = .);
        *(.dtors)
        *(.fini_array*)
        PROVIDE(end_kernel_dtors = .);
    }

    .modinfo :
    {
        PROVIDE(start_modinfo = .);
        KEEP(*(.modinfo*));
        PROVIDE(end_modinfo = .);

        PROVIDE(start_module_ctors = .);
        *(.module_ctors)
        PROVIDE(end_module_ctors = .);
    }
}

INSERT AFTER .rodata;
