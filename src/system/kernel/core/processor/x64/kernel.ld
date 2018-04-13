/* Link.ld -- Linker script for the kernel - ensure everything goes in the */
/*            Correct place.  */

ENTRY(start)
SECTIONS
{
  . = 0xFFFFFFFF80000000 + SIZEOF_HEADERS;

  . = ALIGN(4096);

  PROVIDE(kernel_start = .);

  .init : AT(ADDR(.init) - 0xFFFFFFFF7FF00000)
  {
    PROVIDE(kernel_init = .);
    *(.init.multiboot)
    *(.init.text)
    *(.init.data)

    . = ALIGN(4096);
    *(.asm.bss)
    PROVIDE(kernel_init_end = .);
  }

  . = ALIGN(4096);

  .text : AT(ADDR(.text) - 0xFFFFFFFF7FF00000)
  {
    *(.text*)
  }

  . = ALIGN(4096);

  .data : AT(ADDR(.data) - 0xFFFFFFFF7FF00000)
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
  
  .mptrampoline : AT(ADDR(.mptrampoline) - 0xFFFFFFFF7FF00000)
  {
    *(.trampoline.text16)
    
    . = ALIGN(0x100);
    *(.trampoline.text32)
    
    . = ALIGN(0x100);
    *(.trampoline.data.gdtr)
    
    . = ALIGN(0x100);
    *(.trampoline.data.gdt)

    . = ALIGN(0x100);
    *(.trampoline.data.gdtr64)
    *(.trampoline.data.gdt64)
  }

  . = ALIGN(4096);

  .bss : AT(ADDR(.bss) - 0xFFFFFFFF7FF00000)
  {
    *(.asm.preserve.bss)
    *(.bss*)
    *(COMMON)
  }

  /DISCARD/ : { *(.comment) *(.eh_frame) }

  . = ALIGN(4096);

  PROVIDE(kernel_end = .);
}
