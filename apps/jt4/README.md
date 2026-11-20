# JT4.APP - JT4 decoder mode and beacon

JT4 A..G decoder (EME / microwave) for the radio's DECODE screen: one-minute periods, `$` lines
like WSJT-X, Fano decoding (no deep search). Decodes are reported through `@SPOT`. With the beacon
armed by the owner, the same app transmits `<call> <grid4>` built from `@MYCALL` / `@MYLOC`.

**QSO mode** (new in 20261119v13; emulation-tested, not yet run on hardware): tap a decoded line
and the app prints the next message of the standard sequence (grid, report, R-report, `RRR`, `73`)
as `TX? ...`; tap that line and it is sent once, in the minute opposite to the other station's.
With CQ armed on the radio the app calls `CQ <call> <grid4>` in every second minute. A completed
QSO is appended to `ICAPQSO.ADI`. Standard messages only (calls with a prefix or suffix are
refused). Every transmission needs a tap by the owner; see "QSO mode" in
[`../../README.md`](../../README.md) and [`../digi-common/qso.h`](../digi-common/qso.h).

The submode is the one the owner sets on the radio (DECODE › SET › SUBMODE, read from `@MODECFG`
and followed while the app runs); the file below overrides it.
Optional `JT4.CFG`: `sub=A`..`G` (submode, default G), `flo=` / `fhi=` (search range, Hz),
`txfreq=`, `call=` / `loc=`.

## Sources and origin

| Files | Origin | License |
|---|---|---|
| `jt4dec.c/.h` | JT4 receiver; signal model and decode structure after WSJT-X `gen4.f90`, `jt4.f90`, `xcor4`, `decode4`, `extract4` | GPL-3.0-or-later |
| `fano232.c/.h` | K=32 r=1/2 convolutional code and Fano decoder: port of WSJT-X `fano232.f90`, `encode232.f90` (after Phil Karn KA9Q), metric table from `getmet4.f90` | GPL-3.0-or-later |
| `jt72msg.c/.h` | 72-bit message unpacking: port of WSJT-X `packjt.f90` | GPL-3.0-or-later |
| `jt4enc.c/.h` | JT4 encoder after WSJT-X `gen4.f90`, `encode4.f90`, `packjt.f90` (free text and standard messages), `interleave4.f90` | GPL-3.0-or-later |
| [`../digi-common/`](../digi-common/) | app shell, beacon driver, QSO mode, spotting glue, synthesizer | GPL-3.0-or-later |

Derived from **WSJT-X**, (c) Joe Taylor K1JT and the WSJT Development Group, GPL-3.0
(<https://wsjt.sourceforge.io/>).

## License

**GPL-3.0-or-later** ([`LICENSE`](LICENSE)). Copyright (c) 2026 Dawid SQ6EMM; WSJT-X parts (c) K1JT et al.

## Build

Needs the Arm GNU toolchain (`arm-none-eabi-gcc`; the release builds use 12.2.rel1) and Python 3.

```sh
sh build.sh          # -> build/705/JT4.APP (IC-705), build/9700/JT4.APP (IC-9700)
```
