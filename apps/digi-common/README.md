# digi-common - code shared by the decoder / beacon apps

Not an app: these files are compiled into [FT8](../ft8/), [JT4](../jt4/), [PI4](../pi4/),
[WSPR](../wspr/) and [Q65](../q65/).

| File | What | SPDX |
|---|---|---|
| `digiapp.c` | the decoder app shell for the period-based modes (`-DDIGI_FT8`, `-DDIGI_JT4`, `-DDIGI_PI4`): capture each period to the SD card, decode the previous one, print decode lines, settings from `@MODECFG` and `<MODE>.CFG`, logs, QSO mode (FT8, JT4) | GPL-3.0-or-later |
| `qso.h` | header-only QSO mode for FT8, JT4 and Q65: the owner taps a decoded line (`@QSO`), the app proposes the next standard message, a second tap sends it once (`@QTX0` / `@QTX1`, CQ: `@QTXC`); ADIF record through `@QLOG` | GPL-3.0-or-later |
| `beacon.h` | header-only beacon driver: message from `@MYCALL` / `@MYLOC`, arming state from `@TX`, streaming through `icap_tx_*` | GPL-3.0-or-later |
| `spot.h` | header-only spotting glue: writes one line per decode to `@SPOT` (the firmware reports to PSKReporter) | GPL-3.0-or-later |
| `digi_util.c/.h` | freestanding helpers: math, FFT, buffered SD reader | GPL-3.0-or-later |
| `msg77.c/.h` | WSJT-X 77-bit standard message packing (subset: two calls + grid, report, `RRR`, `RR73`, `73`), after WSJT-X `packjt77.f90` | GPL-3.0-or-later |
| `txsynth.c/.h` | streaming MFSK / GFSK audio synthesizer and period alignment (own code) | MIT |
| `flags.sh` | compiler / linker flags shared by the build scripts (FT8, JT4, PI4, WSPR) | GPL-3.0-or-later |

## License

This directory as a whole is **GPL-3.0-or-later** (see [`LICENSE`](LICENSE)): `msg77.c` follows
WSJT-X (c) Joe Taylor K1JT et al. (GPL-3.0), and the shell links GPL decoders. Files marked MIT
above may also be used under the MIT license on their own (text in [`../../LICENSE`](../../LICENSE)).

Copyright (c) 2026 Dawid SQ6EMM.
