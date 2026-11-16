# Icom App SDK — ICAP (IC-705 / IC-9700)

**ICAP** is an application platform for custom IC-705 / IC-9700 firmware: small
`*.APP` programs on the SD card, loaded and run by the radio in a sandboxed,
lowest-priority task that never pre-empts RX / TX / CI-V / display / DSP.

This repository is the **API documentation**. The `icap_abi.h` header and the
reference apps are published separately; for now this repo holds the docs only.

Read this fully before writing code.

## 1. What an app is

- A single `*.APP` file in the radio's apps folder on the SD card:
  - IC-705: `\IC-705\Apps\`
  - IC-9700: `\IC-9700\Apps\`
- The user starts it from **MENU > SET > Apps** (up to 8 files are listed). The same screen
  shows the app's 8-line text panel and a STOP row.
- The loader copies the image into a fixed RAM window, applies relocations, zeroes BSS,
  syncs the caches and calls `app_main(const struct icap_svc *svc)` in a dedicated RTX
  task with the **lowest priority in the system** (below every stock task). Radio
  operation (RX, TX, CI-V, display, DSP) always takes precedence.
- The app gets a **service table** (`struct icap_svc`) for text output, files, time,
  memory info and read-only radio state. **No service can key the transmitter or change a
  radio setting**, and apps must never try to do either by other means.
- The same `.APP` binary can run on both radios (the loader relocates it). Use
  `svc->radio_id` to branch on radio-specific details.

### Firmware that contains the loader

| Radio | Release (opening-screen marker) | `svc->fw_build` | ABI | Loader |
|---|---|---|---|---|
| IC-9700 | `20261114v7` | `0x97001607` | **2 + split image** | yes |
| IC-9700 | `20261114v6` | `0x97001606` | 2 (large heap, single-segment) | yes |
| IC-9700 | `20261113v3`..`20261114v5` (and 1.6d, 1.6e, 1.6f, v2) | `0x97001604` | 1 | yes |
| IC-705 E | `20261113v1` (and 1.4E, 1.4F) | `0x07051405` | 1 | yes |
| IC-705 J | `20261114v1` | `0x0705140A` (`ICAP_FW_IC705J_14A`) | 1 | yes (same app window, same `.APP` files) |
| IC-705 J | `20261113v1` and older | — | — | **no loader** |

`fw_build` identifies the loader/memory-map generation, not the release; it changes only
when the app window or ABI changes. Header `fw_build = 0` means "any build". The IC-9700
`20261114v6` loader accepts both ABI v1 and ABI v2 apps; a v1 app sees exactly the old v1
service table (`size` `0x48`, no heap) and runs unchanged.
The IC-705 E and J builds have the same app window, service table and RAM layout (all §7
addresses are identical); only firmware ROM addresses differ, which apps must not use. An app
that needs to tell E from J compares `svc->fw_build` (SYSINFO v1.1 does this for one ROM
constant).

## 2. Hard rules (an app that breaks these can freeze the radio)

1. **Never write outside your own image, BSS and stack**, plus the window's free gap
   reported by `mem_info()` (`free_lo..free_hi`). All other memory belongs to the radio.
2. **Never disable interrupts**, never change CPU mode, MMU, caches (except as the loader
   does), the vector table, peripherals or GPIO. Never call firmware functions directly.
3. **Poll `svc->should_stop()` at least every 500 ms** and return from `app_main` when it
   returns 1. After 3 s without returning, the loader kills the task (state "App killed").
   Killing a task that is inside a file call is the one case that can leave the SD file
   system locked until reboot — so always close files before returning and keep file calls
   short.
4. **Don't busy-spin for long.** You run below every stock task, so you only get leftover
   CPU, but long loops without `sleep_ms()`/`yield()` also delay your own STOP handling.
   Call `yield()` or `sleep_ms(1+)` at least every ~10 ms of computation.
5. **Read-only access to radio RAM is allowed only for addresses documented in §7**,
   with plain aligned loads. Never write there.
6. **Never transmit, never touch TX-related state.** `radio_state()` tells you `is_tx`;
   if you process audio, just ignore data while `is_tx` is 1.
7. Services may only be called from the app task (not from an exception handler), and
   pointers you pass must point into your own app memory (the loader checks this and
   returns `ICAP_E_ARG`).

What the loader protects you from: data abort, prefetch abort and undefined-instruction
faults **in the app task** kill only the app (the panel shows `App crashed at …` with PC,
FSR and FAR). A stack overflow detected by the canary also kills the app. What it cannot
protect against: writing into radio memory, hanging with interrupts off, or holding a
file-system call forever.

## 3. Execution environment

| | IC-705 | IC-9700 |
|---|---|---|
| CPU | Cortex-A9 r3p0, 384 MHz | same |
| FPU | VFPv3-D32 + NEON (hard-float AAPCS) | same |
| App window | `0x20831000 .. 0x20849000` (96 KiB) | `0x207F2000 .. 0x20802000` (64 KiB) |
| App stack | top 16 KiB of the window (`0x20845000..0x20849000`) | `0x207FE000..0x20802000` |
| Image + BSS limit | window start .. stack start (80 KiB) | (48 KiB) |
| Free CPU (measured) | ~86 % idle | ~87 % idle |
| RTX task slots (measured) | 25 of 56 used | 39 of 55 used |
| Text panel | 8 lines × 31 characters | same |

- The CPU runs privileged (SYS mode) like all RTX tasks; there is **no memory
  protection** — the rules in §2 are your responsibility.
- The window is executable, cacheable, write-back. Everything you need must fit in it:
  code, rodata, data, BSS and the 16 KiB stack. Large buffers: put them in BSS, or in the
  window's free gap (`mem_info()`), never elsewhere.
- VFP/NEON: the start-up stub executes one VFP instruction first so the firmware's lazy
  FPU-enable path turns the coprocessor on for your task. Don't use NEON before
  `app_main` runs. VFP/NEON registers are saved per task.
- There is **no libc**: compile freestanding, provide your own small helpers (see
  `sysinfo.c` for `printf`-like formatting). `-lgcc` is linked for division/soft helpers.
  No `malloc` — use static buffers.
- Time: `tick_ms()` is the RTX millisecond clock (wraps after ~49 days). `sleep_ms(0)`
  is a yield.

## 4. Service table reference (`struct icap_svc`, ABI v1 / v2)

All functions use the AAPCS calling convention. Offsets are fixed.

| Off | Function | Description |
|---|---|---|
| 0x00 | `uint32_t abi_version` | `1` |
| 0x04 | `uint32_t radio_id` | `0x0705` or `0x9700` |
| 0x08 | `uint32_t fw_build` | see §1 |
| 0x0c | `uint32_t size` | sizeof the table (check before using services added later) |
| 0x10 | `void print(const char *s)` | Append a line to the 8-line panel, scrolling when full. Max 31 visible chars (longer is cut). |
| 0x14 | `void set_line(int line, const char *s)` | Overwrite panel line 0..7 (for status displays that don't scroll). |
| 0x18 | `void clear(void)` | Clear the panel. |
| 0x1c | `int f_open(const char *name, int mode)` | Open a file **in the apps folder only**. `name` is 8.3, characters `A-Z a-z 0-9 _ -`, one dot, no path. `ICAP_O_READ` (existing file) or `ICAP_O_WRITE` (create/truncate). Returns fd ≥ 0 or `ICAP_E_*`. Max **2** files open. |
| 0x20 | `int f_read(int fd, void *buf, uint32_t len)` | Bytes read (0 = EOF) or `<0`. |
| 0x24 | `int f_write(int fd, const void *buf, uint32_t len)` | Bytes written or `<0`. Write in chunks of ≤ 4 KiB. |
| 0x28 | `int f_close(int fd)` | Always close before returning. |
| 0x2c | `void sleep_ms(uint32_t ms)` | Block the app task. |
| 0x30 | `void yield(void)` | Let other tasks run. |
| 0x34 | `uint32_t tick_ms(void)` | Kernel ms counter. |
| 0x38 | `int mem_info(struct icap_meminfo *mi)` | Window, image, BSS, stack and free-gap addresses. |
| 0x3c | `int radio_state(struct icap_radio *rs)` | `freq_hz` (MAIN operating frequency, Hz), `sub_freq_hz` (IC-9700 SUB, 0 on IC-705), `mode` (internal mode code, not stable — use only for display/logging), `is_tx`. The frequency is the radio's tuned (IF) frequency; transverter RF is not applied. |
| 0x40 | `int should_stop(void)` | 1 once the user tapped STOP (or the app is being unloaded). |
| 0x44 | `uint32_t task_id(void)` | RTX task id of the app. |
| 0x48 | `uint32_t heap_base` | **ABI v2 only.** Base of the large heap, or `0`. Valid only when `abi_version >= 2` and `size >= 0x50`. |
| 0x4c | `uint32_t heap_size` | **ABI v2 only.** Size of the large heap in bytes, or `0` (then there is no heap on this radio/build). |

The service table is the same layout in v1 and v2; v2 only appends `heap_base`/`heap_size`.
Always gate their use on the table size, exactly as for any later-added field:

```c
if (svc->abi_version >= 2 && svc->size >= 0x50 && svc->heap_size) {
    uint8_t *heap = (uint8_t *)svc->heap_base;   /* svc->heap_size bytes, already zeroed */
    /* big resident buffers (e.g. Q65 arrays) go here */
}
```

The loader zeroes the whole heap on every app load (like BSS), so an app may assume it
starts clear. The heap is plain read/write data memory and is **execute-never** — put data
there, never code. It is accepted by the loader's pointer checks (so `f_read`/`f_write`
buffers, and strings passed to services, may live in the heap) for v2 apps only.

Error codes: `ICAP_E_ARG -1` (bad argument / pointer outside app memory),
`ICAP_E_NOFD -2` (more than 2 files), `ICAP_E_IO -3` (SD error, card missing),
`ICAP_E_NAME -4` (bad 8.3 name).

## 5. File format and toolchain

`*.APP` = 64-byte `struct icap_header` + segment-1 image + relocation table
(u32 offsets of 32-bit words holding image-relative addresses, `R_ARM_ABS32` only).
CRC-32 (IEEE, reflected) covers image + relocation table (+ segment 2 for split images).
`mkapp.py` builds all of it.

**Split image (ABI v2, IC-9700 `20261114v7`):** a v2 app may set `flags |= ICAP_FLAG_SPLIT`
and append a **segment 2** (`.rodata`, `seg2_size` bytes) after the relocation table. The file
is then `header + seg1 + relocs + seg2`, CRC over `seg1 + relocs + seg2`. Segment 2 is loaded
**verbatim at the fixed region base `0x209ad080`** and the app must be **fully linked at its
final addresses** (segment 1 at the window `0x207f2000`, segment 2 at `0x209ad080`) so that
`reloc_count == 0` and no runtime relocation is needed. This lets a large `.rodata` (e.g. Q65
const tables) live in the granted region instead of the 48 KiB window. `svc->heap_base` /
`heap_size` then point to the region **after** the 8-byte-aligned `.rodata`; only that part is
zeroed. See §7.

Header fields you choose: `name` (shown nowhere yet; use ≤ 15 chars), `models` bitmask
(`1` = IC-705, `2` = IC-9700, `3` = both), `fw_build` (0 = any), `stack_size` (minimum
stack you need; the loader always gives 16 KiB). The menu lists the **file name**, so name
files clearly (8.3, e.g. `FT8MON.APP`). For a split image also set `abi_version = 2`,
`flags = ICAP_FLAG_SPLIT`, `seg2_size` and target `fw_build 0x97001607` (or 0).

Build (on the remote host, in Docker, as for SYSINFO — see [`build_apps.sh`](build_apps.sh)):

```sh
CF="-mcpu=cortex-a9 -marm -mfpu=neon-vfpv3 -mfloat-abi=hard -O2 -ffreestanding -nostdlib \
    -fno-builtin -fno-pic -mword-relocations -fno-common -fno-tree-loop-distribute-patterns -Wall -Isdk"
