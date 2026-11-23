/* SPDX-License-Identifier: GPL-3.0-or-later
 * Q65.APP link script for the IC-9700 (ICAP ABI 3 split image; see q65_split705.ld for the scheme).
 *
 * WIN and REG are the app memory layout of the ICAP loader for this radio and firmware build (what
 * icap_mem_info() reports at run time) - they are app memory, not firmware code or data.  The 9700
 * window holds image + bss (the 8 KiB task stack is separate), so the rolling debug log g_dbg and the
 * decode log g_dec are placed in segment 2 in the data region, as on the IC-705. */
ENTRY(_icap_start)
MEMORY
{
  WIN (rwx) : ORIGIN = 0x207F6000, LENGTH = 0x13000   /* 76 KiB: image + BSS (stack is separate)   */
  REG (r)   : ORIGIN = 0x209AD080, LENGTH = 0x49F80   /* 296 KiB region: .rodata + g_dbg here, heap after */
}
SECTIONS
{
  .text : {
    KEEP(*(.icap_start)) *(.text .text.*) *(.glue_7) *(.glue_7t) *(.vfp11_veneer) *(.v4_bx)
    . = ALIGN(4);
  } > WIN
  .data : { . = ALIGN(4); *(.data .data.*) . = ALIGN(4); } > WIN
  _icap_image_end = .;
  PROVIDE(__exidx_start = .); PROVIDE(__exidx_end = .);   /* panic=abort: no unwind tables */
  .rodata : { . = ALIGN(8); _icap_seg2_start = .; *(.rodata .rodata.*) . = ALIGN(8); } > REG
  .rbss (NOLOAD) : { . = ALIGN(8); *q65_705.o(.bss.g_dbg) *q65_705.o(.bss.g_dec) . = ALIGN(8); _icap_seg2_end = .; } > REG
  .bss (NOLOAD) : { . = ALIGN(8); _icap_bss_start = .; *(.bss .bss.* COMMON) . = ALIGN(8); _icap_bss_end = .; } > WIN
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) *(.got*) *(.dynamic) *(.interp) }
}
