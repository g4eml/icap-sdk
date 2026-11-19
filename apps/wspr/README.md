# WSPR.APP - WSPR-2 beacon

Beacon-only app (the WSPR decoder is not published yet). Registered as mode `WSPR`; with the beacon
armed by the owner it transmits `<call> <grid4> <dBm>` built from `@MYCALL` / `@MYLOC` in the even
UTC minutes. Optional `WSPR.CFG`: `dbm=` (reported power, default 23), `txfreq=` (audio centre
frequency, Hz, default 1500), `call=` / `loc=`.

## Sources and origin

| Files | Origin | License |
|---|---|---|
| `wsprbcn.c` | the app | GPL-3.0-or-later |
| `wsprenc.c/.h` | WSPR-2 encoder; message packing, convolutional code, interleaver and sync vector reproduce WSJT-X `wsprsim_utils.c` (c) 2014-2018 Steven Franke K9AN, Joe Taylor K1JT | GPL-3.0-or-later |
| [`../digi-common/`](../digi-common/) | beacon driver, synthesizer, `msg77.c` | GPL-3.0-or-later |

Derived from **WSJT-X**, (c) Joe Taylor K1JT et al., GPL-3.0 (<https://wsjt.sourceforge.io/>).

## License

**GPL-3.0-or-later** ([`LICENSE`](LICENSE)). Copyright (c) 2026 Dawid SQ6EMM; WSJT-X parts (c) K9AN, K1JT.

## Build

Needs the Arm GNU toolchain (`arm-none-eabi-gcc`; the release builds use 12.2.rel1) and Python 3.

```sh
sh build.sh          # -> build/WSPR.APP (one file for the IC-705 and the IC-9700)
```