LF="-nostdlib -T sdk/app.ld -Wl,--emit-relocs -Wl,--no-dynamic-linker"
# compile + link (image arm-gcc:latest)
arm-none-eabi-gcc $CF -c sdk/icap_start.S -o build/icap_start.o
arm-none-eabi-gcc $CF -I. -c myapp/myapp.c -o build/myapp.o
arm-none-eabi-gcc $CF $LF build/icap_start.o build/myapp.o -lgcc -o build/myapp.elf
# pack (any python3)
python3 sdk/mkapp.py build/myapp.elf MYAPP.APP --name MYAPP --models 3 --stack 8192
```

Requirements enforced by `mkapp.py`: `-mword-relocations` (no absolute MOVW/MOVT pairs),
no position-dependent data outside `R_ARM_ABS32`, `.ARM.exidx` discarded (no C++
exceptions). Keep the entry symbol `app_main` (called by `icap_start.S`).
`-fno-tree-loop-distribute-patterns` stops GCC from emitting calls to `memset`/`memcpy`
that don't exist; if you need them, write your own.

Loader checks (any failure shows an error on the Apps screen and nothing runs): magic,
ABI version, header size, model bit, `fw_build`, sizes fit the window, file size exact,
CRC, every relocation offset and target inside the image.

## 6. Minimal app

```c
#include "icap_abi.h"

