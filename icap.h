/*
 * icap.h - Icom App SDK (ICAP) public API for IC-705 / IC-9700 custom firmware.
 *
 * An ICAP app is a single *.APP file on the SD card. The radio's custom firmware
 * loads it into a sandboxed, lowest-priority task and calls your entry point:
 *
 *     #include "icap.h"
 *     int app_main(const struct icap_svc *svc)
 *     {
 *         icap_use(svc);                 // once, before any icap_* call
 *         icap_print("hello");
 *         while (!icap_should_stop()) {
 *             ... ;
 *             icap_sleep_ms(50);         // or icap_yield()
 *         }
 *         return 0;
 *     }
 *
 * You never touch raw memory addresses or call firmware functions directly: the
 * loader mediates everything through the service table below, and the icap_*()
 * wrappers in this header are all you need.
 *
 * Status: v1rc. The API has few users and may still change before v1.0.
 *
 * Copyright (c) 2026 Dawid SQ6EMM.
 * SPDX-License-Identifier: MIT
 *
 * This header is original work. It contains no Icom code and grants no rights to
 * any Icom firmware. "Icom", "IC-705", "IC-9700" are trademarks of Icom Inc.;
 * this project is independent and not affiliated with or endorsed by Icom.
 */
#ifndef ICAP_H
#define ICAP_H

#include <stdint.h>
#include <stddef.h>

/* ---- app file (*.APP) header ------------------------------------------- */

#define ICAP_MAGIC        0x50414349u   /* 'ICAP' */
#define ICAP_ABI_VERSION  3u            /* v1rc; needs v1rc firmware (older loaders refuse it) */
#define ICAP_HDR_SIZE     64u
/* v1rc mode registration: an ABI 3 app may carry a 32-byte extension after the 64-byte header
 * (hdr_size = ICAP_HDR_SIZE_EXT) declaring up to 4 decoder modes.  The loader reads it when it scans
 * the Apps folder, without starting the app; the RTTY button then cycles RTTY -> RTTY-R -> the
 * registered modes, and selecting one starts the app (icap_mode() tells it which). */
#define ICAP_HDR_SIZE_EXT 96u
#define ICAP_RMODE_KEEP   0u               /* leave the radio's mode alone                        */
#define ICAP_RMODE_USBD   1u               /* switch to USB-D and show the DECODE screen          */
struct icap_mode_decl { char name[6]; uint8_t radio_mode; uint8_t flags; };   /* name: up to 4 chars used */
struct icap_header_ext { struct icap_mode_decl mode[4]; };                   /* 32 bytes */


/* header.flags */
#define ICAP_FLAG_SPLIT     0x1u        /* two-segment image (large .rodata)   */
#define ICAP_FLAG_AUTOSTART 0x2u        /* auto-run at boot / SD mount (opt-in) */
#define ICAP_FLAG_BACKGROUND 0x4u       /* EXIT hides the window, the app keeps running */

/* header.models bitmask / svc.radio_id */
#define ICAP_MODEL_IC705   0x1u
#define ICAP_MODEL_IC9700  0x2u
#define ICAP_RADIO_IC705   0x0705u
#define ICAP_RADIO_IC9700  0x9700u

struct icap_header {
    uint32_t magic;        /* ICAP_MAGIC                                     */
    uint16_t abi_version;  /* ICAP_ABI_VERSION                              */
    uint16_t hdr_size;     /* ICAP_HDR_SIZE                                 */
    uint32_t models;       /* bitmask of ICAP_MODEL_*                       */
    uint32_t fw_build;     /* 0 = any build, else must equal svc->fw_build  */
    uint32_t load_size;    /* image bytes (multiple of 4)                   */
    uint32_t bss_size;     /* zeroed bytes after the image                  */
    uint32_t entry_off;    /* entry offset in the image (bit0 = Thumb)      */
    uint32_t reloc_count;  /* u32 offsets after the image                   */
    uint32_t stack_size;   /* minimum stack the app needs                   */
    uint32_t crc32;        /* over image + relocs (+ seg2)                  */
    char     name[16];     /* NUL-padded display name                       */
    uint32_t flags;        /* ICAP_FLAG_*                                   */
    uint32_t seg2_size;    /* split image: bytes of segment 2 (.rodata)     */
};

/* ---- plain data structures --------------------------------------------- */

