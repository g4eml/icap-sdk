/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Q65.APP for the IC-705 - Q65 monitor, T/R period 15, 30 or 60 s, submode A..E (ICAP ABI 3, split image,
 * decoder mode "Q65").
 *
 * Selected with the RTTY button (RTTY -> RTTY-R -> Q65), the loader switches the radio to USB-D,
 * opens the DECODE screen and starts this app in the background.  Every T/R period, without gaps:
 *   - from the period start (:00 for 60 s; :00/:30; :00/:15/:30/:45 - icap_clock_now, GPS-disciplined on the
 *     705) capture g_cap_s (default 55 / 28 / 14 s) of 8 kHz RX audio (icap_audio_read) to Q65CAPA.PCM /
 *     Q65CAPB.PCM alternately (raw s16); ICAP_E_OVERRUN gaps are zero-filled to their wall-clock length so the
 *     time base stays true;
 *   - when period N's capture ends its decode starts (q65b_decode_cb, 8000 Hz) and runs while period
 *     N+1 is captured: every decoder callback (PCM read, spectra read/append, log, abort poll) pumps the
 *     audio ring into the other file.  A decode still running when the NEXT capture ends is aborted
 *     (logged) and the next decode starts - no capture is ever lost.  A 60D decode takes ~3 s, no decode ~5 s;
 *     the shorter periods take proportionally less.
 *   - each decode -> DECODE screen, WSJT-X style "HHMM SNR DT FREQ MESSAGE" with the HHMM of its minute
 *     (HHMMSS of its period for 15 / 30 s).
 * Files open at once: capture write + PCM read + spectra tmp + one transient log write = 4 (loader v11).
 * Logs: Q65DBG.TXT (debug; buffered while a capture is open, flushed between captures or at most every
 * 10 s through the 4th file slot), Q65DEC.TXT (decode log).
 * Mode: the period is Q65.CFG "period=15|30|60" (default 60); the submode is the radio's own setting of this mode
 * (loader virtual file "@MODECFG", DECODE screen > SET > SUBMODE, "mode=Q65 sub=D ...", re-read before every decode)
 * unless Q65.CFG has "sub=A..E".  8 kHz audio cannot carry Q65-15D/15E (or 30E above ~320 Hz): the window says so.
 * Q65.CFG (optional, key=value): period= sub= flo= fhi= (Hz, <= 1200 Hz span) cap= (s) beacon= call= loc=
 * txfreq=; a file starting with a number is the old "fmin fmax [capture_s]".
 * TX is unchanged and Q65-60 only (the radio keys Q65 in 60 s slots): the beacon sends Q65-60 in the SUBMODE
 * menu's submode; QSO mode (answers and CQ, always Q65-60D) is active only while the app decodes Q65-60D.
 *
 * Memory: segment 1 (.text/.data/.bss) in the 80 KiB app window, segment 2 (.rodata) at the base of
 * the 240 KiB app data region; the rest of the region is one PCM read chunk + the decoder heap (the decoder
 * takes one ~210 KB arena from it).  The decoder reads the capture strictly forward: sync pass, one pass for
 * the best candidate, one for the others, one for the SNR of a decode.
 * Only the public ICAP API (icap.h) is used - no raw radio addresses.
 */
#include "icap.h"
#include "q65beacon_cb.h"
/* integrated beacon (beacon=1 in Q65.CFG): the decoder app also transmits.  Same beacon machinery as the
 * digi apps (beacon.h) + the Q65 encoder; mutually exclusive with decode at run time (both linked in). */
#include "beacon.h"
#include "q65enc.h"
#include "msg77.h"
#include "spot.h"              /* v4: report a decode to the loader's @SPOT service (no per-app PSKReporter engine) */

typedef uint32_t u32; typedef uint16_t u16; typedef uint8_t u8;
typedef int32_t s32; typedef int16_t s16;

/* ---- configuration ---------------------------------------------------- */
#define FS          8000u         /* ICAP audio service rate                   */
#define DEF_FMIN    600.0f
#define DEF_FMAX    1800.0f
#define CHUNK       4096u         /* s16 per SD write / read (8 KiB)           */
#define DRAIN_N     512u
#define DBG_CAP     2048u         /* was 6144; trimmed (window .bss) so the integrated beacon encoder fits both
                                   * windows without taking any decoder heap - the Rust decoder is untouched */
#define FLUSH_MS    10000u        /* debug-log flush interval while a capture is open */
#define DEC_CAP     2048u         /* was 4096; trimmed (window .bss), as DBG_CAP above */
#define DEC_HEAP_MIN (160u * 1024u)
#ifndef PCM_CAP
#define PCM_CAP     CHUNK         /* PCM read buffer: one SD chunk (the decoder reads the file forward) */
#endif

static const char *const F_CAP[2] = { "Q65CAPA.PCM", "Q65CAPB.PCM" };
#define F_SPEC "Q65SPEC.TMP"
#define F_DBG  "Q65DBG.TXT"
#define F_DEC  "Q65DEC.TXT"
#define F_CFG  "Q65.CFG"

static int stop(void) { return icap_should_stop(); }

/* ---- tiny freestanding helpers -------------------------------------- */
static u32 z_strlen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
static void z_move(char *d, const char *s, u32 n) { for (u32 i = 0; i < n; i++) d[i] = s[i]; }
static char *fmt_s(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char *fmt_u(char *p, u32 v) { char t[11]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) *p++ = t[--n]; *p = 0; return p; }
static char *fmt_i(char *p, s32 v) { if (v < 0) { *p++ = '-'; v = -v; } return fmt_u(p, (u32)v); }
static char *fmt_u0(char *p, u32 v, int w) { char t[11]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n < w) t[n++] = '0'; while (n) *p++ = t[--n]; *p = 0; return p; }
/* right-align the text produced by f into width w */
static char *pad_to(char *start, char *end, int w)
{
    int n = (int)(end - start);
    if (n >= w) return end;
    int sh = w - n;
    for (int i = n - 1; i >= 0; i--) start[i + sh] = start[i];
    for (int i = 0; i < sh; i++) start[i] = ' ';
    start[w] = 0; return start + w;
}
static s32 iround(float f) { return f < 0 ? -(s32)(-f + 0.5f) : (s32)(f + 0.5f); }

/* ---- static buffers (segment 1 .bss, inside the app window) ----------- */
static char g_dbg[DBG_CAP]; static u32 g_dbg_len;      /* rolling debug log          */
static char g_dec[DEC_CAP]; static u32 g_dec_len;      /* rolling decode log         */
static char g_line[192];
static char g_ln2[96];
static s16  g_capbuf[CHUNK];                           /* capture chunk               */

