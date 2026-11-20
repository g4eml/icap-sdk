# WSPR.APP - WSPR-2 decoder mode and beacon

WSPR-2 decoder for the radio's DECODE screen: 2-minute periods from the even UTC minute, lines in
the WSJT-X WSPR layout with the audio frequency (`HHMM SNR  DT FREQ DR  CALL GRID DBM`), sorted by
frequency. Decodes with a plain call sign are reported through `@SPOT`. With the beacon armed by
the owner, the same app transmits `<call> <grid4> <dBm>` built from `@MYCALL` / `@MYLOC`, in every
second period, always starting in an even UTC minute; decoding and the beacon never run at once.
One file for both radios.

The app does not store a period's audio: it runs the decoder's down-converter while the period is
received (mix at 1500 Hz, 8000 -> 375 samples/s) and writes the 16-bit I/Q to the SD card (171 KB
per period, `WSPRIQA.BIN` / `WSPRIQB.BIN` in turn); a complete period is decoded while the next one
is captured. A decode that is still running when the next capture is complete is cut off (decodes
found so far are kept).

Settings: the owner's for this mode from the radio (`@MODECFG`: PSKREPORT, search range), then the
optional `WSPR.CFG`, which wins: `pskr=`, `flo=` / `fhi=` (audio Hz of the signal centre, default
1390..1610, at most 1350..1650), `dbm=` (reported power, default 23), `txfreq=` (audio centre
frequency of the beacon, Hz, default 1500), `call=` / `loc=`.

Status: the beacon runs on the radio. The decoder (new in firmware release 20261119v13) is tested
in emulation and has not yet been confirmed on hardware.

## Sources and origin

| Files | Origin | License |
|---|---|---|
| `wsprapp.c` | the app: capture, decode scheduling, display, log, beacon | GPL-3.0-or-later |
| `wsprdec.c/.h` | WSPR-2 decoder in freestanding C, **ported from WSJT-X `wsprd`** (`wsprd.c`, `wsprd_utils.c`, `fano.c`, `metric_tables.c`; Copyright 2001-2018 Joe Taylor K1JT and 2014-2018 Steven Franke K9AN, GPL-3.0). The candidate search, `sync_and_demodulate`, non-coherent sequence detection, signal subtraction, the soft-decision Fano decoder with the WSJT-X metric tables and the message unpacking are re-implementations of that code. Own replacements: a polyphase FIR down-converter instead of the FFT down-sampler of `readwavfile()`, an in-tree radix-2 512-point FFT instead of FFTW, all memory from a caller buffer, reduced-memory storage (16-bit I/Q, no stored spectrogram). Default `wsprd` decode path only (Fano, 3 passes, subtraction); no Jelinek / OSD decoder. The call-sign hash is Bob Jenkins' lookup3, as `wsprd` uses it (`nhash`) | GPL-3.0-or-later |
| `wsprenc.c/.h` | WSPR-2 encoder; message packing, convolutional code, interleaver and sync vector reproduce WSJT-X `wsprsim_utils.c` (c) 2014-2018 Steven Franke K9AN, Joe Taylor K1JT | GPL-3.0-or-later |
| [`../digi-common/`](../digi-common/) | beacon driver, spotting glue, synthesizer, `msg77.c` | GPL-3.0-or-later |
| [`v6/`](v6/) | `wsprbcn.c`, the former beacon-only app (superseded, not built) | GPL-3.0-or-later |

Derived from **WSJT-X**, (c) Joe Taylor K1JT et al., GPL-3.0 (<https://wsjt.sourceforge.io/>).

## License

**GPL-3.0-or-later** ([`LICENSE`](LICENSE)). Copyright (c) 2026 Dawid SQ6EMM; WSJT-X parts (c) K1JT, K9AN.

## Build

Needs the Arm GNU toolchain (`arm-none-eabi-gcc`; the release builds use 12.2.rel1) and Python 3.

```sh
sh build.sh          # -> build/WSPR.APP (one file for the IC-705 and the IC-9700)
```