#define ICAP_O_READ   0    /* existing file, read only          */
#define ICAP_O_WRITE  1    /* create or truncate, write only    */
#define ICAP_O_ROOT   0x10 /* OR-flag: SD card root, not the app folder (v1rc) */
#define ICAP_O_RADIO  0x20 /* OR-flag: the radio's folder (\\IC-705, \\IC-9700) - firmware updates */
#define ICAP_NAME_MAX 48   /* longest file name                 */

#define ICAP_E_ARG     (-1) /* bad argument / pointer not in app memory     */
#define ICAP_E_NOFD    (-2) /* too many open files (max 4; 2 before v1rc 20261117v11)                  */
#define ICAP_E_IO      (-3) /* file-system error                            */
#define ICAP_E_NAME    (-4) /* bad file name                                */
#define ICAP_E_PERM    (-5) /* not permitted (e.g. TX not enabled by owner) */
#define ICAP_E_NOSYS   (-6) /* service not provided by this firmware        */
#define ICAP_E_OVERRUN (-7) /* audio: samples were lost since the last read */
#define ICAP_E_BUSY    (-8) /* TX: already transmitting / radio not ready   */
#define ICAP_E_NONET   (-9) /* WLAN off / not connected to an access point  */
#define ICAP_E_DNS    (-10) /* host name could not be resolved              */
#define ICAP_E_CONN   (-11) /* connect or TLS handshake failed              */
#define ICAP_E_TIMEOUT (-12)/* no progress within the timeout               */

#define ICAP_NET_TLS   0x1  /* icap_net_open: TLS connection (HTTPS)        */
#define ICAP_NET_NOSNI 0x2  /* TLS without sending/checking the host name   */

/* Layout below is binary ABI - do not reorder. */
struct icap_meminfo {
    uint32_t app_base;     /* start of the app window (image loaded here)   */
    uint32_t app_limit;    /* end of the window (exclusive)                 */
    uint32_t image_end;    /* app_base + image size                         */
    uint32_t bss_end;      /* image_end + bss size                          */
    uint32_t stack_lo;     /* your stack, lowest address                    */
    uint32_t stack_hi;     /* your stack, initial SP (exclusive top)        */
    uint32_t free_lo;      /* spare part of the window you may use:         */
    uint32_t free_hi;      /*   [free_lo, free_hi)                          */
};

struct icap_radio {
    uint32_t freq_hz;      /* MAIN operating frequency, Hz                  */
    uint32_t sub_freq_hz;  /* SUB receiver (IC-9700), 0 on the IC-705       */
    uint8_t  mode;         /* radio's internal mode code - display/log only */
    uint8_t  is_tx;        /* 1 while transmitting                          */
    uint8_t  reserved[2];
};

struct icap_time {         /* radio local clock (RTC), as set by the owner  */
    uint16_t year;         /* full year, e.g. 2026                          */
    uint8_t  mon;          /* 1..12                                         */
    uint8_t  day;          /* 1..31                                         */
    uint8_t  hour;         /* 0..23                                         */
    uint8_t  min;          /* 0..59                                         */
    uint8_t  sec;          /* 0..59                                         */
    uint8_t  wday;         /* 0 = Sunday .. 6                               */
};

/* ---- service table (filled by the loader) ------------------------------ */
/* You rarely touch this directly - use the icap_*() wrappers below.        */

struct icap_svc {
    uint32_t abi_version;                        /* ICAP_ABI_VERSION on this fw */
    uint32_t radio_id;                           /* ICAP_RADIO_*               */
    uint32_t fw_build;                           /* firmware build id          */
    uint32_t size;                               /* sizeof(*svc) on this fw    */

    /* text panel (Apps / DECODE screen) */
    void (*print)(const char *s);
    void (*set_line)(int line, const char *s);
    void (*clear)(void);

    /* files in the radio's app folder (8.3 names) */
    int  (*f_open)(const char *name, int mode);
    int  (*f_read)(int fd, void *buf, uint32_t len);
    int  (*f_write)(int fd, const void *buf, uint32_t len);
    int  (*f_close)(int fd);

    /* time / scheduling */
    void     (*sleep_ms)(uint32_t ms);
    void     (*yield)(void);
    uint32_t (*tick_ms)(void);

    /* info / control */
    int      (*mem_info)(struct icap_meminfo *mi);
    int      (*radio_state)(struct icap_radio *rs);
    int      (*should_stop)(void);
    uint32_t (*task_id)(void);

    /* large separate heap (0 if none) */
    uint32_t heap_base;
    uint32_t heap_size;

    /* ---- v1rc additions (present only when size covers the field) ---- */