/* ---- rolling text logs, rewritten whole on flush (ICAP has no append/seek) */
#ifdef Q65_HOST_TRACE
void host_trace(const char *s, u32 n);                 /* host harness: full debug trace */
#endif
static void roll_add(char *buf, u32 *len, u32 cap, const char *s, u32 n)
{
#ifdef Q65_HOST_TRACE
    if (cap == DBG_CAP) host_trace(s, n);
#endif
    if (n + 2 > cap) n = cap - 2;
    if (*len + n + 2 > cap) {                          /* drop the oldest half at a line boundary */
        u32 cut = *len / 2;
        while (cut < *len && buf[cut] != '\n') cut++;
        if (cut < *len) cut++;
        if (*len + n + 2 - cut > cap) cut = *len;
        z_move(buf, buf + cut, *len - cut); *len -= cut;
    }
    z_move(buf + *len, s, n); *len += n;
    buf[(*len)++] = '\r'; buf[(*len)++] = '\n';
}
static u32 g_open_err;
static int  file_put(const char *name, const char *b, u32 n)   /* the transient 4th file slot */
{
    int fd = icap_open(name, ICAP_O_WRITE);
    if (fd < 0) { g_open_err++; return fd; }
    u32 off = 0;
    while (off < n) { u32 c = n - off; if (c > 4096) c = 4096; int w = icap_write(fd, b + off, c); if (w <= 0) break; off += (u32)w; }
    icap_close(fd);
    return 0;
}
static u32 g_flushes, g_last_flush, g_cap_on, g_dbg_dirty;
static void flush_dbg(void) { file_put(F_DBG, g_dbg, g_dbg_len); g_flushes++; g_last_flush = icap_ticks(); g_dbg_dirty = 0; }
static void dbg_noflush(const char *s) { roll_add(g_dbg, &g_dbg_len, DBG_CAP, s, z_strlen(s)); g_dbg_dirty = 1; }
/* flush policy: at once while no capture is open; while one is, at most every FLUSH_MS */
static void maybe_flush(void) { if (g_dbg_dirty && (!g_cap_on || icap_ticks() - g_last_flush >= FLUSH_MS)) flush_dbg(); }
static void dbg(const char *s) { dbg_noflush(s); maybe_flush(); }

static void logkv(const char *tag, const char *k1, s32 v1, const char *k2, s32 v2, const char *k3, s32 v3)
{
    char *p = fmt_s(g_line, tag);
    if (k1) { *p++ = ' '; p = fmt_s(p, k1); *p++ = '='; p = fmt_i(p, v1); }
    if (k2) { *p++ = ' '; p = fmt_s(p, k2); *p++ = '='; p = fmt_i(p, v2); }
    if (k3) { *p++ = ' '; p = fmt_s(p, k3); *p++ = '='; p = fmt_i(p, v3); }
    dbg(g_line);
}

/* called by the Rust panic handler before it traps: persist the log */
void q65b_panic_note(void)
{
    dbg_noflush("Q65B ABORT where=decoder reason=rust_panic");
    flush_dbg();
}

/* ---- audio ------------------------------------------------------------ */
static u32 g_drained;
/* ---- integrated beacon (Q65.CFG key=value: beacon=, call=, loc=, txfreq=) ---------------------------- */
/* v6: the beacon is armed / disarmed at run time by the owner (MODE-tile long press -> loader @TX beacon=/owner=,
 * followed via bcn_want in beacon.h); beacon=1 here stays the dev override (the loader arms at app start). */
static int  cfg_beacon = 0, cfg_txfreq = 0;
static char cfg_call[16], cfg_loc[12];
static int b_kmatch(const char *s, const char *k) { int i = 0; while (k[i] && s[i] == k[i]) i++; return k[i] == 0 && s[i] == '='; }
static int b_kint(const char *s) { int v = 0, nd = 0; while (*s && *s != '=') s++; if (*s) s++; for (; *s >= '0' && *s <= '9' && nd < 6; s++, nd++) v = v * 10 + (*s - '0'); return nd ? v : -1; }
static void b_ktok(const char *s, char *out, int cap) { int k = 0; while (*s && *s != '=') s++; if (*s) s++; while (*s == ' ') s++;
    for (; *s > ' ' && k < cap - 1; s++) out[k++] = (*s >= 'a' && *s <= 'z') ? (char)(*s - 32) : *s; out[k] = 0; }
static void read_cfg_beacon(void)
{
    char b[160]; int fd = icap_open(F_CFG, ICAP_O_READ); if (fd < 0) return;
    int n = icap_read(fd, b, (int)sizeof b - 1); icap_close(fd);
    if (n <= 0) return;
    b[n] = 0;
    for (char *s = b; *s; s++) {
        if (s != b && s[-1] != '\n' && s[-1] != '\r' && s[-1] != ' ' && s[-1] != '\t') continue;
        if (b_kmatch(s, "beacon")) { int v = b_kint(s); if (v >= 0) cfg_beacon = v != 0; }
        else if (b_kmatch(s, "call")) b_ktok(s, cfg_call, sizeof cfg_call);
        else if (b_kmatch(s, "loc"))  b_ktok(s, cfg_loc, sizeof cfg_loc);
        else if (b_kmatch(s, "txfreq")) { int v = b_kint(s); if (v >= 100 && v <= 3000) cfg_txfreq = v; }
    }
}
/* v4 heap union: the beacon encoder lives in the app region (icap_heap), not a permanent static reservation.
 * Decode and beacon never run at once, so the encoder state sits in the PCM read chunk at the region start (idle
 * while not decoding) and the Q65 decoder heap behind it is never touched - the decoder keeps its FULL heap. */
static q65_tx_t *g_tx;
/* P3a: the SUBMODE the owner chose on the radio - "sub=<A..E>" of the loader's "@MODECFG" line, read for every
 * transmission (an older loader has no such file: the default, D).  Thumb, and nothing else of that line is used: the
 * 9700 build of this app has ~130 B of its window left. */
__attribute__((noinline, optimize("Os"), target("thumb"))) static int mc_sub(void)
{
    char b[24];
    if (bcn_vfline("@MODECFG", b, (int)sizeof b))
        for (const char *s = b; s[1]; s++)
            if (s[0] == 'b' && s[1] == '=' && (unsigned)(s[2] - 'A') <= 4u) return s[2] - 'A';     /* su[b=]X */
    return Q65_SUBMODE_DEFAULT;
}
static int bcn_fill(void *c, int16_t *b, int n) { (void)c; return q65_tx_fill(g_tx, b, n); }
static int bcn_prepare(const char *call, const char *loc, bcn_fill_t *f, void **c, const char **err)
{
    char msg[28]; int e = msg77_beacon_text("DE", call, loc, msg);
    if (!e) e = q65_tx_init(g_tx, msg, cfg_txfreq ? (double)cfg_txfreq : 1500.0, 60, mc_sub(), 0, 0);
    if (e) { *err = e == TXE_CALL ? "call needs call= in CFG" : e == TXE_GRID ? "bad grid" : "encode error"; return -1; }
    *f = bcn_fill; *c = 0; return 0;
}

