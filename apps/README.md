# ICAP reference apps

Each app is its own project in a subdirectory, with **its own `LICENSE`**.

The SDK header [`../icap.h`](../icap.h) is MIT, so an app may use any license,
including GPL. Build each app against `../icap.h` and pack it into a `*.APP`.

| App | What | Radios | Status | License (planned) |
|-----|------|--------|--------|-------------------|
| **Q65** | Q65-60 decoder mode (A..E), WSJT-X-style decode lines, SNR in the WSJT-X 2500 Hz convention | IC-705 E, IC-9700 | runs on the radio (on-air decodes confirmed on the IC-9700) | GPL-3.0 (WSJT-X-derived) |
| **FT8** | FT8 decoder mode, 15 s periods, `~` lines like WSJT-X | IC-705 E, IC-9700 | emulation-tested | MIT parts (ft8_lib) + own code |
| **JT4** | JT4 A..G decoder mode (EME/microwave), `$` lines like WSJT-X | IC-705 E, IC-9700 | emulation-tested | GPL-3.0 (WSJT-X-derived) |
| **PI4** | PI4 microwave-beacon decoder mode | IC-705 E, IC-9700 | emulation-tested | per upstream |
| **FETCH** | HTTP(S) downloader: syncs a server folder to the SD card (MD5SUMS check, firmware `.dat` to the radio folder) | IC-705 (WLAN, HTTPS) / IC-9700 (wired LAN, HTTP only) | runs on both radios | MIT |
| **CLOCK**, **SYSINFO**, **SPEED** | small examples: clock/window, platform info, memory speed test | both | runs on the radio | MIT |

All decoders share one design: every period (minute, or 15 s for FT8) is captured from
`icap_audio_read()` to the SD card while the previous one is decoded, so no period is
skipped. Only decoded lines go to the DECODE screen (`icap_decode_out`); status and errors
go to the app window and a `<MODE>DEC.TXT` log. Each decoder has an optional
`<MODE>.CFG` (search range etc.). Typical decode time on the IC-705 is a few seconds.
On the IC-9700 it also depends on the SD card speed, because captures are written to it.

Source code for the apps will be published here; until then the table describes what
exists.

> Apps that reuse or derive from GPL-licensed code (e.g. WSJT-X) **must** be
> distributed under the GPL and carry the GPL text in their own directory. MIT on
> the SDK header is compatible with that.

TX beacons (`icap_tx_*`) are the next step; receive, clock, files, network and the
decoder-mode integration are in place.
