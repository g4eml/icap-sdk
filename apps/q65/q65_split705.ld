/* SPDX-License-Identifier: GPL-3.0-or-later
 * Q65.APP link script for the IC-705 (ICAP ABI 3 split image).
 *
 * A split image is fully linked at its final addresses (no run-time relocation): segment 1
 * (.text/.data/.bss) in the ICAP app window WIN, segment 2 (.rodata + the two log buffers) at the base
 * of the ICAP app data region REG; the decoder heap follows segment 2.  WIN and REG are the app memory
 * layout of the ICAP loader for this radio and firmware build (what icap_mem_info() reports at run
 * time) - they are app memory, not firmware code or data.
 *
 * The rolling debug log g_dbg and the decode log g_dec (written a line at a time, flushed to
 * Q65DBG.TXT / Q65DEC.TXT; not on the decode path) live in segment 2 so the integrated beacon encoder
 * fits the window.  They are NOBITS inside [_icap_seg2_start, _icap_seg2_end): mkapp_split.py emits zeros,
 * the loader copies + CRCs it, and the heap starts after it (heap_base = region + align8(seg2)).
 * The .rodata/.rbss statements precede .bss so the specific g_dbg/g_dec pattern wins over the generic .bss.* one. */
ENTRY(_icap_start)
MEMORY
{
  WIN (rwx) : ORIGIN = 0x20831000, LENGTH = 0x14000   /* 80 KiB: image + BSS (stack is separate)   */
  REG (r)   : ORIGIN = 0x207F0000, LENGTH = 0x3C000   /* 240 KiB region: .rodata + logs here, heap after */
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
