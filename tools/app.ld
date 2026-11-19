/* ICAP app link script: one image linked at 0, relocated by the loader (R_ARM_ABS32 only). */
ENTRY(_icap_start)
SECTIONS
{
  . = 0;
  .text : { KEEP(*(.icap_start)) *(.text .text.*) *(.glue_7) *(.glue_7t) *(.vfp11_veneer) *(.v4_bx) }
  .rodata : { . = ALIGN(4); *(.rodata .rodata.*) }
  .data : { . = ALIGN(4); *(.data .data.*) . = ALIGN(4); }
  _icap_image_end = .;
  .bss (NOLOAD) : { . = ALIGN(8); _icap_bss_start = .; *(.bss .bss.* COMMON) . = ALIGN(8); _icap_bss_end = .; }
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) *(.got*) *(.dynamic) *(.interp) }
}
