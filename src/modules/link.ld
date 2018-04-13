/* Link.ld -- Linker script for the kernel - ensure everything goes in the */
/*            Correct place.  */

SECTIONS
{
  /DISCARD/ : {
    *(.text.crt0*);
  }

  . = SIZEOF_HEADERS;

  .dynamic :
  {
    *(.dynamic)
  } :text :dynamic

  .dynsym :
  {
    *(.dynsym)
  }

  .dynstr :
  {
    *(.dynstr)
  }

  .init :
  {
    *(.init*)
  } :text

  .fini :
  {
    *(.fini*)
  } :text

  . = ALIGN(0x100);
  .text :
  {
    *(.text*)
  } :text

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
  .ctors :
  {
    HIDDEN(start_ctors = .);
    *(.init_array*)
    *(.ctors*)
    HIDDEN(end_ctors = .);
  } :rodata

  . = ALIGN(4096);
  .dtors :
  {
    HIDDEN(start_dtors = .);
    *(.fini_array*)
    *(.dtors*)
    HIDDEN(end_dtors = .);
  } :rodata

  . = ALIGN(4096);
  .rwdata :
  {
    *(.data*)
    *(.sdata*)

    *(.got.plt) *(.got)

    *(.bss*)
    *(.dynbss)
  } :data
}

PHDRS
{
  text PT_LOAD FLAGS(5) FILEHDR PHDRS; /* r-x */
  rodata PT_LOAD FLAGS(4); /* rw- */
  data PT_LOAD FLAGS(6); /* rw- */
  dynamic PT_DYNAMIC  FLAGS(4); /* r-- */
}