/* ---- decode mode: T/R period (Q65.CFG period=) and submode (radio SUBMODE menu, or Q65.CFG sub=) ---------
 * The decoder takes 15 / 30 / 60 s, submodes A..E (at 8 kHz not 15D/15E; 30E only with tone 0 below ~320 Hz).
 * Derived per period: the frame end (TX start + 85 symbols, WSJT-X: TX starts 0.5 s into a 15/30 s period, 1 s
 * into a 60 s one) - a capture must reach it to be decoded - and the capture length, whole seconds in
 * [frame end, period - 1] (60 s: 52..57 as before), default 14 / 28 / 55 s. */
static u32 g_period = 60;                               /* s */
static int g_sub = Q65_SUBMODE_DEFAULT;                 /* 0..4 = A..E, the one of the current / next decode */
static int cfg_sub = -1;                                /* Q65.CFG sub=: -1 = follow the radio's SUBMODE menu */
static u32 g_frame_n;                                   /* samples from the period start to the frame end */
static u32 g_cap_s;
static float g_dt0;                                     /* WSJT-X DT reference: the TX start, s */
static void mode_init(u32 cap_cfg)
{
    u32 tx0 = q65_tx_start_ms((int)g_period);
    g_dt0 = (float)tx0 * 0.001f;
    g_frame_n = tx0 * (FS / 1000u) + q65_tx_samples((int)g_period);
    u32 lo = (g_frame_n + FS - 1u) / FS, hi = g_period == 60u ? 57u : g_period - 1u;
    g_cap_s = g_period == 15u ? 14u : g_period == 30u ? 28u : 55u;
    if (cap_cfg >= lo && cap_cfg <= hi) g_cap_s = cap_cfg;
}
static void sub_update(void) { g_sub = cfg_sub >= 0 ? cfg_sub : mc_sub(); }
/* "Q65-30C" */
static char *mode_name(char *p) { p = fmt_s(p, "Q65-"); p = fmt_u(p, g_period); *p++ = (char)('A' + g_sub); *p = 0; return p; }

/* ---- Q65.CFG ----------------------------------------------------------------------------------------------
 * key=value, one per line: period=15|30|60  sub=A..E  flo=<Hz> fhi=<Hz> (tone-0 search range)  cap=<s>
 * (capture length) + the beacon keys (beacon= call= loc= txfreq=).  Older files that start with a number are
 * read as before: "fmin fmax [capture_s]". */
static float g_fmin = DEF_FMIN, g_fmax = DEF_FMAX;
static void read_cfg(void)
{
    u32 v[3] = { 0, 0, 0 }; int vi = 0;
    char b[200]; int fd = icap_open(F_CFG, ICAP_O_READ), n = 0;
    if (fd >= 0) { n = icap_read(fd, b, (int)sizeof b - 1); icap_close(fd); }
    if (n < 0) n = 0;
    b[n] = 0;
    int i = 0; while (b[i] == ' ' || b[i] == '\t' || b[i] == '\r' || b[i] == '\n') i++;
    if (b[i] >= '0' && b[i] <= '9') {                    /* the old positional form */
        while (i < n && vi < 3) {
            while (i < n && (b[i] < '0' || b[i] > '9')) i++;
            if (i >= n) break;
            u32 acc = 0; while (i < n && b[i] >= '0' && b[i] <= '9') { if (acc < 100000u) acc = acc * 10u + (u32)(b[i] - '0'); i++; }
            v[vi++] = acc;
        }
    } else {
        for (char *s = b; *s; s++) {
            if (s != b && s[-1] != '\n' && s[-1] != '\r' && s[-1] != ' ' && s[-1] != '\t') continue;
            int x = b_kint(s);
            if (b_kmatch(s, "period")) { if (x == 15 || x == 30 || x == 60) g_period = (u32)x; }
            else if (b_kmatch(s, "sub")) { char t[4]; b_ktok(s, t, sizeof t); if (t[0] >= 'A' && t[0] <= 'E' && !t[1]) cfg_sub = t[0] - 'A'; }
            else if (b_kmatch(s, "flo") && x >= 0) { v[0] = (u32)x; if (vi < 2) vi = 2; }
            else if (b_kmatch(s, "fhi") && x >= 0) { v[1] = (u32)x; if (vi < 2) vi = 2; }
            else if (b_kmatch(s, "cap") && x >= 0) { v[2] = (u32)x; vi = 3; }
        }
    }
    /* the span is capped at the default 1200 Hz: the decoder heap is sized for it */
    if (vi >= 2 && v[1] > v[0] && v[1] <= 3000u && v[1] - v[0] <= 1200u) { g_fmin = (float)v[0]; g_fmax = (float)v[1]; }
    mode_init(vi >= 3 ? v[2] : 0);
    if (n) read_cfg_beacon();
}

/* =====================================================================
 * Capture state machine, driven by pump() from the main loop AND from every decoder callback, so
 * minute N+1 is recorded while minute N is decoded.
 * ===================================================================== */
