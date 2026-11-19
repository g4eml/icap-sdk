# Icom App SDK — ICAP (IC-705 / IC-9700)

**ICAP** lets you write small apps for custom IC-705 / IC-9700 firmware. An app is a
single `*.APP` file on the SD card; the radio loads it into a sandboxed,
lowest-priority task and calls your entry point. Apps get a clean C API and never
touch raw memory or call firmware functions directly.

The whole API is in [`icap.h`](icap.h). This README explains it.

> **Status: v1rc.** Few users so far, so the API may still change before v1.0.
> Everything below runs on the current custom firmware releases: IC-705 E **20261119v8** and
> IC-9700 **20261119v6** with the SD-card runtime **20261119v8** (patcher:
> https://sq6emm.github.io/icom-custom-firmware/). Build tools and an example are in
> [`tools/`](tools/) and [`examples/`](examples/).

---

## Hello world

```c
#include "icap.h"

int app_main(const struct icap_svc *svc)
{
    icap_use(svc);                         /* required, once, first */
    icap_clear();
    icap_print("Hello from ICAP!");
    icap_set_line(2, icap_is_ic705() ? "IC-705" : "IC-9700");

    while (!icap_should_stop())            /* poll at least every 500 ms */
        icap_sleep_ms(100);

    return 0;
}
```

The app shows an 8-line text panel on the radio and a STOP row; it can read and
write files on the SD card, read the radio's state and clock, and — on firmware
that provides them — read receive audio, print to the native DECODE screen, and
transmit.

---

## The rules (an app that breaks them can freeze the radio)

1. **Stay in your own memory** — your image, BSS, stack and the free gap from
   `icap_mem_info()`. Everything else belongs to the radio.
2. **Never call firmware directly, never disable interrupts, never change CPU/MMU/
   caches/peripherals/GPIO.** Use only the `icap_*` API.
3. **Poll `icap_should_stop()` at least every 500 ms** and return from `app_main`
   when it returns non-zero. (After ~3 s the loader force-kills the app.)
4. **Don't hog the CPU.** You run below every stock task; call `icap_yield()` or
   `icap_sleep_ms()` regularly. Always `icap_close()` files before returning.
5. **TX is opt-in and gated** (see below). An app can never change frequency or
   mode, and only transmits through the `icap_tx_*` API while the owner has
   enabled it.

Faults in your app (bad pointer, stack overflow, …) kill only the app — the radio
keeps running and shows the reason. The radio always keeps priority for RX, TX,
CI-V, the display and the DSP.

---

## API reference

Call `icap_use(svc)` once at the top of `app_main`, then use these.

### Screen
```c
void icap_print(const char *s);              /* add a line (scrolls)        */
void icap_set_line(int line, const char *s); /* set line 0..7               */
void icap_clear(void);
```

### Files (radio's app folder, or the SD card root)
```c
int icap_open(const char *name, int mode);   /* ICAP_O_READ / ICAP_O_WRITE [| ICAP_O_ROOT] */
int icap_read (int fd, void *buf, uint32_t len);
int icap_write(int fd, const void *buf, uint32_t len);
int icap_close(int fd);
int icap_remove(const char *name, int flags); /* delete; flags 0 or ICAP_O_ROOT (v1rc) */
```
Plain file names only (no paths): up to 48 characters of `A-Z a-z 0-9 _ - .`, no leading or
trailing dot. Files live in the app folder; OR `ICAP_O_ROOT` into the mode to use the SD card root, or
`ICAP_O_RADIO` for the radio's own folder (`\IC-705`, `\IC-9700`) - that is where **MENU › Firmware
Update** looks for `.dat` files (v1rc firmware only; older loaders return `ICAP_E_NAME`). At most
**4 files open** (2 on firmware before 20261117v11).
Buffers must be inside your own memory. Always close files before returning.

### Time & scheduling
```c
void     icap_sleep_ms(uint32_t ms);
void     icap_yield(void);
uint32_t icap_ticks(void);                   /* monotonic ms                */
int      icap_clock_now(struct icap_time *t);/* radio local clock (RTC)     */
```

### Info & control
```c
int      icap_mem_info(struct icap_meminfo *mi);
int      icap_radio_state(struct icap_radio *rs); /* freq, mode, is_tx      */
int      icap_should_stop(void);
uint32_t icap_radio_id(void);                /* ICAP_RADIO_IC705 / _IC9700  */
void    *icap_heap(void);                    /* large heap, or 0 if none    */
uint32_t icap_heap_size(void);
```

### Receive audio (decoders)
```c
int icap_audio_rate(void);                   /* sample rate, Hz             */
int icap_audio_read(int16_t *buf, int nsamp);/* samples read, 0 none, <0 err */
```
Received audio is **mono, signed 16-bit, 8000 Hz** — enough for the ~0.2–3.5 kHz
SSB passband that Q65, FT8 and JT4 use (rescale WSJT-X constants from 12 kHz, e.g.
Q65-60 NSPS 7200 → 4800). Read it in a loop and feed your decoder; keep a small ring
rather than whole recordings. `ICAP_E_OVERRUN` means samples were lost since the
last read (treat it as a gap).

### Decoded text out (native DECODE screen)
```c
int icap_decode_out(const char *utf8);       /* show text on the DECODE screen */
int icap_mode(char *buf, int max);           /* registered mode that started the app */
```
**Decoder modes (v1rc):** an app can register up to 4 decoder modes in its header
(packer: `--mode Q65`, name up to 4 characters). On the radio's MODE screen the RTTY button then cycles
RTTY → RTTY-R → each registered decoder → RTTY. Selecting a decoder switches the
radio to USB-D, opens the DECODE screen and starts the app; the mode button and the
mode indicator show the decoder's name. Leaving the mode stops the app. Keep the
DECODE screen for decodes only — WSJT-X style lines such as
`1859 -15  2.6  997 :  VK7MO K6QPV DM12` — and put status text on the app window
(`icap_set_line`) or in a log file.

**App config (`<NAME>.CFG`).** An app may read a text file named after it (e.g.
`Q65.CFG`) from its folder with the normal file API, one `key=value` per line.
The reference decoders use `flo=`/`fhi=` (tone-0 search range, Hz), `sub=` (JT4/Q65
submode) and `scope=<lo>-<hi>` or `scope=<Hz>` (the DECODE mini-scope span, Hz).
These are app conventions, not firmware — pick your own keys.

### Station identity & radio state (read-only virtual files)
```c
/* opened with the normal file API; each returns short UTF-8 text, empty if not set */
icap_open("@MYCALL", ICAP_O_READ);   /* the radio's MY call sign                      */
icap_open("@MYLOC",  ICAP_O_READ);   /* 6-char Maidenhead locator from the radio's    */
                                     /*   position (GPS or a manual entry); empty = none */
icap_open("@CLOCK",  ICAP_O_READ);   /* clock lock + time to the next TX slot           */
icap_open("@FREQ",   ICAP_O_READ);   /* the true on-air frequency (Hz), transverter-aware */
icap_open("@TX",     ICAP_O_READ);   /* TX / beacon state (see Transmit)                 */
icap_open("@SPOT",   ICAP_O_WRITE);  /* report a decode to PSKReporter (see Spotting)    */
```
Each read file is one short line ending in `\n`; the `k=v` fields are separated by spaces:

| File | Example | Fields |
|---|---|---|
| `@MYCALL` | `SQ6EMM` | the MY call sign; empty = not set |
| `@MYLOC` | `JO81mc` | 6-char locator from the radio's position; empty = no valid position |
| `@CLOCK` | `K=… S=… L=1 P=60 O=0 N=…` | `L` 1 = clock locked; `P`/`O` the active TX period / offset (s); `N` ms to the next slot |
| `@FREQ` | `f=1296174000 usb=1 rx=main trv=0` | true on-air dial in Hz (transverter offset applied), receiver, active transverter slot (0 = none) |
| `@TX` | `st=0 err=0 n=3 … beacon=1 owner=0 clk=1 gate=0` | `beacon` 1 = the owner armed the beacon; `owner` 1 = the owner stopped TX (no TX until the app restarts); `err` reason of the last abort; `gate` 0 = TX would be allowed now; `n` transmissions so far |

Parse fields by name (more may be added); ignore what you do not know.
These let a beacon or a spotting app read who/where the station is and what it is on,
without the app storing any of it. A name starting with `@` is reserved: firmware
without this feature rejects it with `ICAP_E_NAME`, so probe once and fall back to
your own config keys (e.g. `call=`, `loc=`) if it is absent.

### Transmit (gated)
```c
int  icap_tx_available(void);                /* 1 if this firmware offers TX */
int  icap_tx_begin(void);                    /* key TX; <0 if owner hasn't enabled it */
int  icap_tx_write(const int16_t *buf, int nsamp); /* 8 kHz mono -> modulator */
void icap_tx_end(void);                      /* unkey                        */
```
**The owner arms the beacon on the radio:** in a decoder mode, a *long press* of the mode tile in
the emission popup arms or disarms it, and while it is armed a red frame marks the main-screen
emission button. A decoder app that can also transmit polls `@TX` about once a second and switches
between decoding and transmitting when `beacon=` changes; `icap_tx_begin()` returns
`ICAP_E_PERM` while it is not armed. A `beacon=1` key in the app's own `.CFG` is a bench
override only.

TX is gated: the radio's TX safety always applies — PTT lock, the
TX time-out timer, band/out-of-band limits and any transverter inhibit can refuse or
end a transmission at any moment, and STOP / EXIT / the mic PTT end it immediately.
You set nothing about the radio: the owner tunes to a legal frequency and sets the
power and the DATA modulation source; you only key and feed 8 kHz audio. Transmission
is **period-aligned** — the firmware keys at the mode's time-slot boundary, so the
radio clock must be set (the beacon waits for the clock to lock). Build the audio
with your own encoder; the station call and locator come from `@MYCALL` / `@MYLOC`.
`icap_tx_begin()` returns the milliseconds until the radio keys (the period start), or an
error (`ICAP_E_PERM` not armed, stopped by the owner or the session TX limit used up,
`ICAP_E_BUSY` radio not ready or clock not locked yet); then stream the whole transmission with `icap_tx_write()` and end with
`icap_tx_end()`. If the owner keys the radio, presses STOP/EXIT or changes band/mode, the write
fails and `@TX` reports `owner=1`.

### Spotting (PSKReporter) - `@SPOT`
A decoder reports each decode by writing **one line** to the write-only virtual file `@SPOT`:
```c
int fd = icap_open("@SPOT", ICAP_O_WRITE);   /* ICAP_E_NAME: firmware without spotting */
icap_write(fd, "SR6LEG JO81 802 Q65 12\n", 23); /* <call> <grid|-> <audio tone Hz> <mode> <snr> */
icap_close(fd);
```
The firmware does the rest: it adds the true on-air frequency (dial from `@FREQ` + the tone),
the UTC time and the station identity, queues the spots and sends them to PSKReporter
(IPFIX over UDP, `report.pskreporter.info:4739`) - only when `@MYCALL` and `@MYLOC` are set and
the radio has network. Reports are batched: at the first write after 5 minutes, or at once when
8 different stations are queued. Use the ADIF mode name (`FT8`, `Q65`, `JT4`, ...) and the
sender's call exactly as decoded (`-` when the message carries no grid). The write itself may take
a moment when a batch is sent; failures are retried later and never reported to the app.

### Network (IC-705 WLAN station / IC-9700 wired LAN)
```c
int icap_net_status(void);                    /* 0 = connected to an AP with an IP */
int icap_net_open(const char *host, uint32_t port, uint32_t flags, uint32_t timeout_ms);
int icap_net_send(int h, const void *buf, uint32_t len, uint32_t timeout_ms);
int icap_net_recv(int h, void *buf, uint32_t len, uint32_t timeout_ms); /* 0 = closed */
int icap_net_close(int h);
int icap_net_error(void);                     /* last raw WLAN-module code (diagnostics) */
```
`host` is a name or a dotted IPv4 address, and must be a string in your app's own memory (a
string literal or a local buffer is fine). `ICAP_NET_UDP` opens a connected UDP
datagram socket (one `icap_net_send` = one datagram, no TLS) — for reporting/telemetry.
`ICAP_NET_TLS` gives an HTTPS-capable
TLS connection (encryption is done by the radio's WLAN module). At most 2
connections; anything left open is closed when the app exits. Calls return
`ICAP_E_NONET`, `ICAP_E_DNS`, `ICAP_E_CONN` or `ICAP_E_TIMEOUT` instead of blocking,
and STOP ends a pending call. Server certificates are not verified yet — don't
send secrets. On the **IC-9700** the same calls use the wired LAN port (DHCP or static
IP from the radio's network settings); there is no TLS there, so `ICAP_NET_TLS` returns
an error — use plain HTTP on a local network.

### Popup window
```c
int icap_popup(const char *text);             /* up to 7 lines x 31 chars, '\n' */
```
Shows the radio's own message window with a CLOSE button (e.g. "cannot connect").

Services added in v1rc (clock, audio, decode, TX, network, popup) may be absent on older
firmware; the wrappers return `ICAP_E_NOSYS` instead of crashing, so you can
probe with e.g. `icap_tx_available()` or `icap_audio_rate() > 0`.

---

## Writing & packaging an app

- Freestanding C, hard-float (VFP/NEON), ARM or Thumb. No libc beyond what you
  bundle; the API is your runtime.
- Export `int app_main(const struct icap_svc *)` as the entry point.
- Link your image with [`tools/app.ld`](tools/app.ld) and the entry stub
  [`tools/icap_start.S`](tools/icap_start.S), then pack it into a `*.APP` (header + image +
  relocations, CRC-checked) with [`tools/mkapp.py`](tools/mkapp.py). A complete example with the
  compiler flags is in [`examples/hello/`](examples/hello/) (`build.sh`).
- Copy the `*.APP` to the radio's app folder and start it from **MENU › SET ›
  Apps**.
- **Autostart:** to run an app automatically at power-on, put its file name on the
  first line of `AUTORUN.CFG` in the same folder (e.g. `Q65.APP`). It starts about
  15 s after boot, once per power cycle; the Apps status row then shows `(auto)` and
  STOP works as usual. Delete the file to turn autostart off.
- **Window and EXIT:** starting an app opens its full-screen window (title, your 8 text lines, live).
  By default EXIT (or a touch) stops the app. Pack with `--background` (header flag
  `ICAP_FLAG_BACKGROUND`) for apps that should keep running (decoders, beacons, downloads): then EXIT
  only hides the window, and tapping the app in the Apps menu shows it again.
- Apps that use the v1rc services are packed with header ABI **3** and need firmware
  with the v1rc loader; older loaders refuse them cleanly.

A small, relocatable `*.APP` runs on IC-705 (E/J) and IC-9700 alike; branch on
`icap_radio_id()` for model-specific behaviour. Large apps (the decoders) are built
per radio, because the app window, stack and data-region sizes differ.

---

## Status — the DIGI platform (v1rc)

Apps are pluggable **decoders** and **beacon transmitters** inside the radio's native UI.

- **RX decode** *(implemented, IC-705 E and IC-9700)*: `icap_audio_read()` → your decoder →
  `icap_decode_out()` on the native DECODE screen. Reference decoders: Q65-60, FT8, JT4, PI4 —
  see [`apps/`](apps/).
- **Decoder picker** *(implemented)*: the RTTY button cycles through the registered decoder modes.
- **TX beacons** *(implemented, confirmed on both radios)*: armed by the owner with a long press,
  red frame on the emission button, period-aligned, station identity from `@MYCALL` / `@MYLOC`.
  FT8, Q65, JT4 and PI4 transmit from inside their decoder apps; WSPR-2 is a beacon-only app.
- **Spotting** *(implemented, confirmed on both radios)*: `@SPOT` → PSKReporter, transverter-aware.
- **Autostart** *(implemented)*: `AUTORUN.CFG`.
- **Next:** a WSPR decoder; tighter duplicate suppression for spotting.
- **Performance:** very limited resources — fixed-point DSP, streaming (no full PCM buffers),
  minimal allocation, NEON where available.

On the IC-9700 the loader runtime is a file on the SD card that the firmware loads at start-up, so
it can be updated without reflashing; always copy the runtime files together with the apps bundle
of the same release.

---

## License & notices

The SDK — the [`icap.h`](icap.h) header and this documentation — is **MIT** licensed
(see [`LICENSE`](LICENSE)). MIT keeps the header conflict-free, so apps under any
license (including GPL decoders derived from WSJT-X) can include it. **Apps under
[`apps/`](apps/) carry their own licenses** — see each app directory's own `LICENSE`.

- This repository is **original work**. It contains **no Icom code** and grants
  **no rights to any Icom firmware**; it only describes an application interface to
  independently developed custom firmware.
- *Icom*, *IC-705* and *IC-9700* are trademarks of **Icom Inc.** This project is
  **independent and not affiliated with, authorized by, or endorsed by Icom**.
- Provided as is, without warranty. Modifying radio firmware is done at your own
  risk and your own responsibility for complying with local regulations and your
  amateur-radio licence. The reference firmware/loader is published separately.