    int  (*clock_now)(struct icap_time *t);      /* radio RTC                  */

    /* audio is mono signed 16-bit at audio_rate() (8000 Hz) in both directions */
    int  (*audio_read)(int16_t *buf, int nsamp); /* samples read, 0 none, <0 err */
    int  (*audio_rate)(void);                    /* sample rate, Hz            */
    void (*decode_out)(const char *utf8);        /* text -> native DECODE screen */

    /* TX - gated: owner must enable it; a hard timeout and STOP end TX */
    int  (*tx_begin)(void);                      /* key TX; <0 if not allowed  */
    int  (*tx_audio_write)(const int16_t *buf, int nsamp); /* samples queued, <0 err */
    void (*tx_end)(void);                        /* drain queued audio, unkey  */

    /* network: the radio's WLAN in station mode; TCP, DNS and TLS are done by the
     * WLAN module. At most 2 connections per app; the loader closes leftovers. */
    int  (*net_status)(void);                    /* 0 = connected with an IP   */
    int  (*net_open)(const char *host, uint32_t port, uint32_t flags, uint32_t timeout_ms);
    int  (*net_send)(int h, const void *buf, uint32_t len, uint32_t timeout_ms);
    int  (*net_recv)(int h, void *buf, uint32_t len, uint32_t timeout_ms);
    int  (*net_close)(int h);
    int  (*net_error)(void);                     /* last raw WLAN-module code  */

    /* modal message window (radio's own popup with a CLOSE button) */
    int  (*popup)(const char *text);

    /* delete a file (app folder, or the SD card root with ICAP_O_ROOT) */
    int  (*remove)(const char *name, int flags);

    /* registered decoder mode the owner selected with the RTTY/EXTRA button (see ICAP_HDR_SIZE_EXT) */
    int  (*mode)(char *buf, int max);
};

typedef int (*icap_entry_t)(const struct icap_svc *svc);

/* ---- wrappers ---------------------------------------------------------- */

/* One shared pointer for all translation units of the app (weak = defined once). */
__attribute__((weak)) const struct icap_svc *icap__svc;

/* Call once at the top of app_main() before any other icap_* call. */
static inline void icap_use(const struct icap_svc *svc) { icap__svc = svc; }

/* True if this firmware provides a given v1rc service. */
#define ICAP_HAS(field) \
    (icap__svc->size >= offsetof(struct icap_svc, field) + sizeof(icap__svc->field) \
     && icap__svc->field)

/* identity */
static inline uint32_t icap_radio_id(void) { return icap__svc->radio_id; }
static inline uint32_t icap_fw_build(void) { return icap__svc->fw_build; }
static inline int      icap_is_ic705(void) { return icap__svc->radio_id == ICAP_RADIO_IC705; }
static inline int      icap_is_ic9700(void){ return icap__svc->radio_id == ICAP_RADIO_IC9700; }

/* screen */
static inline void icap_print(const char *s)              { icap__svc->print(s); }
static inline void icap_set_line(int line, const char *s) { icap__svc->set_line(line, s); }
static inline void icap_clear(void)                       { icap__svc->clear(); }

/* files */
static inline int  icap_open(const char *name, int mode)            { return icap__svc->f_open(name, mode); }
static inline int  icap_read(int fd, void *buf, uint32_t len)       { return icap__svc->f_read(fd, buf, len); }
static inline int  icap_write(int fd, const void *buf, uint32_t len){ return icap__svc->f_write(fd, buf, len); }
static inline int  icap_close(int fd)                              { return icap__svc->f_close(fd); }

/* time / scheduling */
static inline void     icap_sleep_ms(uint32_t ms) { icap__svc->sleep_ms(ms); }
static inline void     icap_yield(void)           { icap__svc->yield(); }
static inline uint32_t icap_ticks(void)           { return icap__svc->tick_ms(); }

/* info / control */
static inline int      icap_mem_info(struct icap_meminfo *mi)  { return icap__svc->mem_info(mi); }
static inline int      icap_radio_state(struct icap_radio *rs) { return icap__svc->radio_state(rs); }
static inline int      icap_should_stop(void)                  { return icap__svc->should_stop(); }
static inline uint32_t icap_task_id(void)                      { return icap__svc->task_id(); }

/* large heap (returns 0 if this firmware grants none) */
static inline void    *icap_heap(void)      { return (void *)(uintptr_t)icap__svc->heap_base; }
static inline uint32_t icap_heap_size(void) { return icap__svc->heap_size; }

