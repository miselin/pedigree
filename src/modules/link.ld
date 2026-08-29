/* Link.ld -- Linker script for the kernel - ensure everything goes in the */
/*            Correct place.  */

SECTIONS
{
  . = SIZEOF_HEADERS;

  .note :
  {
    *(.note*)
  }

  . = ALIGN(@PEDIGREE_TARGET_PAGE_SIZE@);
  .text :
  {
    *(.init*)
    *(.plt*)
    *(.plt.got*)
    *(.text*)
    *(.fini*)
  } :text

  . = ALIGN(@PEDIGREE_TARGET_PAGE_SIZE@);
  .dynsym :
  {
    *(.dynsym)
  } :rodata

  . = ALIGN(@PEDIGREE_TARGET_PAGE_SIZE@);
  .dynstr :
  {
    *(.dynstr)
  } :rodata

  . = ALIGN(@PEDIGREE_TARGET_PAGE_SIZE@);
  .modinfo :
  {
    *(.modinfo*)
  } :rodata

  . = ALIGN(@PEDIGREE_TARGET_PAGE_SIZE@);
  .rodata :
  {
    *(.rodata*)
  } :rodata

  . = ALIGN(@PEDIGREE_TARGET_PAGE_SIZE@);
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

  . = ALIGN(@PEDIGREE_TARGET_PAGE_SIZE@);
  .dynamic :
  {
    *(.dynamic)
  } :data :dynamic

  . = ALIGN(@PEDIGREE_TARGET_PAGE_SIZE@);
  .got :
  {
    *(.got)
  } :data

  . = ALIGN(@PEDIGREE_TARGET_PAGE_SIZE@);
  .got.plt :
  {
    *(.got.plt)
  } :data

  . = ALIGN(@PEDIGREE_TARGET_PAGE_SIZE@);
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

  . = ALIGN(@PEDIGREE_TARGET_PAGE_SIZE@);
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
