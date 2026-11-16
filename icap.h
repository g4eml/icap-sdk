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
 * SPDX-License-Identifier: MIT
 */
#ifndef ICAP_H
#define ICAP_H

#include <stdint.h>
#include <stddef.h>

/* ---- app file (*.APP) header ------------------------------------------- */

#define ICAP_MAGIC        0x50414349u   /* 'ICAP' */
#define ICAP_ABI_VERSION  3u            /* v1rc service table */
#define ICAP_HDR_SIZE     64u

/* header.flags */
#define ICAP_FLAG_SPLIT     0x1u        /* two-segment image (large .rodata)   */
#define ICAP_FLAG_AUTOSTART 0x2u        /* auto-run at boot / SD mount (opt-in) */

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

#define ICAP_E_ARG   (-1)  /* bad argument / pointer not in app memory */
#define ICAP_E_NOFD  (-2)  /* too many open files               */
#define ICAP_E_IO    (-3)  /* file-system error                 */
#define ICAP_E_NAME  (-4)  /* bad 8.3 file name                 */
#define ICAP_E_PERM  (-5)  /* not permitted (e.g. TX not enabled by owner) */
#define ICAP_E_NOSYS (-6)  /* service not provided by this firmware */

struct icap_meminfo {
    uint32_t app_base, app_limit;   /* your window (image loaded here)   */
    uint32_t free_lo, free_hi;      /* spare gap in the window you may use */
    uint32_t stack_lo, stack_hi;    /* your stack                         */
};

struct icap_radio {
    uint32_t freq_hz;    /* current VFO frequency                */
    uint8_t  mode;       /* radio mode code                      */
    uint8_t  is_tx;      /* 1 while transmitting                 */
    uint8_t  _rsv[2];
};

struct icap_time {       /* radio local clock (RTC)              */
    uint16_t year;
    uint8_t  mon, day, hour, min, sec, _pad;
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

    int  (*audio_read)(int16_t *buf, int nsamp); /* RX PCM; samples read/0/<0  */
    int  (*audio_rate)(void);                    /* RX sample rate, Hz         */
    void (*decode_out)(const char *utf8);        /* text -> native DECODE screen */

    /* TX - gated: owner must enable it; a hard timeout and STOP end TX */
    int  (*tx_begin)(void);                      /* key TX; <0 if not allowed  */
    int  (*tx_audio_write)(const int16_t *buf, int nsamp);
    void (*tx_end)(void);                        /* unkey                      */
};

typedef int (*icap_entry_t)(const struct icap_svc *svc);

/* ---- wrappers ---------------------------------------------------------- */

static const struct icap_svc *icap__svc = 0;

/* Call once at the top of app_main() before any other icap_* call. */
static inline void icap_use(const struct icap_svc *svc) { icap__svc = svc; }

/* True if this firmware provides a given v1rc service. */
#define ICAP_HAS(field) \
    (icap__svc->size > offsetof(struct icap_svc, field) && icap__svc->field)

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

#endif /* ICAP_H */
