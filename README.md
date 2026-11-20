# Icom App SDK — ICAP (IC-705 / IC-9700)

**ICAP** lets you write small apps for custom IC-705 / IC-9700 firmware. An app is a
single `*.APP` file on the SD card; the radio loads it into a sandboxed,
lowest-priority task and calls your entry point. Apps get a clean C API and never
touch raw memory or call firmware functions directly.

The whole API is in [`icap.h`](icap.h). This README explains it.

> **Status: v1rc.** Few users so far, so the API may still change before v1.0.
> Everything below describes the custom firmware release **20261119v14** for the IC-705 (E / J)
> and the IC-9700 (patcher: https://sq6emm.github.io/icom-custom-firmware/). The firmware image
> and the runtime files in the app folder of one release belong together - install them as a set.
> QSO mode (FT8 / Q65 / JT4) and APRS are both verified on hardware (IC-705 E and IC-9700).
> Build tools and an example are in [`tools/`](tools/) and [`examples/`](examples/).

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
These are app conventions, not firmware — pick your own keys. The settings the owner makes on the
radio itself (DECODE › SET) reach the app through `@MODECFG` (below); the reference decoders apply
their built-in defaults first, then `@MODECFG`, then the `.CFG` file, which wins.

### Station identity & radio state (read-only virtual files)
```c
/* opened with the normal file API; each returns short UTF-8 text, empty if not set */
icap_open("@MYCALL", ICAP_O_READ);   /* the radio's MY call sign                      */
icap_open("@MYLOC",  ICAP_O_READ);   /* 6-char Maidenhead locator from the radio's    */
                                     /*   position (GPS or a manual entry); empty = none */
icap_open("@CLOCK",  ICAP_O_READ);   /* clock lock + time to the next TX slot           */
icap_open("@FREQ",   ICAP_O_READ);   /* the true on-air frequency (Hz), transverter-aware */
icap_open("@TX",     ICAP_O_READ);   /* TX / beacon state (see Transmit)                 */
icap_open("@MODECFG",ICAP_O_READ);   /* the owner's settings for this decoder mode       */
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
| `@MODECFG` | `mode=JT4 sub=G pskr=1 flo=200 fhi=3000 gen=7` | the settings the owner chose on the radio for the decoder mode the app was started for (see below) |

Parse fields by name (more may be added); ignore what you do not know.
These let a beacon or a spotting app read who/where the station is and what it is on,
without the app storing any of it. A name starting with `@` is reserved: firmware
without this feature rejects it with `ICAP_E_NAME`, so probe once and fall back to
your own config keys (e.g. `call=`, `loc=`) if it is absent.

**`@MODECFG` - the per-mode settings from the radio** (release 20261119v13). On the DECODE screen
the owner sets each decoder mode under **SET** (SUBMODE, PSKREPORT); the radio keeps these values
and hands them to the app as one line, the fields in this order, separated by single blanks:

```
mode=<NAME> sub=<A..|-> pskr=<0|1> [flo=<Hz> fhi=<Hz>] [scope=<lo>-<hi>] gen=<n>
```

| Field | Meaning |
|---|---|
| `mode` | name of the decoder mode (up to 4 characters) |
| `sub` | submode letter (JT4 `A`..`G`, Q65 `A`..`E`), `-` for a mode without submodes |
| `pskr` | 1 = PSKReporter spotting on, 0 = off (the firmware itself drops `@SPOT` lines while it is 0) |
| `flo`, `fhi` | tone search range in Hz - present only when set |
| `scope` | span of the DECODE mini-scope in Hz - present only when set |
| `gen` | a counter that changes whenever any value of the line changes |

The file is read-only. Read it at start and about once a second while idle, and compare `gen` (or
the whole line) to follow a change the owner makes while the app runs. `ICAP_E_NAME` means there is
nothing to read - older firmware, the runtime files of the release not installed, or an app that
was not started as a decoder mode: keep your defaults and your own `.CFG` keys.

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

### QSO mode (owner-driven) - `@QSO`, `@QTX0` / `@QTX1`, `@QTXC`, `@QLOG`
*New in release 20261119v13, verified on hardware (IC-705 E and IC-9700) in 20261119v14.*

In a decoder mode the owner can work a QSO from the DECODE screen. **An app never sends a QSO
message on its own:** every transmission needs a tap by the owner, and the firmware - not the
app - decides whether a tap counts. The reference apps (FT8, JT4, Q65) do this through
[`apps/digi-common/qso.h`](apps/digi-common/qso.h): a tap on a decoded line makes the app print the
next message of the standard sequence as a prompt (`TX? SP6ABC SQ6EMM JO81`); a tap on that `TX?`
line sends it once, in the period opposite to the tapped station's. CQ calling is armed on the
radio (hold the mode tile twice as long as for the beacon: red frame, then green). A green frame
on the emission button means CQ is armed or a QSO transmission is armed / on the air; the red
frame stays the beacon.

Four more virtual files carry this. All are opened with `ICAP_O_READ` and answer with one short
line; opening `@QTX*` or `@QLOG` *is* the request. Firmware without QSO mode returns `ICAP_E_NAME`.

| File | Answer | What it does |
|---|---|---|
| `@QSO` | `n=<taps> cq=<0\|1> q=<0\|1> t=<text>` | Nothing - it is the state to poll (the reference apps: every 300 ms). `n` counts the owner's taps on a text row of the DECODE screen (wraps at 65536); `t` is the tapped row as drawn (up to 63 characters, trailing blanks cut; it is the last field and contains blanks); `cq` 1 = the owner armed CQ; `q` 1 = a QSO / CQ transmission is armed or on the air. |
| `@QTX0`, `@QTX1` | `r=<ms>` or `r=-<code>` | Asks for **one** transmission in the even (`0`) or odd (`1`) period of the mode (FT8: :00 / :30 = even, :15 / :45 = odd; 60 s modes: the parity of the minute). `r >= 0`: armed, the first sample of the period is due in `r` ms. `r=-5` (`ICAP_E_PERM`): no valid tap (see the rules), a beacon is armed, the app is not a QSO-capable decoder mode, or the owner aborted and has not tapped since. `r=-8` (`ICAP_E_BUSY`): the TX gate refuses - clock not locked, not USB-D, modulation source, transmitter busy, ... |
| `@QTXC` | as `@QTX0` | The same for the CQ period; granted only while the owner has CQ armed (`r=-5` otherwise). The first grant takes the next period, later ones keep its parity. |
| `@QLOG <record>` | `r=0` or `r=-<code>` | Appends `<record>` + CR LF to the ADIF log `ICAPQSO.ADI` in the app folder (a new or empty file gets an ADIF header first). The record is the text after the blank, 1..240 characters, e.g. `<CALL:6>SP6ABC <MODE:3>FT8 ... <EOR>`. `r=-8` while a transmission is armed, `r=-3` on a file error - the file is then untouched and the request can be repeated. |

After a grant, do **not** call `icap_tx_begin()`: write the audio with `icap_tx_write()` starting
about 1 s before the period (the write blocks until the period starts; the firmware keys the radio
itself and gives up if the first sample is not there 300 ms after the period start), then call
`icap_tx_end()`. To cancel an armed transmission, call `icap_tx_end()` without writing.

```c
char b[104]; int fd, n;
fd = icap_open("@QSO", ICAP_O_READ);              /* poll: did the owner tap a row? */
if (fd >= 0) { n = icap_read(fd, b, sizeof b - 1); icap_close(fd); /* parse n=, cq=, t= */ }
...
fd = icap_open("@QTX1", ICAP_O_READ);             /* after the owner confirmed: one TX, odd period */
if (fd >= 0) { n = icap_read(fd, b, sizeof b - 1); icap_close(fd); /* "r=<ms>" or "r=-<code>" */ }
```

**Rules the firmware enforces** (an app cannot get around them):

- **One tap, at most one answer.** A tap gives the app credit for one granted transmission, and
  only if no transmission was armed or on the air when it was made. (Such a tap is still reported
  in `@QSO`, so an app can use it to cancel an armed answer.)
- **An owner abort latches.** PTT, EXIT or TRANSMIT end the transmission and void the credit;
  nothing is granted again until the owner taps a row *after* the abort.
- **A tap is a tap on a text row.** A touch that began elsewhere (a finger dragged from a soft key
  onto a row) does not count, and nothing is armed while a finger is held on the mode tile.
- **One period, same gate as the beacon.** A granted transmission is a single period under the
  full TX gate of [Transmit](#transmit-gated): clock lock, band / mode / PTT / time-out-timer
  checks, the per-transmission cap, the session TX limit while CQ is armed (without CQ each
  tap-confirmed answer is its own session).
- **Beacon and CQ are never armed together**, and taps are not delivered to the app while a beacon
  is armed.
- **The log is append-only.** `ICAPQSO.ADI` is opened as it is and written at its end; it is
  created only when it does not exist and is never truncated. Nothing is written to it while a
  transmission is armed.
- Spots are not sent to PSKReporter while a transmission is armed or on the air.

What the reference apps add on their side, and what your app should do too: confirm a `TX?` tap
only when the prompt has been on the screen for about a second (a prompt that appears under the
finger is not a decision); drop taps made during a transmission; keep SD-card work out of the path
between leaving the decoder and the first audio sample; repeat a refused log write (every 5 s, 6
times) and tell the owner if the QSO could not be logged.

### APRS - a firmware feature (release 20261119v14)
*Verified on hardware (IC-705 E and IC-9700).* APRS is built into the firmware, not a reference app
in this SDK: **MENU › SET › GPS › GPS TX Mode** gains a fourth choice, **APRS** (next to OFF /
D-PRS / NMEA). With it the loader starts and stops the bundled `APRS.APP` by itself - the owner
does not open it from the app list. `APRS.APP` sends a 1200 Bd AFSK (Bell 202) position beacon on
the current frequency (FM only), using the same gated TX path as the beacon above
(`icap_tx_begin` / `icap_tx_write` / `icap_tx_end`), and feeds received stations into the radio's
own RX History; frames are also written to `Apps\APRS.LOG`. The station identity comes from the
radio (`@MYCALL` and the GPS/manual position); nothing is configured in the app.

The interface it uses is already in this SDK - gated TX, `@MYCALL` / `@MYLOC` / `@FREQ`, the app
folder - so you can write your own packet app the same way. Two notes from the on-air app:
**receive needs NR (Noise Reduction) off** on that receiver, because NR distorts the AFSK tones and
nothing is decoded; and on the IC-9700 the decoder runs on **MAIN** only. On the IC-705 the
received stations ride the D-STAR receive path, so the firmware injects them only when that path is
free (never while the receiver is in DV or its header is busy).

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
the radio has network, and while the owner has PSKREPORT on for the mode (`pskr=` in `@MODECFG`).
Reports are batched: at the first write after 5 minutes, or at once when
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
  `icap_decode_out()` on the native DECODE screen. Reference decoders: Q65-60, FT8, JT4, PI4,
  WSPR-2 — see [`apps/`](apps/).
- **Decoder picker** *(implemented)*: the RTTY button cycles through the registered decoder modes.
- **TX beacons** *(implemented, confirmed on both radios)*: armed by the owner with a long press,
  red frame on the emission button, period-aligned, station identity from `@MYCALL` / `@MYLOC`.
  FT8, Q65, JT4, PI4 and WSPR-2 transmit from inside their decoder apps.
- **Per-mode settings** *(implemented)*: DECODE › SET (SUBMODE, PSKREPORT) → `@MODECFG`.
- **QSO mode** *(implemented in release 20261119v13 for FT8, Q65 and JT4; verified on hardware in
  20261119v14)*: tap a decode, confirm the `TX?` prompt, one transmission per tap; CQ; ADIF log
  `ICAPQSO.ADI`. See [QSO mode](#qso-mode-owner-driven---qso-qtx0--qtx1-qtxc-qlog).
- **Spotting** *(implemented, confirmed on both radios)*: `@SPOT` → PSKReporter, transverter-aware.
- **APRS** *(implemented in release 20261119v14, confirmed on IC-705 E and IC-9700)*: a fourth GPS
  TX Mode; the firmware runs the bundled `APRS.APP` (1200 Bd AFSK beacon + RX into RX History).
  See [APRS](#aprs---a-firmware-feature-release-20261119v14).
- **Autostart** *(implemented)*: `AUTORUN.CFG`.
- **Next:** tighter duplicate suppression for spotting.
- **Performance:** very limited resources — fixed-point DSP, streaming (no full PCM buffers),
  minimal allocation, NEON where available.

Parts of the loader runtime are files in the app folder on the SD card that the firmware loads at
start-up; they must come from the same release as the firmware image (a set that does not match is
refused, and the features it carries - among them `@MODECFG`, spotting and QSO mode - are then
absent). Always copy the runtime files and the apps bundle of one release together.

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