typedef struct {
    int fd, file;                 /* open write fd, file index 0/1                    */
    u32 need, total, cn;          /* samples wanted / written+buffered / in g_capbuf  */
    u32 pending, streak;          /* overrun seen, not yet zero-filled                */
    u32 ref_t, ref_n, t0;         /* last caught-up point; start tick                 */
    u32 hh, mm, ss;               /* the period being captured (its start)            */
    u32 overruns, gaps, zfill, wr_ms, wr_max, late;
    u32 pump_gap;                 /* longest interval between two pumps while capturing (ms) */
    u32 pre;                      /* samples taken from the pre-roll (late :00 detection)    */
    u32 werr;                     /* an SD write failed / was short: capture abandoned       */
    u32 y, mo, dd;                /* date of the captured minute                             */
} cap_t;
static cap_t K;
static int  g_have_clock, g_prev_sec = -1, g_next_file, g_dec_file = -1;
static u32  g_last_per = 0xFFFFu, g_fr_t0, g_last_pump, g_minutes, g_wait_t;   /* g_last_per: UTC day period index */
static volatile int g_ready = -1;                     /* completed capture waiting for its decode */
static u32  g_ready_hh, g_ready_mm, g_ready_ss, g_ready_n, g_abort_next, g_dropped;
static u32  g_ready_y, g_ready_mo, g_ready_dd, g_go_y, g_go_mo, g_go_dd, g_wr_errs;
/* the WSJT-X time stamp of a period: "HHMM" (60 s), "HHMMSS" (15 / 30 s) */
static char *stamp(char *p, u32 hh, u32 mm, u32 ss)
{
    p = fmt_u0(p, hh, 2); p = fmt_u0(p, mm, 2);
    return g_period < 60u ? fmt_u0(p, ss, 2) : p;
}
/* a status line ("HHMM Q65: <what>") -> app window line 4 + the decode log; the DECODE screen gets decodes only */
static void minute_note(u32 hh, u32 mm, u32 ss, const char *what)
{
    char *p = stamp(g_ln2, hh, mm, ss); p = fmt_s(p, " Q65: "); fmt_s(p, what);
    icap_set_line(4, g_ln2);
    roll_add(g_dec, &g_dec_len, DEC_CAP, g_ln2, z_strlen(g_ln2));
    file_put(F_DEC, g_dec, g_dec_len);
}
static int cap_put(const s16 *b, u32 n)                /* write n samples; 0 = ok */
{
    if (!n) return 0;
    int w = icap_write(K.fd, b, n * 2u);
    if (w != (int)(n * 2u)) { K.werr = 1; g_wr_errs++; return -1; }
    return 0;
}

static void cap_write(void)
{
    u32 t = icap_ticks();
    if (cap_put(g_capbuf, K.cn)) K.total -= K.cn;     /* the file never got them: keep total == file */
    t = icap_ticks() - t; K.wr_ms += t; if (t > K.wr_max) K.wr_max = t;
    K.cn = 0;
}
static void cap_finish(int keep)
{
    if (K.cn) cap_write();
    icap_close(K.fd); K.fd = -1; g_cap_on = 0; g_prev_sec = -1;
    if (K.werr) {                                        /* the file has a hole of unknown size: unusable */
        keep = 0;
        dbg_noflush("Q65B ABORT where=capture reason=sd_write_error");
        minute_note(K.hh, K.mm, K.ss, "SD write error");
    }
    { char *p = fmt_s(g_line, "Q65B CAP "); p = stamp(p, K.hh, K.mm, K.ss);
      p = fmt_s(p, " file="); p = fmt_u(p, (u32)K.file); p = fmt_s(p, " samples="); p = fmt_u(p, K.total);
      p = fmt_s(p, " overruns="); p = fmt_u(p, K.overruns); p = fmt_s(p, " gaps="); p = fmt_u(p, K.gaps);
      p = fmt_s(p, " zfill="); p = fmt_u(p, K.zfill); p = fmt_s(p, " wr_ms="); p = fmt_u(p, K.wr_ms);
      p = fmt_s(p, " wr_max="); p = fmt_u(p, K.wr_max); p = fmt_s(p, " late_ms="); p = fmt_u(p, K.late);
      p = fmt_s(p, " pump_gap_ms="); p = fmt_u(p, K.pump_gap); p = fmt_s(p, " preroll="); p = fmt_u(p, K.pre);
      p = fmt_s(p, " werr="); fmt_u(p, K.werr);
      dbg_noflush(g_line); }
    if (keep && K.total >= g_frame_n) {                   /* the whole frame is in the file */
        if (g_ready >= 0) { g_dropped++; dbg_noflush("Q65B DROP undecoded capture replaced"); }
        g_ready_hh = K.hh; g_ready_mm = K.mm; g_ready_ss = K.ss; g_ready_n = K.total;
        g_ready_y = K.y; g_ready_mo = K.mo; g_ready_dd = K.dd; g_ready = K.file;
        if (g_dec_file >= 0) g_abort_next = 1;           /* the running decode yields to this one */
    } else if (keep) dbg_noflush("Q65B SKIP reason=short_capture");
    g_next_file = K.file ^ 1;
    maybe_flush();
}
/* Pre-roll: while waiting for :00 the audio is not discarded but kept in g_capbuf as a 0.5 s ring, so a
 * :00 that is noticed late (the pump runs inside the decoder, up to a few hundred ms apart) still starts the
 * capture at the right sample. */
#define PRE_N CHUNK
static u32 g_pre_pos, g_pre_n;
static void preroll_read(void)
{
    for (int i = 0; i < 64; i++) {
        /* straight into the ring, up to its end (no separate drain buffer: the IC-9700 window has no room) */
        u32 room = PRE_N - g_pre_pos; if (room > DRAIN_N) room = DRAIN_N;
        int r = icap_audio_read(g_capbuf + g_pre_pos, (int)room);
        if (r == ICAP_E_OVERRUN) { g_pre_n = 0; continue; }   /* discontinuity: forget the pre-roll */
        if (r <= 0) break;
        g_pre_pos = (g_pre_pos + (u32)r) % PRE_N; g_pre_n += (u32)r; if (g_pre_n > PRE_N) g_pre_n = PRE_N;
        g_drained += (u32)r;
        if ((u32)r < room) break;
    }
}
/* lag_ms: how long ago :00 was (estimated); that much pre-roll becomes the start of the capture */
static void cap_start(u32 hh, u32 mm, u32 ss, u32 late, u32 lag_ms)
{
    preroll_read();
    K.file = g_next_file;
    K.fd = icap_open(F_CAP[K.file], ICAP_O_WRITE);
    if (K.fd < 0) { g_open_err++; g_pre_n = 0; logkv("Q65B ABORT where=capture reason=open", "rc", K.fd, "file", K.file, 0, 0); return; }
    u32 pre = lag_ms * (FS / 1000u); if (pre > g_pre_n) pre = g_pre_n;
    u32 st = (g_pre_pos + PRE_N - pre) % PRE_N;               /* oldest pre-roll sample to keep */
    K.werr = 0;
    if (st + pre <= PRE_N) cap_put(g_capbuf + st, pre);
    else if (!cap_put(g_capbuf + st, PRE_N - st)) cap_put(g_capbuf, st + pre - PRE_N);
    g_pre_n = 0; g_pre_pos = 0;
    K.t0 = K.ref_t = icap_ticks(); K.ref_n = pre;
    K.need = g_cap_s * FS; K.total = pre; K.cn = K.pending = K.streak = 0;
    K.overruns = K.gaps = K.zfill = K.wr_ms = K.wr_max = K.pump_gap = 0; K.late = late; K.pre = pre;
    K.hh = hh; K.mm = mm; K.ss = ss; K.y = g_go_y; K.mo = g_go_mo; K.dd = g_go_dd; g_cap_on = 1; g_minutes++;
    if (K.werr) cap_finish(1);                          /* pre-roll write failed: reported, not decoded */
}
/* move everything the ring holds into the capture file */
static void cap_step(void)
{
    for (int guard = 0; guard < 64; guard++) {
        u32 want = CHUNK - K.cn; if (want > K.need - K.total) want = K.need - K.total;
        if (!want) break;
        int r = icap_audio_read(g_capbuf + K.cn, (int)want);
        if (r == ICAP_E_OVERRUN) { K.overruns++; K.pending = 1; if (++K.streak > 50u) { K.streak = 0; break; } continue; }
        K.streak = 0;
        if (r < 0) { logkv("Q65B ABORT where=capture reason=audio_read", "rc", r, 0, 0, 0, 0); cap_finish(0); return; }
        K.cn += (u32)r; K.total += (u32)r;
        if (K.cn == CHUNK) { cap_write(); if (K.werr) { cap_finish(1); return; } }
        if ((u32)r < want) {                             /* caught up with the live stream */
            u32 now = icap_ticks();
            if (K.pending) {
                /* samples lost: zero-fill the hole to its wall-clock length, measured from the last
                 * caught-up point before the overrun (audio/tick clock drift does not accumulate) */
                u32 expected = K.ref_n + (now - K.ref_t) * (FS / 1000u);
                if (expected > K.need) expected = K.need;
                if (expected > K.total) {
                    K.gaps++;
                    while (K.total < expected) {
                        u32 k = expected - K.total; if (k > CHUNK - K.cn) k = CHUNK - K.cn;
                        for (u32 i = 0; i < k; i++) g_capbuf[K.cn + i] = 0;
                        K.cn += k; K.total += k; K.zfill += k;
                        if (K.cn == CHUNK) { cap_write(); if (K.werr) { cap_finish(1); return; } }
                    }
                }
                K.pending = 0;
            }
            K.ref_t = now; K.ref_n = K.total;
            break;
        }
    }
    if (K.total >= K.need) cap_finish(1);
    else if (icap_ticks() - K.t0 > K.need / (FS / 1000u) + 3000u) { dbg_noflush("Q65B ABORT where=capture reason=audio_stalled"); cap_finish(1); }
}
/* not capturing: watch for the next period start (radio clock; :00 for 60 s, :00/:30 for 30 s, every :15 for 15 s),
 * else free-run one period from launch */
