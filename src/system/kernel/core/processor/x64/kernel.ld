/* Link.ld -- Linker script for the kernel - ensure everything goes in the */
/*            Correct place.  */

ENTRY(start)
SECTIONS
{
  . = 0xFFFFFFFF80000000 + SIZEOF_HEADERS;
  .init : AT(ADDR(.init) - 0xFFFFFFFF7FF00000)
  {
    . = ALIGN(4096);
    HIDDEN(init = .);
    HIDDEN(_init = .);
    HIDDEN(__init = .);
    *(.init.multiboot)
    *(.init.text);
    *(.init.data);
    *(.asm.bss);
    HIDDEN(trampoline = .);
    *(.trampoline.text);
    HIDDEN(trampoline_end = .);
    . = ALIGN(4096);
  }

  .text : AT(ADDR(.text) - 0xFFFFFFFF7FF00000)
  {
    . = ALIGN(4096);
    HIDDEN(code = .);
    HIDDEN(_code = .);
    HIDDEN(__code = .);
    *(.text*)
    . = ALIGN(4096);
  }

  .data : AT(ADDR(.data) - 0xFFFFFFFF7FF00000)
  {
    . = ALIGN(4096);
    HIDDEN(start_ctors = .);
    *(.ctors)
    HIDDEN(end_ctors = .);
    HIDDEN(start_dtors = .);
    *(.dtors)
    HIDDEN(end_dtors = .);
    
    HIDDEN(start_module_ctors = .);
    *(.module_ctors)
    HIDDEN(end_module_ctors = .);

    HIDDEN(data = .);
    HIDDEN(_data = .);
    HIDDEN(__data = .);
    *(.data)
    *(.rodata*)
    . = ALIGN(4096);
    
    HIDDEN(start_modinfo = .);
    *(.modinfo)
    HIDDEN(end_modinfo = .);
  }

  .bss : AT(ADDR(.bss) - 0xFFFFFFFF7FF00000)
  {
    . = ALIGN(4096);
    HIDDEN(bss = .);
    HIDDEN(_bss = .);
    HIDDEN(__bss = .);
    *(.asm.preserve.bss)
    *(.bss*)
    . = ALIGN(4096);
  }
  
  .mptrampoline : AT(ADDR(.mptrampoline) - 0xFFFFFFFF7FF00000)
  {
    . = ALIGN(4);
    
    HIDDEN(trampoline16 = .);
    *(.trampoline.text16)
    HIDDEN(trampoline16_end = .);
    
    . = ALIGN(4);
    
    HIDDEN(trampoline32 = .);
    *(.trampoline.text32)
    HIDDEN(trampoline32_end = .);
    
    . = ALIGN(4);
    
    HIDDEN(trampolinegdtr = .);
    *(.trampoline.data.gdtr)
    HIDDEN(trampolinegdtr_end = .);
    
    . = ALIGN(4);
    
    HIDDEN(trampolinegdt = .);
    *(.trampoline.data.gdt)
    HIDDEN(trampolinegdt_end = .);

    . = ALIGN(4);

    *(.trampoline.data.gdtr64)
    *(.trampoline.data.gdt64)
  }

  /DISCARD/ : { *(.comment) *(.eh_frame) }

  .end : AT(ADDR(.end) - 0xFFFFFFFF7FF00000)
  {
    . = ALIGN(4096);
    HIDDEN(end = .);
    HIDDEN(_end = .);
    HIDDEN(__end = .);
  }
}
