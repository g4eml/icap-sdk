# Q65.APP - Q65-60 decoder mode and beacon

Q65-60 decoder (submodes A..E) for the radio's DECODE screen: every UTC minute is captured to the
SD card and decoded while the next one is captured; WSJT-X style lines
(`HHMM SNR  DT FREQ :  MESSAGE`), SNR in the WSJT-X 2500 Hz convention. Decodes are reported
through `@SPOT`. With the beacon armed by the owner, the same app transmits a Q65-60 beacon message
built from `@MYCALL` / `@MYLOC`, in the Q65-60 submode the owner set on the radio (DECODE › SET ›
SUBMODE, read from `@MODECFG`).

**QSO mode** (new in 20261119v13; emulation-tested, not yet run on hardware): tap a decoded line
and the app prints the next message of the standard sequence as `TX? ...`; tap that line and it is
sent once, in the minute opposite to the other station's. With CQ armed on the radio the app calls
`CQ <call> <grid4>` in every second minute. A completed QSO is appended to `ICAPQSO.ADI`. QSO
answers and CQ always go out as **Q65-60D** - the decoder linked here decodes 60D only - whatever
SUBMODE says (that setting applies to the beacon). Every transmission needs a tap by the owner; see
"QSO mode" in [`../../README.md`](../../README.md) and [`../digi-common/qso.h`](../digi-common/qso.h).

Optional `Q65.CFG`: see the top of [`q65_705.c`](q65_705.c) and [`../../README.md`](../../README.md).

## Sources and origin

| Files | Origin | License |
|---|---|---|
| `q65_705.c`, `q65beacon_cb.h`, `stubs.c` | the app (used for both radios): capture, SD-backed decoder callbacks, display, logs | GPL-3.0-or-later |
| `q65enc.c/.h` | Q65 encoder after WSJT-X `lib/qra/q65/` (`genq65.f90`, `q65.c`, `qracodes.c`, code tables `qra15_65_64_irr_e23.c`) | GPL-3.0-or-later |
| `decoder/q65beacon/` | the decoder (Rust, `no_std`): streaming low-memory port of the WSJT-X Q65 decode path - one-window FFT front end, Costas sync, fast-fading metric, QRA belief propagation, WSJT-X SNR | GPL-3.0-or-later |
| `decoder/mfsk-core-0.10.1-icap.patch` | changes to the **mfsk-core** crate 0.10.1 by its authors, <https://github.com/jl1nie/mfsk-core>, a Rust port of the WSJT-X modes: `no_std` build of the Q65 parts and a per-row fast-fading metric. The decoder uses its QRA(15,65) code tables, fast-fading tables, CRC and Q65 message unpacking. `decoder/build_decoder.sh` downloads the unmodified crate from crates.io (checksum-pinned) and applies the patch | GPL-3.0-or-later |
| [`../digi-common/`](../digi-common/) | beacon driver, QSO mode, spotting glue, synthesizer, `msg77.c` | GPL-3.0-or-later |
| `q65_split705.ld`, `q65_split9700.ld`, `mkapp_split.py`, `build.sh` | link scripts, packer and build script (below) | GPL-3.0-or-later / MIT (packer) |

Q65 and the QRA codes: Nico Palermo **IV3NWV**, Joe Taylor **K1JT**, Steve Franke **K9AN** and the
WSJT Development Group. Derived from **WSJT-X**, (c) Joe Taylor K1JT et al., GPL-3.0
(<https://wsjt.sourceforge.io/>).

## License

**GPL-3.0-or-later** ([`LICENSE`](LICENSE)). Copyright (c) 2026 Dawid SQ6EMM; WSJT-X parts
(c) K1JT, IV3NWV et al.; mfsk-core (c) its authors.

## Build

1. **Decoder library** (Rust nightly, pinned in `decoder/q65beacon/rust-toolchain.toml`; needs
   network access for the toolchain, mfsk-core and the crates in `Cargo.lock`):

   ```sh
   cd decoder && sh build_decoder.sh
   # or, in the official Rust container image:
   docker run --rm -v "$PWD/decoder":/work -w /work rust:1 sh build_decoder.sh
   ```

   The paths matter for a byte-identical result: the release library was built with this
   directory mounted at `/work` (the crate at `/work/q65beacon`, as in the `docker` line). Built
   elsewhere, the decoder is the same code, but symbol hashes - and so the order of functions in
   `Q65.APP` - differ.

2. **App** (Arm GNU toolchain `arm-none-eabi-gcc`, the release builds use 12.2.rel1; Python 3):

   ```sh
   sh build.sh          # -> build/705/Q65.APP (IC-705), build/9700/Q65.APP (IC-9700)
   ```

   The app file `q65_705.c` is compiled for size as Thumb code on both radios (it is not the hot
   path; the decoder library is).

### Why Q65 is built differently from the other apps

The other apps are one relocatable image (`../../tools/app.ld`, `../../tools/mkapp.py`) that the
loader can place anywhere. Q65 does not fit that way: its code and data fill the app window, so its
read-only data and log buffers go into the app data region. Such a **split image** is linked at
fixed addresses, per radio, and is packed by [`mkapp_split.py`](mkapp_split.py).

The addresses in the two link scripts are the **ICAP app memory of the loader** on each radio - the
window an app is loaded into and the data region it gets as heap, the same memory `icap_mem_info()`
and `icap_heap()` report to any app at run time. They are not addresses of firmware code or data.

| Radio | Link script | App window (segment 1) | App data region (segment 2, then heap) |
|---|---|---|---|
| IC-705 | `q65_split705.ld` | `0x20831000`, 80 KiB | `0x207F0000`, 240 KiB |
| IC-9700 | `q65_split9700.ld` | `0x207F6000`, 76 KiB | `0x209AD080`, 296 KiB |

Because the layout belongs to a loader build, the IC-9700 image also carries the id of the loader
build it was linked for (`--fw` in `build.sh`). A firmware release that moves the app memory needs
these values updated.