int app_main(const struct icap_svc *svc)
{
    struct icap_radio rs;
    svc->clear();
    svc->print(svc->radio_id == ICAP_RADIO_IC9700 ? "Hello IC-9700" : "Hello IC-705");
    while (!svc->should_stop()) {
        if (svc->radio_state(&rs) == 0) {
            /* format rs.freq_hz yourself (no libc) and show it */
        }
        svc->sleep_ms(250);
    }
    return 0;
}
```

Writing a report file:

```c
int fd = svc->f_open("REPORT.TXT", ICAP_O_WRITE);
if (fd >= 0) { svc->f_write(fd, buf, len); svc->f_close(fd); }
```

## 7. Read-only raw data you may use (measured with SYSINFO)

These are stock-firmware buffers, read-only for apps, valid for the `fw_build` values in §1.
Read them with aligned loads; never write; treat them as **unstable** (formats inferred,
confirm before relying on them).

### Received audio

| Radio | Buffer | Layout | Status on the radio |
|---|---|---|---|
| IC-705 | `0x204014a2` "SSIF0-L slots" | 8 slots × 36 frames, 2-byte samples (s16), write slot index u8 at `0x204016e2` (0..7) | live with RX audio (RMS ≈ 6400) |
| IC-705 | `0x204e91f8` WLAN ring | 28800 frames × 4 bytes (48 kHz), write index u16 at `0x205053f8` | live, silent unless WLAN audio output is set to AF |
| IC-9700 | `0x204ade76` "recorder slots" | 8 slots × 36 frames, 4-byte frames, write slot index u8 at `0x204ae2f6` | live with RX audio (RMS ≈ 1600) |
| IC-9700 | `0x20573340` network ring | 28800 frames × 4 bytes (48 kHz, probably MAIN/SUB stereo), write index u16 at `0x2058f540` | live, silent without a LAN audio client |

Exact sample rates, channel order and scaling of the "slots" buffers are **not confirmed
yet** — the next planned app (AUDIOREC) records them to WAV to verify. Decoders (FT8,
Q65) typically need 12 kHz mono: low-pass filter and decimate 48 kHz → 12 kHz (a proper
FIR, not plain dropping).

### The ABI v2 large region (IC-9700 `20261114v6` heap, `20261114v7` split image)

An app whose header `abi_version` is `2` is granted, in addition to the 64 KiB window, a large
separate on-chip region. The whole region is `0x209ad080 .. 0x209fc000` (`0x4ef80` =
323456 B, ~316 KiB). How it is presented depends on whether the app is a split image:

| App | `heap_base` | `heap_size` | Region contents |
|---|---|---|---|
| v2 single-segment (v6/v7) | `0x209ad080` | `0x4ef80` (~316 KiB) | all heap (zeroed) |
| v2 **split** (v7, `ICAP_FLAG_SPLIT`) | `0x209ad080 + align8(seg2_size)` | `0x4ef80 − align8(seg2_size)` | `.rodata` at `0x209ad080` (loaded, kept), heap after it (zeroed) |
| v1 / `≤ 20261114v5` / IC-705 | `0` | `0` | no region |

The region sits **above the firmware's last scatter-loaded ZI section** (which ends exactly
at `0x209ad080`) and below the top-of-RAM firmware-update marker; static analysis of the main
image finds no firmware reference into it (an MMU L1 table is at `0x20000000`, not here), and
SYSINFO observed it untouched. A 16 KiB guard (`0x209fc000..0x20a00000`) is left clear of the
observed top-RAM writes.

A **split-image** app therefore has: segment 1 (`.text` + small `.data/.bss`) executing in the
48 KiB window, its const `.rodata` resident at `0x209ad080`, and `svc->heap_base/heap_size` as
the heap after the rodata. Example footprint (measured Q65, hard-float, size-opt): `.text`
~46 KiB (window), `.rodata` ~31 KiB (region base), heap ~175 KiB (region) — ~206 KiB of the
316 KiB region, comfortably within budget. Use the heap exactly as in §4; read `.rodata`
through its fixed link addresses at `0x209ad080`.

The heap part is zeroed on load, the region is execute-never data memory (put data, not code),
and the **whole** region is accepted by the loader's pointer checks for v2 apps. **Everything
else** — the RTX heap, the big ZI/DSP buffers, the SDRAM gaps and other "untouched" regions
SYSINFO reported — remains **off limits** to apps of either ABI version.

## 8. Testing and delivery checklist

1. Build in Docker on the remote host; never on the notebook.
2. Static checks: image + BSS + needed stack fit the window for every radio in `models`;
   no writes to addresses outside the app's own memory (grep the disassembly for stores to
   absolute addresses); `should_stop()` polled in every long loop.
3. Run the app logic on a PC first where possible (pure C parts: DSP, decoders).
4. Unicorn emulation of `app_main` against a modelled service table is how SYSINFO and the
   loader were tested (see `tests/` and the loader report); do the same for new apps.
5. On the radio: copy the `.APP` to `\IC-xxxx\Apps\`, MENU > SET > Apps, tap it. First
   runs on a dummy load. Test STOP during every phase. Don't power off, update firmware or
   remove the card while an app runs (IC-705: an app keeps running across a soft power-off).
6. Known loader limitations of the IC-705 E 1.4E..20261113v1 and IC-9700 builds (fixed in
   the shared loader source and in IC-705 J 20261114v1; E/9700 get the fixes with their next
   build): a forced kill during the final file close, or right after an `f_open`, could leave
   the SD file system busy or a handle unrecorded; the "busy" flag of the loader could clear
   a moment before the exiting task's RTX slot was free; on the IC-705 E the QUICK menu on an
   Apps row may show odd options (row ids past the stock action table).

## 9. Ideas already analysed

- **FT8/FT4 monitor**: `ft8_lib` (MIT, C) needs up to ~200 KB RAM in its default
  configuration — more than the ABI v1 image/BSS space (705: 80 KiB, 9700: 48 KiB). It needs
  a trimmed configuration or a larger app window (loader change); plus 12 kHz audio from §7
  and a time reference (705: GPS).
- **Q65 decoder** (microwave EME/scatter): WSJT-X code is GPLv3 and mostly Fortran — a C
  port is the main work. Build it as an IC-9700 `20261114v7` **split image**: `.text` (~46 KiB)
  in the 48 KiB window, const QRA/`.rodata` tables (~31 KiB) linked at `0x209ad080`, and the
  decoder heap (~175 KiB with SD-streaming) from `svc->heap_base`. ~206 KiB of the 316 KiB
  region. (ABI v2 is IC-9700 only for now; the IC-705 would need its own free-region analysis.)
- **Audio recorder / spectrum tools** (AUDIOREC): the next step to confirm audio formats.

---

# 10. DIGI platform — ICAP **v1rc** (planned, supersedes the ad-hoc v1/v2 split)

Status: **release candidate**. The API so far has ~2 users (the owner + one), so v1rc may
freely consolidate and break the earlier v1/v2 layout; there is no stable-ABI debt to carry.
Target: a *digital-modes platform* where SD apps are pluggable **decoders and beacon
transmitters** embedded in the radio's **native UI**. Stock mode "RTTY" is relabelled **DIGI**.

## 10.1 Requirements (confirmed with owner 2026-11-16)

- **R1 RX / decode:** app reads RX **PCM audio** (DSP/recorder tap, §7 buffers; format TBD in
  RE), decodes, and prints text to the **native DECODE screen** (reuse the RTTY text ring).
  Target decoders: **Q65** (then FT8/JT4). *APRS is OUT of scope.*
- **R2 TX / beacon:** app **generates a waveform** and injects it at the **TX modulator audio
  input** (same path USB audio uses for WSJT-X), and controls **PTT**. Target beacons: **Q65,
  JT4G**. **TX is GATED:** owner opt-in in a menu; hard **TX timeout**; the app must NOT change
  freq/mode (owner sets a legal frequency); STOP kills TX immediately; band-permit respected.
- **R3 UI / DIGI:** relabel **RTTY → "DIGI"** (mode string + DECODE title); decoder selection
  under DIGI (stock RTTY or an app decoder); reuse the native **DECODE screen**.
- **R4 common:** time from the **radio's local clock (RTC)**, NOT GPS; one `.APP` runs on
  **705 E/J + 9700**; RX stays sandboxed, **TX is a separate, explicit capability**.
- **R5 performance (critical — very few resources):** heavily optimized — **fixed-point /
  integer DSP** (avoid float where possible), **NEON** where present, **streaming** (no full
  PCM recording; a few-k-sample ring), minimal allocation, fit the window + ABI-v2 heap;
  Q65 = **reduced/streaming** decoder (full QRA65 ~1.7 MB will not fit); break long loops with
  `yield()`/`sleep_ms()`.
- **R6 autostart:** an app flagged autostart (`ICAP_FLAG_AUTOSTART` in the `.APP` header, or a
  fixed name `AUTO.APP`) is **auto-loaded at boot / SD mount**, gated by an owner menu opt-in
  ("Auto-run app: Off/On"). An autostarted TX app still obeys the R2 TX gate; STOP still works.

## 10.2 Service table additions (v1rc)

```c
/* RX audio in */
int  (*audio_read)(int16_t *buf, int nsamp);   /* samples read; 0 none; <0 err   */
int  (*audio_rate)(void);                       /* Hz, e.g. 12000 or 48000        */
/* decoded-text out -> native DECODE screen (RTTY text ring) */
void (*decode_out)(const char *utf8);
/* TX (GATED: opt-in + timeout + STOP) */
int  (*tx_begin)(void);                         /* request PTT/TX; <0 if not allowed */
int  (*tx_audio_write)(const int16_t *buf, int nsamp);
void (*tx_end)(void);                           /* PTT off                        */
/* local clock */
int  (*clock_now)(struct icap_time *t);         /* radio RTC; <0 if unset         */
```
`struct icap_time { uint16_t year; uint8_t mon,day,hour,min,sec; uint8_t _pad; };`

## 10.3 Staging

- **S1** relabel RTTY→DIGI + `decode_out` to the native DECODE screen (no audio; feed static/CI-V text) — proves the output path.
- **S2** `audio_read` (RX tap, confirmed format) → real Q65/streaming decode → `decode_out`.
- **S3** TX: `tx_begin`/`tx_audio_write`/`tx_end` + PTT + the R2 gate → Q65/JT4G beacon.
- **S4** decoder selection menu under DIGI.

Reference apps (a Q65 streaming decoder and a minute-aligned Q65 beacon) will be added
once the foundation work lands.
