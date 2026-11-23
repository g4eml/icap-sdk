# Q65.APP on the PC

Test tools for Q65.APP that run on an x86-64 Linux PC. Nothing here goes into the radio image.

| File | What it is |
|---|---|
| `harness.c` | a simulated radio: the real [`../q65_705.c`](../q65_705.c) and the decoder (built with its `radio` feature, as on the radio) run against a virtual clock, an audio stream with one Q65 transmission per T/R period in white noise, SD files in a directory, `@MODECFG`, and a 4-file open limit |
| `q65gen.c` | one T/R period of Q65 test audio (8 kHz WAV) from the app's own encoder ([`../q65enc.c`](../q65enc.c)), for the decoder's command-line tool |
| `build.sh` | builds both into `build/` |
| `test.sh` | regression run of the harness over 60D, 30C, 15C, 30A, 60A and 15B |

## Build and run

Build the decoder first, as described in [`../README.md`](../README.md), so that `../decoder/mfsk-core` exists. Then:

```sh
sh build.sh
sh test.sh                      # exit status 0 = every check passed
```

One harness run, Q65-30C: the app runs for 200 s. The radio's SUBMODE menu says C, and `Q65.CFG` holds
`period=30`:

```sh
mkdir -p run
build/harness run 200 30 2 -15 C "period=30
"
```

The arguments are: directory, seconds, the period and submode (0..4) sent on the air, SNR in dB, the
SUBMODE menu letter, and the `Q65.CFG` text (`-` for none). Two optional arguments follow: the start
time in UTC seconds of the day, and the simulated cost of each SD read or write in ms (default 3; 20
makes decodes take roughly radio time).

The output shows what was sent, the DECODE screen lines, `@SPOT` reports and the app window's title and
status lines. The app's own files (`Q65DBG.TXT`, `Q65DEC.TXT`, the captures) are written to the directory.

Environment variables:
- `TRACE=1` prints the debug log as it is written.
- `HEAP=<bytes>` sets the data region the app gets. The default is the IC-705's: 240 KiB minus segment 2.
  Lower it to find a mode's memory need. When the heap runs out, the decoder panics; on the radio the app
  is then reaped. In the harness it hangs (the panic handler loops), so wrap runs in `timeout`.

A test WAV for the decoder alone:

```sh
build/q65gen x.wav 30 2 1200 -20 0.2 "CQ G4ABC IO91"
../decoder/q65beacon/target/release/q65beacon_cli x.wav --period 30 --sub C --fmin 600 --fmax 1800
```

(`cargo build --release --bin q65beacon_cli` in `../decoder/q65beacon` builds the command-line tool.)

The encoder and decoder are tested against each other here, not against WSJT-X; on-air checks still
need a radio.
