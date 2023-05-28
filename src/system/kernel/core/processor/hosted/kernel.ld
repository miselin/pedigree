
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

        PROVIDE(start_module_dtors = .);
        *(.module_dtors)
        PROVIDE(end_module_dtors = .);
    }
}

INSERT AFTER .rodata;