static u32 g_defer, g_defer_hh, g_defer_mm, g_defer_ss;
static u32 g_poll_t, g_edge_t, g_edge_sec, g_edge_ok;       /* RTC second-edge model */
static void cap_watch(void)
{
    preroll_read();
    u32 hh, mm, ss, lag = 0, now = icap_ticks();
    if (g_defer) {                                       /* a start deferred until the old decode let go */
        if (g_next_file == g_dec_file) { g_abort_next = 1; return; }
        hh = g_defer_hh; mm = g_defer_mm; ss = g_defer_ss; g_defer = 0;
    } else if (g_have_clock) {
        struct icap_time t;
        if (icap_clock_now(&t) != 0) return;
        u32 pk = ((u32)t.hour * 3600u + (u32)t.min * 60u + t.sec) / g_period;   /* period of the day */
        /* every observed second edge with a short poll interval refines where the RTC second starts */
        if (g_prev_sec >= 0 && (u32)g_prev_sec != t.sec) {
            u32 w = now - g_poll_t;
            if (w <= 40u || !g_edge_ok || now - g_edge_t > 20000u) { g_edge_t = now - w / 2u; g_edge_sec = t.sec; g_edge_ok = 1; }
        }
        /* a period start: its first second, seen as an edge (or, right after a capture, a period not yet captured) */
        int go = t.sec % g_period == 0 && (g_prev_sec >= 0 ? (u32)g_prev_sec != t.sec : pk != g_last_per);
        g_prev_sec = t.sec; g_poll_t = now;
        if (!go) return;
        hh = t.hour; mm = t.min; ss = t.sec; g_last_per = pk;
        g_go_y = t.year; g_go_mo = t.mon; g_go_dd = t.day;
        if (g_edge_ok && now - g_edge_t < 20000u) {     /* the period start predicted from the last good edge */
            u32 t00 = g_edge_t + ((g_period - g_edge_sec % g_period) % g_period) * 1000u;
            lag = (s32)(now - t00) > 0 ? now - t00 : 0;
            if (lag > 1000u) lag = 0;                    /* model stale: start now */
        }
    } else {
        if (g_last_per != 0xFFFFu && now - g_fr_t0 < g_period * 1000u) return;
        g_last_per = 0; g_fr_t0 = now; hh = mm = ss = 0;
    }
    if (g_next_file == g_dec_file) {                     /* never truncate the file being decoded */
        g_abort_next = 1; g_defer = 1; g_defer_hh = hh; g_defer_mm = mm; g_defer_ss = ss; g_wait_t = now - lag;
        return;
    }
    u32 late = g_wait_t ? now - g_wait_t : lag; g_wait_t = 0;
    cap_start(hh, mm, ss, late, late);
}
/* the audio pump: called everywhere (rate-limited to 10 ms unless forced) */
static u32 g_last_call;
static void pump(int force)
{
    u32 now = icap_ticks();
    if (g_cap_on && now - g_last_call > K.pump_gap) K.pump_gap = now - g_last_call;
    g_last_call = now;
    if (!force && now - g_last_pump < 10u) return;
    g_last_pump = now;
    if (g_cap_on) cap_step(); else cap_watch();
}

/* =====================================================================
 * SD-backed decoder callbacks (no f_seek: forward-only cache for PCM, a
 * cursor with reopen-on-backward for the spectra store).
 * ===================================================================== */
typedef struct {
    int fd;
    s16 *cache; u32 cap;         /* one chunk of the file (samples, s16)   */
    u32 base, len;               /* the chunk covers [base, base+len)      */
    u32 total;                   /* samples in the file                    */
} pcm_src_t;
static pcm_src_t g_pcm;
static u32 g_pcm_reopen;
static const char *g_pcm_name;
static u32 g_io_err;                                  /* SD open/read failure in the decode: abort it */
static void pump(int force);

static void pcm_reopen(pcm_src_t *s)
{
    if (s->fd >= 0) icap_close(s->fd);
    s->fd = icap_open(g_pcm_name, ICAP_O_READ);
    if (s->fd < 0) { g_open_err++; g_io_err = 1; s->fd = -1; }   /* never keep a negative rc as fd */
    s->base = 0; s->len = 0; g_pcm_reopen++;
}
static size_t cb_pcm_len(void *ctx) { (void)ctx; return g_pcm.total; }
/* The decoder reads each pass strictly forward (sync: once through the file; per candidate / SNR: once
 * through the frame), so the file is streamed chunk by chunk; a read behind the current chunk reopens
 * the file (no f_seek), a read ahead of it reads the chunks in between. */
