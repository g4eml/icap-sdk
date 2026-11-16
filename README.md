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

### Files (radio's app folder, 8.3 names)
```c
int icap_open(const char *name, int mode);   /* ICAP_O_READ / ICAP_O_WRITE  */
int icap_read (int fd, void *buf, uint32_t len);
int icap_write(int fd, const void *buf, uint32_t len);
int icap_close(int fd);
```

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
Read a stream of received PCM and feed it to your decoder. Keep a small ring —
don't retain whole recordings.

### Decoded text out (native DECODE screen)
```c
int icap_decode_out(const char *utf8);       /* show text on the DECODE screen */
```

### Transmit (gated)
```c
int  icap_tx_available(void);                /* 1 if this firmware offers TX */
int  icap_tx_begin(void);                    /* key TX; <0 if owner hasn't enabled it */
int  icap_tx_write(const int16_t *buf, int nsamp); /* feed the modulator     */
void icap_tx_end(void);                      /* unkey                        */
```
TX is only possible when the owner has enabled app-TX in the radio menu. A hard
timeout and STOP always end transmission. You set nothing about the radio — the
owner tunes to a legal frequency; you only key and feed audio.

Services added in v1rc (clock, audio, decode, TX) may be absent on older
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
  Apps** (or mark it `ICAP_FLAG_AUTOSTART` to auto-run, if the owner enabled it).

The same `*.APP` runs on IC-705 (E/J) and IC-9700; branch on `icap_radio_id()`
for model-specific behaviour.

---

## Roadmap — the DIGI platform (v1rc)

The goal is a digital-modes platform: apps are pluggable **decoders** and
**beacon transmitters** inside the radio's native UI (the stock **RTTY** screen
becomes **DIGI**).

- **RX decode:** `icap_audio_read()` → your decoder → `icap_decode_out()` on the
  native DECODE screen. First target: **Q65** (then FT8 / JT4).
- **TX beacons:** generate a waveform → `icap_tx_begin / tx_write / tx_end`.
  First targets: **Q65**, **JT4G**. TX stays owner-gated with a timeout.
- **DIGI mode:** RTTY relabelled DIGI with a decoder picker (stock RTTY or an app).
- **Autostart:** an `ICAP_FLAG_AUTOSTART` app runs at power-on (owner opt-in) — a
  radio that boots straight into a Q65 monitor or beacon.
- **Performance:** very limited resources — fixed-point DSP, streaming (no full
  PCM buffers), minimal allocation, NEON where available.

Staging: **S1** DIGI label + `decode_out` · **S2** audio + real decode · **S3** TX
+ beacon · **S4** decoder picker.

---

## License & notices

**Noncommercial.** This SDK — the [`icap.h`](icap.h) header and this documentation —
is licensed under the **PolyForm Noncommercial License 1.0.0** (free for hobby,
research and educational use; commercial use is not granted). See [`LICENSE`](LICENSE).

- This repository is **original work**. It contains **no Icom code** and grants
  **no rights to any Icom firmware**; it only describes an application interface to
  independently developed custom firmware.
- *Icom*, *IC-705* and *IC-9700* are trademarks of **Icom Inc.** This project is
  **independent and not affiliated with, authorized by, or endorsed by Icom**.
- Provided as is, without warranty. Modifying radio firmware is done at your own
  risk and your own responsibility for complying with local regulations and your
  amateur-radio licence. The reference firmware/loader is published separately.
