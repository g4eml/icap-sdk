# ICAP reference apps

Each app is its own project in a subdirectory, with **its own `LICENSE`**.

The SDK header [`../icap.h`](../icap.h) is MIT, so an app may use any license,
including GPL. Build each app against `../icap.h` and pack it into a `*.APP`.

| App | What | Radios | Status | License |
|-----|------|--------|--------|---------|
| [**Q65**](q65/) | Q65 decoder mode, 15 / 30 / 60 s, submodes A..E (not 15D/15E at 8 kHz), WSJT-X-style decode lines, SNR in the WSJT-X 2500 Hz convention; Q65-60 beacon; QSO mode (Q65-60D) | IC-705 E, IC-9700 | runs on the radio (60D decode + beacon confirmed; Q65-30B decode confirmed on the IC-9700); the other 15 / 30 s modes and submodes host-tested; QSO mode emulation-tested, not yet run on hardware | GPL-3.0-or-later (derived from WSJT-X; uses mfsk-core) |
| [**FT8**](ft8/) | FT8 decoder mode, 15 s periods, `~` lines like WSJT-X; FT8 beacon; QSO mode | IC-705 E, IC-9700 | decode runs on the radio; beacon emulation-tested; QSO mode emulation-tested, not yet run on hardware | GPL-3.0-or-later as a whole; the decoder core and the vendored ft8_lib are MIT |
| [**JT4**](jt4/) | JT4 A..G decoder mode (EME/microwave), `$` lines like WSJT-X; JT4 beacon; QSO mode | IC-705 E, IC-9700 | decode runs on the radio; beacon emulation-tested; QSO mode emulation-tested, not yet run on hardware | GPL-3.0-or-later (derived from WSJT-X) |
| [**PI4**](pi4/) | PI4 microwave-beacon decoder mode; PI4 beacon (with CW ID and carrier) | IC-705 E, IC-9700 | runs on the radio (beacon confirmed) | GPL-3.0-or-later as a whole; the encoder is MIT (own code from the OZ2M / DJ5HG specification) |
| [**WSPR**](wspr/) | WSPR-2 decoder mode, 2 min periods, lines in the WSJT-X WSPR layout; WSPR-2 beacon | IC-705 E, IC-9700 | beacon runs on the radio; decoder emulation-tested, not yet confirmed on hardware | GPL-3.0-or-later (decoder ported from WSJT-X `wsprd`, encoder derived from WSJT-X) |
| [**FETCH**](fetch/) | HTTP(S) downloader: syncs a server folder to the SD card (MD5SUMS check, firmware `.dat` to the radio folder) | IC-705 (WLAN, HTTPS) / IC-9700 (wired LAN, HTTP only) | runs on both radios | MIT |
| [**CLOCK**](clock/), [**SPEED**](speed/) | small examples: clock/window, memory speed test | both | runs on the radio | MIT |
| **SYSINFO** | platform info for the bench (reads firmware internals) | both | binary only, source not published | MIT |

[`digi-common/`](digi-common/) holds the code the decoder and beacon apps share (app shell, beacon
driver, QSO mode, spotting glue, audio synthesizer); it is GPL-3.0-or-later.

The sources here are the ones the app bundle of firmware release **20261119v13** is built from.

Every decoder reports its decodes to PSKReporter through the firmware's `@SPOT` service, and the
beacon is armed by the owner on the radio (long press of the mode tile; a red frame on the
emission button shows it is armed). The per-mode settings the owner makes on the radio
(DECODE › SET: SUBMODE, PSKREPORT) reach the apps through `@MODECFG`.

**QSO mode** (FT8, Q65, JT4; new in 20261119v13): the owner taps a decoded line, the app proposes
the next message of the standard sequence as a `TX?` line, and a tap on that line sends it once.
CQ is armed on the radio (a longer hold of the mode tile, green frame). Completed QSOs are
appended to the ADIF file `ICAPQSO.ADI` in the app folder. The apps never transmit a QSO message
without a tap, and the firmware enforces that - see "QSO mode" in [`../README.md`](../README.md).
QSO mode has passed emulation tests (the radio's code with these apps, the transmit audio decoded
by WSJT-X) but **has not yet been run on hardware**: first tests into a dummy load.

Built apps for each release are on the patcher page:
https://sq6emm.github.io/icom-custom-firmware/

All decoders share one design: every period (minute, or 15 s for FT8) is captured from
`icap_audio_read()` to the SD card while the previous one is decoded, so no period is
skipped (WSPR stores the down-converted signal instead of the audio). Only decoded lines go to the DECODE screen (`icap_decode_out`); status and errors
go to the app window and a `<MODE>DEC.TXT` log. Each decoder has an optional
`<MODE>.CFG` (search range etc.). Typical decode time on the IC-705 is a few seconds.
On the IC-9700 it also depends on the SD card speed, because captures are written to it.

The source code of each app is in its subdirectory, with a `README.md` (what it is, where the
code comes from), the full license text in `LICENSE` and a `build.sh` that builds the `*.APP` for
each supported radio with the public [`../tools/`](../tools/). The C builds are reproducible: the
same toolchain gives the same file, byte for byte; Q65 also needs the documented build path for
its Rust decoder (see [`q65/README.md`](q65/README.md)).

> Apps that reuse or derive from GPL-licensed code (e.g. WSJT-X) **must** be
> distributed under the GPL and carry the GPL text in their own directory. MIT on
> the SDK header is compatible with that.

Receive, clock, files, network, the decoder-mode integration, TX beacons and spotting are
in place on both radios; QSO mode is implemented and waits for its first run on hardware.
