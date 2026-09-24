ENTRY(armv7_start)

PHDRS
{
  boot_text PT_LOAD FLAGS(5);
  boot_data PT_LOAD FLAGS(6);
  kernel_text PT_LOAD FLAGS(5);
  kernel_data PT_LOAD FLAGS(6);
}

SECTIONS
{
  . = 0x40200000;
  PROVIDE(kernel_physical_start = .);
  .boot.text : { *(.boot.text*) } :boot_text
  . = ALIGN(16384);
  .boot.bss (NOLOAD) : {
    armv7_boot_bss_start = .;
    *(.boot.bss*)
    armv7_boot_bss_end = .;
  } :boot_data

  . = ALIGN(4096);
  . += 0x80000000;
  PROVIDE(kernel_start = .);
  .text : AT(ADDR(.text) - 0x80000000) {
    *(.text*)
    *(.init.text*)
  } :kernel_text
  . = ALIGN(4096);
  .rodata : AT(ADDR(.rodata) - 0x80000000) { *(.rodata*) } :kernel_text
  . = ALIGN(4096);
  .data : AT(ADDR(.data) - 0x80000000) {
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
  } :kernel_data
  . = ALIGN(4096);
  .bss (NOLOAD) : AT(ADDR(.bss) - 0x80000000) {
    PROVIDE(armv7_bss_start = .);
    *(.bss*)
    *(.init.bss*)
    *(COMMON)
    PROVIDE(armv7_bss_end = .);
  } :kernel_data
  . = ALIGN(4096);
  PROVIDE(kernel_end = .);
  PROVIDE(kernel_physical_end = . - 0x80000000);

  /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note.GNU-stack) }
}