static size_t cb_pcm_read(void *ctx, size_t start, float *out, size_t n)
{
    (void)ctx; pcm_src_t *s = &g_pcm; u32 st = (u32)start, done = 0;
    pump(0);
    if (!g_io_err && (s->fd < 0 || st < s->base)) pcm_reopen(s);   /* first read / backward */
    while (done < (u32)n && !g_io_err && s->fd >= 0) {
        u32 idx = st + done;
        if (idx >= s->total) break;
        if (idx >= s->base + s->len) {                 /* next chunk */
            u32 nb = s->base + s->len, rd = s->total - nb; if (rd > s->cap) rd = s->cap;
            int got = icap_read(s->fd, s->cache, rd * 2u);
            if (got < 2) { g_io_err = 1; break; }       /* the file is shorter than its capture: SD error */
            s->base = nb; s->len = (u32)got / 2u;
            if (idx >= s->base + s->len) pump(0);       /* long forward skip: keep the capture fed */
            continue;
        }
        u32 k = s->base + s->len - idx; if (k > (u32)n - done) k = (u32)n - done;
        const s16 *c = s->cache + (idx - s->base);
        for (u32 i = 0; i < k; i++) out[done + i] = (float)c[i] * (1.0f / 32768.0f);
        done += k;
    }
    for (u32 i = done; i < (u32)n; i++) out[i] = 0.0f;
    return done;
}

typedef struct { int fd; int writing; u32 ncols, nrows, cur_row; } spec_t;
static spec_t g_spec;
static void cb_spec_begin(void *ctx, size_t ncols)
{
    (void)ctx; spec_t *s = &g_spec;
    if (s->fd >= 0) icap_close(s->fd);
    s->fd = icap_open(F_SPEC, ICAP_O_WRITE);
    if (s->fd < 0) { g_open_err++; g_io_err = 1; s->fd = -1; }
    s->writing = 1; s->ncols = (u32)ncols; s->nrows = 0; s->cur_row = 0;
}
static void cb_spec_append(void *ctx, const float *row, size_t ncols)
{
    (void)ctx; spec_t *s = &g_spec;
    pump(0);
    if (s->fd < 0 || !s->writing) return;
    if (icap_write(s->fd, row, (u32)ncols * 4u) != (int)(ncols * 4u)) g_io_err = 1;
    s->nrows++;
}
static void spec_open_read(spec_t *s)
{
    if (s->fd >= 0) icap_close(s->fd);
    s->fd = icap_open(F_SPEC, ICAP_O_READ); s->writing = 0; s->cur_row = 0;
    if (s->fd < 0) { g_open_err++; g_io_err = 1; s->fd = -1; }
}
static void cb_spec_read(void *ctx, size_t idx, float *out, size_t ncols)
{
    (void)ctx; spec_t *s = &g_spec; u32 nb = (u32)ncols * 4u;
    pump(0);
    if (g_io_err) { u8 *o = (u8 *)out; for (u32 i = 0; i < nb; i++) o[i] = 0; return; }
    if (s->writing || s->fd < 0 || (u32)idx < s->cur_row) spec_open_read(s);
    while (s->fd >= 0 && s->cur_row < (u32)idx) {       /* forward skip (no f_seek): read the rows in between */
        if (icap_read(s->fd, out, nb) < (int)nb) break;
        s->cur_row++;
    }
    int got = s->fd >= 0 ? icap_read(s->fd, out, nb) : -1;
    if (got < (int)nb) { u8 *o = (u8 *)out; for (u32 i = got > 0 ? (u32)got : 0; i < nb; i++) o[i] = 0; }
    s->cur_row++;
}
static size_t cb_spec_nrows(void *ctx) { (void)ctx; return g_spec.nrows; }
static size_t cb_spec_ncols(void *ctx) { (void)ctx; return g_spec.ncols; }

static void free_dec_fds(void)
{
    if (g_pcm.fd >= 0) { icap_close(g_pcm.fd); g_pcm.fd = -1; }
    if (g_spec.fd >= 0) { icap_close(g_spec.fd); g_spec.fd = -1; }
}

/* cooperative abort: STOP, or the next capture is complete (its decode takes over).  Pumps audio and
 * yields (~every 20 ms) so the firmware stays responsive. */
static u32 g_last_yield, g_abort_polls, g_last_poll, g_poll_gap;
/* P4b: QSO mode (qso.h, included below).  Thumb: the 9700 window is nearly full, and none of it is hot. */
#define QSO_FN static __attribute__((noinline, optimize("Os"), target("thumb")))
QSO_FN int qso_poll(void);
static int qso_gate(void);
static int cb_should_abort(void *ctx)
{
    (void)ctx;
    g_abort_polls++;
    { u32 t = icap_ticks(); if (t - g_last_poll > g_poll_gap) g_poll_gap = t - g_last_poll; g_last_poll = t; }
    pump(0);
    u32 now = icap_ticks();
    if (now - g_last_yield >= 20u) { g_last_yield = now; icap_yield(); }
    return stop() || g_abort_next || g_io_err || qso_gate();   /* P4b: our QSO transmission is due */
}
static u32 g_dec_t0;                                    /* decode start: decoder log lines carry "+<ms> " since then */
static void cb_log(void *ctx, const char *s, size_t len)
{
    (void)ctx;
    char *p = g_line; *p++ = '+'; p = fmt_u(p, icap_ticks() - g_dec_t0); *p++ = ' ';
    u32 n = (u32)(p - g_line), k = (u32)len; if (k > sizeof g_line - n) k = sizeof g_line - n;
    z_move(p, s, k);
    roll_add(g_dbg, &g_dbg_len, DBG_CAP, g_line, n + k); g_dbg_dirty = 1;
    pump(0);
    maybe_flush();
}

