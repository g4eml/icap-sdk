@ ICAP app entry: enable VFP/NEON lazily the stock way (a VFP instruction traps once through the
@ firmware's undefined-instruction handler, which turns the coprocessor on for this task), then call
@ app_main(svc).  r0 = const struct icap_svc *
    .syntax unified
    .arm
    .section .icap_start, "ax"
    .global _icap_start
_icap_start:
    vmrs    r1, fpscr          @ first FP instruction: cp10 -> stock VFP enable (never NEON first)
    b       app_main
