# Icom App SDK — ICAP (IC-705 / IC-9700)

**ICAP** lets you write small apps for custom IC-705 / IC-9700 firmware. An app is a
single `*.APP` file on the SD card; the radio loads it into a sandboxed,
lowest-priority task and calls your entry point. Apps get a clean C API and never
touch raw memory or call firmware functions directly.

The whole API is in [`icap.h`](icap.h). This README explains it.

> **Status: v1rc.** Few users so far, so the API may still change before v1.0.

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
(packer: `--mode Q65`). On the radio's MODE screen the RTTY button then cycles
RTTY → RTTY-R → each registered decoder → RTTY. Selecting a decoder switches the
radio to USB-D, opens the DECODE screen and starts the app; the mode button and the
mode indicator show the decoder's name. Leaving the mode stops the app. Keep the
DECODE screen for decodes only — WSJT-X style lines such as
`1859 -15  2.6  997 :  VK7MO K6QPV DM12` — and put status text on the app window
(`icap_set_line`) or in a log file.

### Transmit (gated)
```c
int  icap_tx_available(void);                /* 1 if this firmware offers TX */
int  icap_tx_begin(void);                    /* key TX; <0 if owner hasn't enabled it */
int  icap_tx_write(const int16_t *buf, int nsamp); /* 8 kHz mono -> modulator */
void icap_tx_end(void);                      /* unkey                        */
```
TX is only possible when the owner has enabled app-TX in the radio menu. A hard
timeout and STOP always end transmission. You set nothing about the radio — the
owner tunes to a legal frequency; you only key and feed audio.

### Network (WLAN station)
```c
int icap_net_status(void);                    /* 0 = connected to an AP with an IP */
int icap_net_open(const char *host, uint32_t port, uint32_t flags, uint32_t timeout_ms);
int icap_net_send(int h, const void *buf, uint32_t len, uint32_t timeout_ms);
int icap_net_recv(int h, void *buf, uint32_t len, uint32_t timeout_ms); /* 0 = closed */
int icap_net_close(int h);
int icap_net_error(void);                     /* last raw WLAN-module code (diagnostics) */
```
`host` is a name or a dotted IPv4 address; `ICAP_NET_TLS` gives an HTTPS-capable
TLS connection (encryption is done by the radio's WLAN module). At most 2
connections; anything left open is closed when the app exits. Calls return
`ICAP_E_NONET`, `ICAP_E_DNS`, `ICAP_E_CONN` or `ICAP_E_TIMEOUT` instead of blocking,
and STOP ends a pending call. Server certificates are not verified yet — don't
send secrets.

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
- Link your image, then pack it into a `*.APP` (header + image + relocations,
  CRC-checked). A packer script and the loader are published with the reference apps.
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

The same `*.APP` runs on IC-705 (E/J) and IC-9700; branch on `icap_radio_id()`
for model-specific behaviour.

---

## Roadmap — the DIGI platform (v1rc)

The goal is a digital-modes platform: apps are pluggable **decoders** and
**beacon transmitters** inside the radio's native UI (the stock **RTTY** screen
becomes **DIGI**).

- **RX decode:** `icap_audio_read()` → your decoder → `icap_decode_out()` on the
  native DECODE screen. Q65-60, JT4, PI4 and FT8 decoders run on the IC-705 E *(implemented)*.
- **TX beacons:** generate a waveform → `icap_tx_begin / tx_write / tx_end`.
  First targets: **Q65**, **JT4G**. TX stays owner-gated with a timeout.
- **Decoder picker:** the RTTY button cycles through the registered decoder modes. *(implemented)*
- **Autostart:** `AUTORUN.CFG` on the SD card (owner opt-in) — a radio that boots
  straight into a Q65 monitor or beacon. *(implemented)*
- **Performance:** very limited resources — fixed-point DSP, streaming (no full
  PCM buffers), minimal allocation, NEON where available.

Status: S1 `decode_out`, S2 audio + real decode and S4 decoder picker are done on the
IC-705 E; the IC-9700 port of the v1rc loader is in progress; S3 TX + beacon is next.

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