/* ---- decode one completed capture ---------------------------------------- */
static void minute_line(char *p, u32 hh, u32 mm, u32 ss, const q65b_result_t *r)
{
    /* WSJT-X layout: HHMM[SS]%4d%5.1f%5d :  msg  (':' = WSJT-X Q65 marker) */
    char *q = stamp(p, hh, mm, ss);
    char *a = q; q = fmt_i(q, iround(r->snr_db)); q = pad_to(a, q, 4);
    float dt = r->dt_s - g_dt0; s32 d10 = iround(dt * 10.0f);
    a = q; if (d10 < 0) { *q++ = '-'; d10 = -d10; }
    q = fmt_u(q, (u32)d10 / 10u); *q++ = '.'; *q++ = (char)('0' + (u32)d10 % 10u); *q = 0;
    q = pad_to(a, q, 5);
    a = q; q = fmt_u(q, (u32)iround(r->freq_hz)); q = pad_to(a, q, 5);
    *q++ = ' '; *q++ = ':'; *q++ = ' '; *q++ = ' ';
    fmt_s(q, r->text);
}
static u32 g_ndec, g_dec_heap;                         /* g_dec_heap: in every DECPOLL line (the BOOT line rolls out) */
static void show_title(void)                            /* app window line 0: "Q65-30C monitor (IC-705)" */
{
    char *p = mode_name(g_ln2); p = fmt_s(p, " monitor ("); p = fmt_s(p, icap_is_ic9700() ? "IC-9700)" : "IC-705)");
    icap_set_line(0, g_ln2);
    /* too wide for 8 kHz audio: the decoder refuses it (Q65-30E only below ~320 Hz, so not here either) */
    if ((g_period == 15u && g_sub >= 3) || (g_period == 30u && g_sub == 4)) {
        p = mode_name(g_ln2); fmt_s(p, ": too wide for 8 kHz");
        icap_set_line(4, g_ln2);
    }
}
static void decode_one(void)
{
    int file = g_ready; u32 hh = g_ready_hh, mm = g_ready_mm, ss = g_ready_ss, n = g_ready_n;
    u32 y = g_ready_y, mo = g_ready_mo, dd = g_ready_dd;
    g_ready = -1; g_dec_file = file; g_abort_next = 0; g_io_err = 0;
    g_pcm_name = F_CAP[file];
    g_pcm.total = n; g_pcm.fd = -1; g_pcm.base = g_pcm.len = 0; g_pcm_reopen = 0;
    g_spec.fd = -1; g_spec.writing = 0; g_spec.nrows = g_spec.ncols = g_spec.cur_row = 0;
    g_abort_polls = 0; g_last_yield = g_last_poll = icap_ticks(); g_poll_gap = 0;
    { int old = g_sub; sub_update(); if (g_sub != old) show_title(); }   /* the owner may change SUBMODE at any time */
    { char *p = fmt_s(g_line, "Q65B DECODE "); p = stamp(p, hh, mm, ss); *p++ = ' '; p = mode_name(p); p = fmt_s(p, " file="); fmt_u(p, (u32)file); dbg(g_line); }
    icap_set_line(2, "decoding");
    q65b_cb_t cb;
    cb.ctx = 0; cb.pcm_len = cb_pcm_len; cb.pcm_read = cb_pcm_read;
    cb.spec_begin = cb_spec_begin; cb.spec_append = cb_spec_append; cb.spec_read = cb_spec_read;
    cb.spec_nrows = cb_spec_nrows; cb.spec_ncols = cb_spec_ncols;
    cb.log_line = cb_log; cb.should_abort = cb_should_abort;
    q65b_result_t r; r.ok = 0; r.text[0] = 0;
    u32 td = icap_ticks(); g_dec_t0 = td;
    /* Flush denormals to zero for the decode (FPSCR.FZ), as NEON always does.  With FZ off a strong signal that does
     * not decode (wrong submode, or none in the period) fills the scalar FP of the decoder with denormals; on the
     * radio that made a no-decode take > 30 s (Q65-30B, IC-9700).  Results are unchanged; the old FPSCR comes back
     * afterwards (the beacon encoders want exact IEEE arithmetic). */
#ifdef __arm__
    u32 fpscr; __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr)); __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr | (1u << 24)));
#endif
    int rc = q65b_decode_cb(&cb, g_period, (u32)g_sub, 0, FS, g_fmin, g_fmax, 0, 0, &r);
#ifdef __arm__
    __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr));
#endif
    td = icap_ticks() - td;
    free_dec_fds();
    g_dec_file = -1;
    logkv("Q65B DEC", "rc", rc, "ms", (s32)td, "reopen", (s32)g_pcm_reopen);
    logkv("Q65B DECPOLL", "polls", (s32)g_abort_polls, "max_gap_ms", (s32)g_poll_gap, "dec_heap", (s32)g_dec_heap);
    if (rc == 0 && r.ok) {
        g_ndec++;
        minute_line(g_ln2, hh, mm, ss, &r);
        icap_decode_out(g_ln2);
        spot_emit("Q65", r.text, r.freq_hz, iround(r.snr_db));   /* report to the loader @SPOT service (loader adds dial+UTC+gate) */
        char *p = g_line;
        if (y) { p = fmt_u(p, y); *p++ = '-'; p = fmt_u0(p, mo, 2); *p++ = '-'; p = fmt_u0(p, dd, 2); *p++ = ' '; }   /* the capture's date */
        fmt_s(p, g_ln2);
        roll_add(g_dec, &g_dec_len, DEC_CAP, g_line, z_strlen(g_line));
        file_put(F_DEC, g_dec, g_dec_len);
        p = fmt_s(g_line, "Q65B OUT "); fmt_s(p, g_ln2); dbg(g_line);
    } else if (stop()) {
        dbg("Q65B ABORT where=decode reason=stop");
    } else if (g_io_err) {
        logkv("Q65B ABORT where=decode reason=sd_read_error", "ms", (s32)td, "open_err", (s32)g_open_err, 0, 0);
        minute_note(hh, mm, ss, "SD read error");
    } else if (g_abort_next) {
        logkv("Q65B ABORT where=decode reason=next_capture", "ms", (s32)td, "polls", (s32)g_abort_polls, 0, 0);
        minute_note(hh, mm, ss, "decode out of time");
    } else {
        dbg("Q65B OUT no decode");
    }
    g_abort_next = 0;
    { char *p = fmt_s(g_line, "periods "); p = fmt_u(p, g_minutes); p = fmt_s(p, "  decodes "); fmt_u(p, g_ndec); icap_set_line(3, g_line); }
}

/* drop the capture / decode state: before a beacon session (the minute being captured is lost while the radio
 * transmits) and after it.  Only called between decodes (g_dec_file == -1).  The PCM read chunk, which held the
 * encoder state, is re-validated by the next decode's reopen (free_dec_fds); the pre-roll restarts on the ring
 * overrun the TX time leaves behind. */
_Static_assert(sizeof(q65_tx_t) <= PCM_CAP * 2u, "beacon encoder state must fit the PCM read chunk");
static void cap_idle(void)
{
    free_dec_fds();
    if (g_cap_on) cap_finish(0);                         /* closes the capture, not queued for decoding */
    g_ready = -1; g_defer = 0; g_wait_t = 0; g_prev_sec = -1;
}

/* ---- QSO mode (P4b): the operator taps a decoded line, then the "TX?" prompt (qso.h).  The encoder state and the
 * write buffer use the idle PCM read chunk, exactly as the beacon does; a decode never runs while we send. */
