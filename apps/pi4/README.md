# PI4.APP - PI4 beacon decoder mode and PI4 beacon

PI4 (PharusIgnis4) is the digital mode of the Next Generation Beacons platform, used by microwave
beacons together with CW and a carrier. The app decodes PI4 on the radio's DECODE screen (one-minute
periods, the PI4 frame is in the first 24.3 s) and reports decodes through `@SPOT`. With the beacon
armed by the owner, it transmits the standard beacon minute: PI4 frame, CW ID (call and locator),
carrier. Optional `PI4.CFG`: `flo=` / `fhi=` (search range, Hz), `txfreq=` (carrier audio frequency),
`cwshift=` (FSK-CW shift in Hz, 0 = on/off keying), `call=` / `loc=`.

## Sources and origin

| Files | Origin | License |
|---|---|---|
| `pi4dec.c/.h` | own decoder written from the PI4 specification; sequential (Fano) decoding of the K=32 r=1/2 code after Phil Karn KA9Q | GPL-3.0-or-later |
| `pi4enc.c/.h` | own encoder written from the PI4 specification; checked bit-exact against the OZ2M reference encoder | MIT |
| [`../digi-common/`](../digi-common/) | app shell, beacon driver, spotting glue, synthesizer | GPL-3.0-or-later |

PI4 specification: **OZ2M** and **DJ5HG**, <https://rudius.net/oz2m/ngnb/pi4.htm>.

## License

The app as a whole is **GPL-3.0-or-later** ([`LICENSE`](LICENSE)). Copyright (c) 2026 Dawid SQ6EMM.

## Build

Needs the Arm GNU toolchain (`arm-none-eabi-gcc`; the release builds use 12.2.rel1) and Python 3.

```sh
sh build.sh          # -> build/705/PI4.APP (IC-705), build/9700/PI4.APP (IC-9700)
```
