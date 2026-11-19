# CLOCK.APP - clock example

Small example: shows the radio clock (`icap_clock_now`), prints a line on the DECODE screen and
probes which v1rc services the firmware offers.

Own code, uses only the public API in `icap.h`. **MIT** ([`LICENSE`](LICENSE)),
copyright (c) 2026 Dawid SQ6EMM.

## Build

Needs the Arm GNU toolchain (`arm-none-eabi-gcc`) and Python 3.

```sh
sh build.sh          # -> build/CLOCK.APP (one file for the IC-705 and the IC-9700)
```