#define QSO_SEC   60
#define QSO_FINAL "RR73"
#define QSO_CALL  cfg_call
#define QSO_LOC   cfg_loc
#define QSO_TXF   cfg_txfreq
static u32 g_hb;                                        /* base of the app data region */
static void qso_capdrop(void)                           /* after the transmission (P5: no SD work in front of it): as
                                                         * cap_idle, but a capture waiting for its decode is kept */
{
    free_dec_fds();
    if (g_cap_on) cap_finish(0);
    g_defer = 0; g_wait_t = 0; g_prev_sec = -1;
}
/* P5: QSO answers and CQ are always Q65-60D - the decoder of this app is 60D only, so the other station must be on D
 * and so must we (the menu SUBMODE stays the beacon's).  One note per run when the menu says something else. */
static int qso_enc(const char *msg, int tone)
{
    static u8 told;
    g_tx = (q65_tx_t *)(uintptr_t)g_hb;
    if (!told && mc_sub() != 3) { told = 1; icap_decode_out("QSO: sent as Q65-60D"); }
    return q65_tx_init(g_tx, msg, (double)tone, 60, 3, 0, 0);
}
static int qso_stream(void) { return bcn_stream(bcn_fill, 0, (int16_t *)(uintptr_t)(g_hb + 1024u), 160); }
static char *qso_modetag(char *d) { return bcn_cpy(d, "<MODE:4>MFSK <SUBMODE:3>Q65 "); }
#include "qso.h"
/* QSO mode only while the app decodes Q65-60D: its answers and CQ go out as Q65-60D (the radio's Q65 TX slot is 60 s).
 * In any other decode mode the loader's QSO file is not read (a tap on a decode row does nothing, CQ is not called);
 * a QSO in progress is dropped and the tap counter resynchronised when it comes back. */
static int qso_gate(void)
{
    if (g_period == 60u && g_sub == 3) return qso_poll();
    if (Q.init || Q.have || Q.due) { qso_cancel(); Q.have = 0; Q.init = 0; }
    return 0;
}

int app_main(const struct icap_svc *svc)
{
    icap_use(svc);
    g_pcm.fd = -1; g_spec.fd = -1; K.fd = -1; g_cap_on = 0; g_ready = -1; g_dec_file = -1;
    g_dbg_len = 0; g_dec_len = 0; g_last_per = 0xFFFFu; g_prev_sec = -1; g_next_file = 0;
    if (!ICAP_HAS(audio_read) || !icap_heap_size()) {
        icap_print("Q65 needs ICAP v1rc firmware");     /* the window shows 32 characters */
        while (!stop()) icap_sleep_ms(200);
        return -1;
    }
    struct icap_time t;
    g_have_clock = icap_clock_now(&t) == 0;
    int rate = icap_audio_rate();

    /* region: [PCM read chunk (s16)][decoder heap] */
    u32 hb = (u32)(uintptr_t)icap_heap(), hs = icap_heap_size();
    g_hb = hb;
    g_pcm.cap = PCM_CAP;
    u32 cache_b = (g_pcm.cap * 2u + 7u) & ~7u;
    g_pcm.cache = (s16 *)(uintptr_t)hb;
    /* v4: PSKReporter is a LOADER service (@SPOT) - NOTHING is carved from the decoder region here any more, so
     * the decoder gets its FULL heap back (the 9700 Q65 regression: dec_heap was dropped below its high-water in
     * v3 by the per-app pskr ctx+datagram; restored here). */
    u32 dec_off = cache_b;
    u32 dec_heap = hs > dec_off ? hs - dec_off : 0;
    g_dec_heap = dec_heap;
    q65b_heap_init((void *)(uintptr_t)(hb + dec_off), dec_heap);

    char mode[8]; if (icap_mode(mode, sizeof mode) <= 0) fmt_s(mode, "-");
    read_cfg();
    logkv("Q65B BOOT fw705 v6-bcnswitch", "heap", (s32)hs, "cache", (s32)cache_b, "dec_heap", (s32)dec_heap);
    sub_update();
    logkv("Q65B CFG", "fmin", (s32)g_fmin, "fmax", (s32)g_fmax, "cap_s", (s32)g_cap_s);
    logkv("Q65B CFG", "period", (s32)g_period, "sub", g_sub, "cfg_sub", cfg_sub);
    logkv("Q65B AUDIO", "rate", rate, "clock", g_have_clock, "abi", (s32)svc->abi_version);
    { char *p = fmt_s(g_line, "Q65B MODE "); fmt_s(p, mode); dbg(g_line); }
    if (rate != (int)FS) dbg("Q65B WARN audio rate not 8000");

    for (;;) {
        if (bcn_want_poll(cfg_beacon, 0)) {             /* armed: integrated beacon, until the owner disarms */
            /* v7: the s16 write buffer also lives in the idle PCM chunk (8 KiB), after the encoder - frees 320 B of
             * .bss for the DECODE-screen TX line (the 9700 window had no room) */
            _Static_assert(sizeof(q65_tx_t) <= 1024u && 1024u + 160u * 2u <= PCM_CAP * 2u, "Q65 beacon heap union");
            int16_t *txbuf = (int16_t *)(uintptr_t)(hb + 1024u);
            cap_idle(); dbg("Q65B BCN on");
            g_tx = (q65_tx_t *)(uintptr_t)hb;            /* heap union: encoder in the (idle) PCM read chunk */
            bcn_follow = cfg_beacon;
            bcn_run("Q65", bcn_prepare, cfg_call, cfg_loc, txbuf, 160);
            if (stop()) break;
            cap_idle(); dbg("Q65B BCN off");
        }
        icap_clear(); show_title();
        if (!g_have_clock) icap_set_line(4, "no radio clock: free-running");
        if (dec_heap < DEC_HEAP_MIN) icap_set_line(4, "decoder heap too small");
        icap_audio_read(g_capbuf, 1);                   /* first read starts the loader's capture (not capturing:
                                                         * g_capbuf is the pre-roll ring) */
        while (!stop()) {
            if (bcn_want_poll(cfg_beacon, 1000)) break; /* the owner armed the beacon (checked between decodes) */
            pump(1);
            if (qso_gate()) { qso_run(); continue; }    /* P4b: the granted QSO / CQ transmission, once */
            if (g_ready >= 0) { decode_one(); continue; }
            icap_sleep_ms(20);
        }
        if (stop()) break;
    }
    free_dec_fds();
    if (g_cap_on) cap_finish(0);
    logkv("Q65B EXIT", "periods", (s32)g_minutes, "decodes", (s32)g_ndec, "open_err", (s32)g_open_err);
    flush_dbg();
    return 0;
}