/* local clock (v1rc) */
static inline int icap_clock_now(struct icap_time *t) {
    return ICAP_HAS(clock_now) ? icap__svc->clock_now(t) : ICAP_E_NOSYS;
}

/* RX audio (v1rc). icap_audio_read returns samples read, 0 if none, <0 on error. */
static inline int icap_audio_read(int16_t *buf, int nsamp) {
    return ICAP_HAS(audio_read) ? icap__svc->audio_read(buf, nsamp) : ICAP_E_NOSYS;
}
static inline int icap_audio_rate(void) {
    return ICAP_HAS(audio_rate) ? icap__svc->audio_rate() : ICAP_E_NOSYS;
}

/* decoded text to the native DECODE screen (v1rc) */
static inline int icap_decode_out(const char *utf8) {
    if (!ICAP_HAS(decode_out)) return ICAP_E_NOSYS;
    icap__svc->decode_out(utf8);
    return 0;
}

/* TX (v1rc, gated). Pattern: icap_tx_begin() -> icap_tx_write()... -> icap_tx_end(). */
static inline int icap_tx_available(void) { return ICAP_HAS(tx_begin) ? 1 : 0; }
static inline int icap_tx_begin(void) {
    return ICAP_HAS(tx_begin) ? icap__svc->tx_begin() : ICAP_E_NOSYS;
}
static inline int icap_tx_write(const int16_t *buf, int nsamp) {
    return ICAP_HAS(tx_audio_write) ? icap__svc->tx_audio_write(buf, nsamp) : ICAP_E_NOSYS;
}
static inline void icap_tx_end(void) {
    if (ICAP_HAS(tx_end)) icap__svc->tx_end();
}

/* Network (v1rc). Pattern: icap_net_status() == 0 -> h = icap_net_open(host, 443,
 * ICAP_NET_TLS, 15000) -> icap_net_send / icap_net_recv ... -> icap_net_close(h).
 * host is a name or dotted IPv4. send returns bytes sent; recv returns bytes read,
 * 0 when the server closed the connection, ICAP_E_TIMEOUT if nothing arrived. */
static inline int icap_net_status(void) {
    return ICAP_HAS(net_status) ? icap__svc->net_status() : ICAP_E_NOSYS;
}
static inline int icap_net_open(const char *host, uint32_t port, uint32_t flags, uint32_t timeout_ms) {
    return ICAP_HAS(net_open) ? icap__svc->net_open(host, port, flags, timeout_ms) : ICAP_E_NOSYS;
}
static inline int icap_net_send(int h, const void *buf, uint32_t len, uint32_t timeout_ms) {
    return ICAP_HAS(net_send) ? icap__svc->net_send(h, buf, len, timeout_ms) : ICAP_E_NOSYS;
}
static inline int icap_net_recv(int h, void *buf, uint32_t len, uint32_t timeout_ms) {
    return ICAP_HAS(net_recv) ? icap__svc->net_recv(h, buf, len, timeout_ms) : ICAP_E_NOSYS;
}
static inline int icap_net_close(int h) {
    return ICAP_HAS(net_close) ? icap__svc->net_close(h) : ICAP_E_NOSYS;
}
static inline int icap_net_error(void) {
    return ICAP_HAS(net_error) ? icap__svc->net_error() : 0;
}

/* Popup window (v1rc): up to 7 lines of up to 31 characters, split with '\n'.
 * Shown by the radio's UI with a CLOSE button; returns ICAP_E_BUSY if one is pending. */
static inline int icap_popup(const char *text) {
    return ICAP_HAS(popup) ? icap__svc->popup(text) : ICAP_E_NOSYS;
}

/* Delete a file (v1rc): same names as icap_open; flags 0 = app folder, ICAP_O_ROOT = SD card root.
 * Returns 0, ICAP_E_NAME, or ICAP_E_IO (e.g. the file does not exist). */
static inline int icap_remove(const char *name, int flags) {
    return ICAP_HAS(remove) ? icap__svc->remove(name, flags) : ICAP_E_NOSYS;
}

/* Decoder mode (v1rc): if the app was started by selecting one of its registered modes on the
 * RTTY/EXTRA button, copies that mode's name (e.g. "Q65") and returns its length; 0 otherwise. */
static inline int icap_mode(char *buf, int max) {
    return ICAP_HAS(mode) ? icap__svc->mode(buf, max) : 0;
}

#endif /* ICAP_H */
