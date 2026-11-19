# FT8.APP - FT8 decoder mode and beacon

FT8 decoder for the radio's DECODE screen: 15 s periods, `~` lines like WSJT-X
(`HHMMSS SNR  DT FREQ ~  MESSAGE`), three decode passes with signal subtraction, callsign
hash table kept across periods. Decodes are reported through `@SPOT`. With the beacon armed by
the owner, the same app transmits `DE <call> <grid4>` built from `@MYCALL` / `@MYLOC`.
Optional `FT8.CFG`: `flo=` / `fhi=` (search range, Hz), `txfreq=` (beacon audio frequency, Hz),
`call=` / `loc=` (override the radio's identity).

## Sources and origin

| Files | Origin | License |
|---|---|---|
| `ft8dec.c/.h` | own decoder (waterfall, subtraction, OSD, SNR) around ft8_lib; carries adapted copies of ft8_lib's sync search and belief-propagation decoder | MIT |
| `ft8/` | vendored **ft8_lib** (c) Kārlis Goba, <https://github.com/kgoba/ft8_lib> - the upstream revision and the list of copied files are in [`ft8/UPSTREAM`](ft8/UPSTREAM), its license in [`ft8/LICENSE`](ft8/LICENSE); `ft8_port.c/.h` adapt it to a freestanding build | MIT |
| `ft8enc.c/.h` | FT8 beacon encoder: tones from ft8_lib, 77-bit packing from `../digi-common/msg77.c` | GPL-3.0-or-later |
| [`../digi-common/`](../digi-common/) | app shell, beacon driver, spotting glue, synthesizer; `msg77.c` follows WSJT-X `packjt77.f90` | GPL-3.0-or-later |

FT8 is a protocol by Joe Taylor K1JT and Steve Franke K9AN (WSJT-X, GPL-3.0).

## License

The app as a whole is **GPL-3.0-or-later** ([`LICENSE`](LICENSE)), because it links the GPL files
above. The MIT parts (`ft8dec.c/.h`, `ft8/`) stay MIT and can be reused on their own.
Copyright (c) 2026 Dawid SQ6EMM; ft8_lib (c) 2018 Kārlis Goba.

## Build

Needs the Arm GNU toolchain (`arm-none-eabi-gcc`; the release builds use 12.2.rel1) and Python 3.

```sh
sh build.sh          # -> build/705/FT8.APP (IC-705), build/9700/FT8.APP (IC-9700)
```

The two builds differ in stack size and in where the decoder keeps its working arrays; see the
comments in `build.sh`. Copy the file for your radio to the app folder as `FT8.APP`.
