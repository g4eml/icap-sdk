# SPEED.APP - memory speed test

Runs the same small kernels (memset, memcpy, a*x+y, FFT, FIR) on a buffer in the app window, in the
app data region and on the app stack, and shows the throughput of each on the app window and in
`SPEED.TXT`. Useful to see how much slower the data region is on a given radio before deciding
where a decoder keeps its hot data.

Own code, uses only the public API in `icap.h`. **MIT** ([`LICENSE`](LICENSE)),
copyright (c) 2026 Dawid SQ6EMM.

## Build

Needs the Arm GNU toolchain (`arm-none-eabi-gcc`) and Python 3.

```sh
sh build.sh          # -> build/SPEED.APP (one file for the IC-705 and the IC-9700)
```
