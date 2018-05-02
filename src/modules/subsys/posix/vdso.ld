
/**
 * Linker script to make sure vdso has a single read-only loadable segment,
 * with all needed sections for loading at runtime.
 */

SECTIONS
{
    . = SIZEOF_HEADERS;

    /* typical sections for a shared object */
    .hash : { *(.hash) } :text
    .gnu.hash : { *(.gnu.hash) }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }
    .gnu.version : { *(.gnu.version) }
    .gnu.version_d : { *(.gnu.version_d) }
    .gnu.version_r : { *(.gnu.version_r) }

    .note : { *(.note.*) } :text :note

    /* keep exception frame around */
    .eh_frame_hdr : { *(.eh_frame_hdr) } :text :eh_frame_hdr
    .eh_frame : { KEEP (*(.eh_frame)) } :text

    .dynamic : { *(.dynamic) } :text :dynamic

    /* all kinds of data */
    .rodata : { *(.rodata*) } :text
    .data : {
          *(.data*)
          *(.sdata*)
          *(.got.plt) *(.got)
          *(.gnu.linkonce.d.*)
          *(.bss*)
          *(.dynbss*)
          *(.gnu.linkonce.b.*)
    }

    . = ALIGN(0x100); /* text, stay away from data */
    .text : { *(.text*) } :text =0x90909090
}

PHDRS
{
    text            PT_LOAD     FLAGS(5) FILEHDR PHDRS; /* r+x */    dynamic         PT_DYNAMIC  FLAGS(4); /* only r */
    note            PT_NOTE     FLAGS(4); /* only r */
    eh_frame_hdr    PT_GNU_EH_FRAME;
}
