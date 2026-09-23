ENTRY(arm64_start)

PHDRS
{
  boot PT_LOAD FLAGS(7);
  kernel PT_LOAD FLAGS(7);
}

SECTIONS
{
  . = 0x40200000;
  PROVIDE(kernel_physical_start = .);

  .boot.text : {
    *(.boot.text*)
  } :boot

  . = ALIGN(4096);
  .boot.bss (NOLOAD) : {
    PROVIDE(arm64_boot_bss_start = .);
    *(.boot.bss*)
    PROVIDE(arm64_boot_bss_end = .);
  } :boot

  . = ALIGN(4096);
  . += 0xffff000000000000;
  PROVIDE(kernel_start = .);

  .text : AT(ADDR(.text) - 0xffff000000000000) {
    *(.text*)
    *(.init.text*)
  } :kernel

  . = ALIGN(4096);
  .rodata : AT(ADDR(.rodata) - 0xffff000000000000) {
    *(.rodata*)
  } :kernel

  . = ALIGN(4096);
  .data : AT(ADDR(.data) - 0xffff000000000000) {
    PROVIDE(start_kernel_ctors = .);
    KEEP(*(.ctors))
    KEEP(*(.preinit_array*))
    KEEP(*(.init_array*))
    PROVIDE(end_kernel_ctors = .);
    PROVIDE(start_kernel_dtors = .);
    KEEP(*(.dtors))
    KEEP(*(.fini_array*))
    PROVIDE(end_kernel_dtors = .);
    *(.data*)
    *(.init.data*)

    . = ALIGN(4096);
    PROVIDE(start_modinfo = .);
    *(.modinfo)
    PROVIDE(end_modinfo = .);
    PROVIDE(start_module_ctors = .);
    *(.module_ctors)
    PROVIDE(end_module_ctors = .);
    PROVIDE(start_module_dtors = .);
    *(.module_dtors)
    PROVIDE(end_module_dtors = .);
  } :kernel

  . = ALIGN(4096);
  .bss (NOLOAD) : AT(ADDR(.bss) - 0xffff000000000000) {
    PROVIDE(arm64_bss_start = .);
    *(.bss*)
    *(.init.bss*)
    *(COMMON)
    PROVIDE(arm64_bss_end = .);
  } :kernel

  . = ALIGN(4096);
  PROVIDE(kernel_end = .);
  PROVIDE(kernel_physical_end = . - 0xffff000000000000);

  /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note.GNU-stack) }
}
