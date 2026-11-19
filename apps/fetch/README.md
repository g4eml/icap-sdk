# FETCH.APP - HTTP(S) downloader

Downloads the files listed in `FETCH.CFG` to the SD card and exits: single files or a whole server
folder (synchronised; verified against an `MD5SUMS` file when the folder has one). Firmware `.dat`
files go to the radio's own folder, where **MENU > Firmware Update** looks. IC-705: WLAN, HTTP and
HTTPS. IC-9700: wired LAN, plain HTTP only. The `FETCH.CFG` syntax is described at the top of
[`fetch.c`](fetch.c); [`FETCH.CFG`](FETCH.CFG) is an example.

Own code, uses only the public API in `icap.h`. **MIT** ([`LICENSE`](LICENSE)),
copyright (c) 2026 Dawid SQ6EMM.

## Build

Needs the Arm GNU toolchain (`arm-none-eabi-gcc`) and Python 3.

```sh
sh build.sh          # -> build/FETCH.APP (one file for the IC-705 and the IC-9700)
```
