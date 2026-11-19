# ICAP build tools

| File | What |
|---|---|
| `app.ld` | link script: one image linked at 0, relocated by the loader (R_ARM_ABS32 only) |
| `icap_start.S` | entry stub: enables the FPU the radio's way, then jumps to `app_main(svc)` |
| `mkapp.py` | packer: ELF -> `*.APP` (header, image, relocations, CRC32); pure Python 3 |

Compiler flags that matter (see [`../examples/hello/build.sh`](../examples/hello/build.sh)):
`-mcpu=cortex-a9 -marm -mfpu=neon-vfpv3 -mfloat-abi=hard -ffreestanding -nostdlib -fno-pic
-mword-relocations -fno-section-anchors -fno-common`, linked with `-T app.ld -Wl,--emit-relocs`.
`mkapp.py` refuses absolute MOVW/MOVT relocations and pointers that leave the app, so a packed
app can be loaded anywhere.

Packer options: `--name` (up to 15 chars), `--models` (1 = IC-705, 2 = IC-9700, 3 = both),
`--stack`, `--abi 3` (app uses the v1rc services), `--background` (EXIT hides the window),
`--autostart` (still needs `AUTORUN.CFG`), `--mode NAME[:usbd|keep]` (register a decoder mode).

Most apps (FT8, JT4, PI4, WSPR, FETCH, CLOCK, SPEED and the example) are relocatable and run on both
radios. Q65 is a large two-segment image linked for each radio's app memory layout; its build files
(link scripts and `mkapp_split.py`) are in [`../apps/q65/`](../apps/q65/).
