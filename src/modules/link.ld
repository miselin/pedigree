/* Link.ld -- Linker script for the kernel - ensure everything goes in the */
/*            Correct place.  */

SECTIONS
{
  . = SIZEOF_HEADERS;

  .note :
  {
    *(.note*)
  }

  . = ALIGN(4096);
  .text :
  {
    *(.init*)
    *(.plt*)
    *(.plt.got*)
    *(.text*)
    *(.fini*)
  } :text

  . = ALIGN(4096);
  .dynsym :
  {
    *(.dynsym)
  } :rodata

  . = ALIGN(4096);
  .dynstr :
  {
    *(.dynstr)
  } :rodata

  . = ALIGN(4096);
  .modinfo :
  {
    *(.modinfo*)
  } :rodata

  . = ALIGN(4096);
  .rodata :
  {
    *(.rodata*)
  } :rodata

  . = ALIGN(4096);
  .init_array :
  {
    HIDDEN(start_ctors = .);
    *(.init_array*)
    *(.ctors*)
    HIDDEN(end_ctors = .);
  } :data

  .fini_array :
  {
    HIDDEN(start_dtors = .);
    *(.fini_array*)
    *(.dtors*)
    HIDDEN(end_dtors = .);
  } :data

  . = ALIGN(4096);
  .dynamic :
  {
    *(.dynamic)
  } :data :dynamic

  . = ALIGN(4096);
  .got :
  {
    *(.got)
  } :data

  . = ALIGN(4096);
  .got.plt :
  {
    *(.got.plt)
  } :data

  . = ALIGN(4096);
  .data :
  {
    *(.data*)
    *(.sdata*)
  } :data

  .comment :
  {
    *(.comment)
  }

  .gnu_debuglink :
  {
    *(.gnu_debuglink)
  }

  /DISCARD/ : {
    *(.text.crt0*);
  }

  . = ALIGN(4096);
  .bss :
  {
    *(.bss*)
  } :data
}

PHDRS
{
  text PT_LOAD FLAGS(5) FILEHDR PHDRS; /* r-x */
  rodata PT_LOAD FLAGS(4); /* r-- */
  data PT_LOAD FLAGS(6); /* rw- */
  dynamic PT_DYNAMIC FLAGS(4); /* r-- */
}
