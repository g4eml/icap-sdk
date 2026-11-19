/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * DIGI app shell for the period-based decoders (JT4, PI4: 60 s; FT8: 15 s) - one binary per decoder:
 *   compile with -DDIGI_JT4, -DDIGI_PI4 or -DDIGI_FT8 and link the decoder (jt4dec.c ... / pi4dec.c / ft8dec.c ...).
 *
 * Registered as a decoder mode (mkapp --mode JT4 / PI4 / FT8): the loader starts it when the owner selects
 * the mode with the RTTY button (radio in USB-D, DECODE screen open) and stops it when the mode is left.
 *
 * FT8 (-DDIGI_FT8): the same machinery with a 15 s period (:00/:15/:30/:45, WSJT-X), capture from the period
 * start for 14.6 s, decode lines "HHMMSS SNR  DT FREQ ~  MESSAGE" (WSJT-X: %6s%4d%5.1f%5d ~  %s), at most
 * FT8_MAXOUT per period, no duplicate text within a period; the callsign hash table (<...> calls) lives at the
 * start of the heap for the whole run, the decoder works in the rest.  The description below says "minute"
 * for the period.
 *
 * Every UTC minute (radio RTC) is captured: from :(60-PREROLL) for CAP_SECS seconds, 8 kHz audio from the
 * loader goes to the SD card, alternating <MODE>CAPA.PCM / <MODE>CAPB.PCM.  A finished capture is decoded
 * while the NEXT minute is already being recorded (the decoder's read callback and abort poll keep pumping
 * audio into the other file), so a decode may take up to ~a minute and no minute is skipped.  If a decode is
 * still running when the next capture is complete, it is cut off ("decode out of time", decodes found so far
 * are kept) and the newer minute is decoded.  Each decode is printed on the DECODE screen as
 *   "HHMM SNR  DT FREQ c  MESSAGE"  (WSJT-X layout, c = WSJT-X mode marker, HHMM = the minute it was sent in);
 *   only decodes go to the DECODE screen - status and errors go to the app window (line 2) and the log
 * and kept in <MODE>DEC.TXT (newest ~2 KB).  Optional <MODE>.CFG: "sub=G" (JT4 submode A..G), "flo=200",
 * "fhi=3000" (search range of tone 0, Hz).  Working memory = the loader's app data region (icap_heap()).
 * At most 3 files are open at once (capture, decode read, log).  Built only against the public SDK icap.h.
 */
#include "icap.h"
#ifdef DIGI_JT4
#include "jt4dec.h"
#define MODE_NAME  "JT4"
#define MODE_CHAR  '$'                  /* WSJT-X decode-line marker */
#define PREROLL    1u                   /* start capturing at :59 (radio clock up to ~1 s late) */
#define CAP_SECS   55u                  /* :59 .. :54 - TX 47.1 s from ~:01, EME DT up to +4 s */
#define NOM_START  2.0f                 /* TX start in the capture = preroll + 1 s */
#define NOM_MIN    1.0f                 /* lowest nominal the decoder was validated with (DT search -1 s) */
#define E_ABORT    JT4_ERR_ABORT
#define E_ARG      JT4_ERR_ARGS
#define E_IO       JT4_ERR_IO
#elif defined(DIGI_PI4)
#include "pi4dec.h"
#define MODE_NAME  "PI4"
#define MODE_CHAR  ' '                  /* PI4 is not in WSJT-X: no marker */
#define PREROLL    2u                   /* start capturing at :58 (radio clock up to ~2 s off) */
#define CAP_SECS   29u                  /* :58 .. :27 - 24.3 s of PI4 data from :00 (CW ID / carrier follow) */
#define NOM_START  2.0f
#define NOM_MIN    0.0f
#define E_ABORT    PI4_ERR_ABORT
#define E_ARG      PI4_ERR_ARG
#define E_IO       PI4_ERR_IO
#elif defined(DIGI_FT8)
#include "ft8dec.h"
#define MODE_NAME  "FT8"
#define MODE_CHAR  '~'                  /* WSJT-X FT8 decode-line marker */
#define PERIOD     15                   /* s: :00 :15 :30 :45 */
#define PREROLL    0u                   /* capture from the period start (TX starts 0.5 s into it) */
#define CAP_SAMP   (146u * FS / 10u)    /* 14.6 s: TX 12.64 s from :00.5 + DT up to ~+1.4 s, ends before the next edge */
#define CAP_TMO_MS 14800u               /* by time (no audio while transmitting): still before the next period's edge */
#define NOM_START  0.5f                 /* DT = 0 <=> signal starts 0.5 s into the period */
#define NOM_MIN    (-1.0f)              /* edge not seen: the decoder searches DT -2.3..+2.4 s around the nominal */
#define E_ABORT    FT8_ERR_ABORT
#define E_ARG      FT8_ERR_ARGS
#define E_IO       FT8_ERR_IO
#define F_MAXCFG   3900                 /* tone 0 below 4 kHz - 50 Hz */
#define MAXOUT     50
#define PERIOD_WORD "period"
#define HIST_SZ    6144                 /* ~3 busy periods of decodes in FT8DEC.TXT (was 8192; trimmed so the
                                         * integrated beacon encoder fits the 9700 window without touching the arena) */
#else
#error "define DIGI_JT4, DIGI_PI4 or DIGI_FT8"
#endif
#ifndef PERIOD                          /* the minute modes */
#define PERIOD     60
#define CAP_SAMP   (CAP_SECS * FS)
#define CAP_TMO_MS (CAP_SECS * 1000u + 500u)
#define F_MAXCFG   3000
#define MAXOUT     4
#define PERIOD_WORD "minute"
#define HIST_SZ    2048
#endif
#define NKEYS      (86400 / PERIOD)     /* periods per day */

#define FS        8000u
#define F_DEC     MODE_NAME "DEC.TXT"
#define F_CFG     MODE_NAME ".CFG"

#ifdef DIGI_HOST
#include <string.h>
#else                                            /* freestanding on the radio; the host harness links libc */
void *memcpy(void *d, const void *s, unsigned n) { char *p = d; const char *q = s; while (n--) *p++ = *q++; return d; }
void *memset(void *d, int c, unsigned n) { char *p = d; while (n--) *p++ = (char)c; return d; }
#endif

static const char *const F_CAP[2] = { MODE_NAME "CAPA.PCM", MODE_NAME "CAPB.PCM" };

static char *cat(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }
static char *num(char *d, int v, int w, char pad)
{
    char t[12]; int n = 0; unsigned u = v < 0 ? (unsigned)-v : (unsigned)v;
    do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u && n < 10);
    if (v < 0) t[n++] = '-';
    while (n < w && n < 11) t[n++] = pad;
    while (n) *d++ = t[--n];
    *d = 0; return d;
}
static char *fix1(char *d, float f, int w)       /* one decimal, right-aligned in w */
{
    if (f > 999.0f) f = 999.0f; else if (f < -999.0f) f = -999.0f;
    int v = (int)(f * 10.0f + (f < 0 ? -0.5f : 0.5f)), a = v < 0 ? -v : v;
    char t[16], *q = t;
    if (v < 0) *q++ = '-';
    q = num(q, a / 10, 0, ' '); *q++ = '.'; *q++ = (char)('0' + a % 10); *q = 0;
    int l = (int)(q - t); while (l < w) { *d++ = ' '; l++; }
    return cat(d, t);
}
static char *hhmm(char *d, int hh, int mm) { d = num(d, hh, 2, '0'); return num(d, mm, 2, '0'); }
#if PERIOD == 60
#define STAMP(d, hh, mm, ss) hhmm(d, hh, mm)                      /* "HHMM" */
#else
#define STAMP(d, hh, mm, ss) num(hhmm(d, hh, mm), ss, 2, '0')     /* "HHMMSS" */
#endif

static int stop(void) { return icap_should_stop(); }

/* ---- config */
static int cfg_sub = 6;                          /* JT4G */
static float cfg_flo = 200.0f, cfg_fhi = 3000.0f;
/* beacon (beacon=1): the app transmits instead of decoding; call=/loc= override @MYCALL/@MYLOC, txfreq= sets the
 * audio tone-0 Hz, cwshift= the PI4 FSK-CW shift Hz.  txlevel/txmax/txlimit are read by the loader, not here. */
static int cfg_beacon = 0, cfg_txfreq = 0, cfg_cwshift = 250;
static char cfg_call[16], cfg_loc[12];
#include "spot.h"                                /* v4: report decodes to the loader @SPOT service (no per-app pskr engine) */
static int kmatch(const char *s, const char *k) { int i = 0; while (k[i] && s[i] == k[i]) i++; return k[i] == 0 && s[i] == '='; }
static int kint(const char *s)                   /* digits after '=' */
{ int v = 0, nd = 0; while (*s && *s != '=') s++; if (*s) s++; for (; *s >= '0' && *s <= '9' && nd < 6; s++, nd++) v = v * 10 + (*s - '0'); return nd ? v : -1; }
static void ktok(const char *s, char *out, int cap)  /* token after '=' (stops at blank/eol), upper-cased */
{ int k = 0; while (*s && *s != '=') s++; if (*s) s++; while (*s == ' ') s++;
  for (; *s > ' ' && k < cap - 1; s++) out[k++] = (*s >= 'a' && *s <= 'z') ? (char)(*s - 32) : *s; out[k] = 0; }
static void read_cfg(void)
{
    char b[160]; int fd = icap_open(F_CFG, ICAP_O_READ);
    if (fd < 0) return;
    int n = icap_read(fd, b, sizeof b - 1); icap_close(fd);
    if (n <= 0) return;
    b[n] = 0;
    for (char *s = b; *s; s++) {
        if (s[0] == 's' && s[1] == 'u' && s[2] == 'b' && s[3] == '=') { char c = (char)(s[4] & ~0x20); if (c >= 'A' && c <= 'G') cfg_sub = c - 'A'; }
        int lo = s[0] == 'f' && s[1] == 'l' && s[2] == 'o' && s[3] == '=';
        int hi = s[0] == 'f' && s[1] == 'h' && s[2] == 'i' && s[3] == '=';
        if (lo || hi) {
            int v = 0, nd = 0; for (char *p = s + 4; *p >= '0' && *p <= '9' && nd < 5; p++, nd++) v = v * 10 + (*p - '0');
            if (nd && v <= F_MAXCFG) { if (lo) cfg_flo = (float)v; else cfg_fhi = (float)v; }   /* tone 0 must leave room */
        }
        /* new beacon keys: matched only at a token start (after newline/blank), so "call=" != "...call=" */
        if (s != b && s[-1] != '\n' && s[-1] != '\r' && s[-1] != ' ' && s[-1] != '\t') continue;
        if (kmatch(s, "beacon")) { int v = kint(s); if (v >= 0) cfg_beacon = v != 0; }
        else if (kmatch(s, "call")) ktok(s, cfg_call, sizeof cfg_call);
        else if (kmatch(s, "loc"))  ktok(s, cfg_loc, sizeof cfg_loc);
        else if (kmatch(s, "txfreq")) { int v = kint(s); if (v >= 100 && v <= 3800) cfg_txfreq = v; }
        else if (kmatch(s, "cwshift")) { int v = kint(s); if (v >= 0 && v <= 1000) cfg_cwshift = v; }
    }
}

/* ---- log (no append mode in the API: keep the newest ~2 KB in RAM, loaded at start, and rewrite) */
static char hist[HIST_SZ]; static uint32_t hl;
static void log_load(void)
{
    int fd = icap_open(F_DEC, ICAP_O_READ); if (fd < 0) return;
    static char t[512]; int r;
    while ((r = icap_read(fd, t, sizeof t)) > 0) {
        if (hl + (uint32_t)r > sizeof hist) { uint32_t cut = hl + (uint32_t)r - sizeof hist; while (cut < hl && hist[cut] != '\n') cut++; if (cut < hl) cut++; memcpy(hist, hist + cut, hl - cut); hl -= cut; }
        if ((uint32_t)r > sizeof hist - hl) r = (int)(sizeof hist - hl);
        memcpy(hist + hl, t, (unsigned)r); hl += (uint32_t)r;
    }
    icap_close(fd);
    if (hl && hist[hl - 1] != '\n') { if (hl + 2 > sizeof hist) hl = sizeof hist - 2; hist[hl++] = '\r'; hist[hl++] = '\n'; }
}
#ifdef DIGI_FT8
static void log_add(const char *s)
{
    int n = 0;
    while (s[n] && n < 90) n++;
    if (hl + (uint32_t)n + 2 > sizeof hist) { uint32_t cut = hl / 2; while (cut < hl && hist[cut] != '\n') cut++; if (cut < hl) cut++; memcpy(hist, hist + cut, hl - cut); hl -= cut; }
    memcpy(hist + hl, s, (unsigned)n); hl += (uint32_t)n; hist[hl++] = '\r'; hist[hl++] = '\n';
}
static void log_flush(void)
{
    int fd = icap_open(F_DEC, ICAP_O_WRITE);
    if (fd >= 0) { icap_write(fd, hist, hl); icap_close(fd); }
}
static void log_line(const char *s) { log_add(s); log_flush(); }
#else
static void log_line(const char *s)
{
    int fd, n = 0;
    while (s[n] && n < 90) n++;
    if (hl + (uint32_t)n + 2 > sizeof hist) { uint32_t cut = hl / 2; while (cut < hl && hist[cut] != '\n') cut++; if (cut < hl) cut++; memcpy(hist, hist + cut, hl - cut); hl -= cut; }
    memcpy(hist + hl, s, (unsigned)n); hl += (uint32_t)n; hist[hl++] = '\r'; hist[hl++] = '\n';
    fd = icap_open(F_DEC, ICAP_O_WRITE);
    if (fd >= 0) { icap_write(fd, hist, hl); icap_close(fd); }
}
#endif
/* DECODE screen = decoded lines only (WSJT-X style); everything else -> app window line 2 + the log */
#ifndef DIGI_FT8
static void say(const char *s) { icap_decode_out(s); log_line(s); }
#else
static void say_nf(const char *s) { icap_decode_out(s); log_add(s); }   /* a period's decodes: one log write */
#endif
static void note(const char *s) { icap_set_line(2, s); log_line(s); }

/* ---- capture: a state machine driven by pump(), which runs from the main loop AND from inside a decode */
static int16_t abuf[800];                        /* 0.1 s */
static struct {
    int fd, slot;                                /* file being written (-1 none), its slot 0/1 */
    uint32_t n, t0;                              /* samples written, tick at start */
    int hh, mm;                                  /* the minute (period) this capture belongs to */
    int last_sec, clock_bad, last_key;           /* last_key: the period (minute of day) last captured */
    float late;                                  /* start lateness (s) -> subtracted from NOM_START */
    int werr, ovr, gap_ms;                       /* SD write error / overruns / audio padded with zeros (ms) */
#if PERIOD != 60
    int ss;
#endif
} cap = { -1, 0, 0, 0, 0, 0, -1, 0, -1, 0.0f, 0, 0, 0
#if PERIOD != 60
    , 0
#endif
};
static struct {
    int ready, hh, mm; uint32_t n; float nom; int werr, gap_ms;
#if PERIOD != 60
    int ss;
#endif
} done[2];                                       /* captures waiting */
static uint32_t gaps;
static int open_err, lost;                       /* capture file could not be opened / ready minute overwritten */
static int decoding = -1;                        /* slot being decoded, -1 none */

#ifdef DIGI_CAPBUF
/* IC-9700 (slow SD cards): the capture is written in DIGI_CAPBUF-sample (8 KiB) calls instead of one per audio
 * read (~20 ms = 320 B, zeros in 400 B), so a card with ~0.1-0.3 s per write call keeps up; cap.n counts the
 * buffered samples too (time base / zero-fill unchanged), a failed write takes them back off (n == file). */
static int16_t cbuf[DIGI_CAPBUF];
static uint32_t cbn;
static void cap_flush(void)
{
    if (cbn && cap.fd >= 0 && !cap.werr && icap_write(cap.fd, cbuf, cbn * 2) != (int)(cbn * 2)) { cap.werr = 1; cap.n -= cbn; }
    cbn = 0;
}
#endif
static void cap_finish(void)
{
#ifdef DIGI_CAPBUF
    cap_flush();
#endif
    icap_close(cap.fd); cap.fd = -1;
    done[cap.slot].ready = 1; done[cap.slot].hh = cap.hh; done[cap.slot].mm = cap.mm; done[cap.slot].n = cap.n;
#if PERIOD != 60
    done[cap.slot].ss = cap.ss;
#endif
    done[cap.slot].nom = NOM_START - cap.late < NOM_MIN ? NOM_MIN : NOM_START - cap.late; done[cap.slot].werr = cap.werr; done[cap.slot].gap_ms = cap.gap_ms;
}
static void cap_start(int key, float late)
{
    for (int tries = 0; tries < 2; tries++) {
        int slot = cap.slot ^ 1;
        if (slot == decoding) slot ^= 1;         /* never overwrite the file being decoded */
        if (slot == decoding) return;
        int fd = icap_open(F_CAP[slot], ICAP_O_WRITE);
        if (fd < 0) { open_err = 1; cap.slot = slot; continue; }   /* try the other slot next */
        if (done[slot].ready) lost++;            /* an undecoded minute is being replaced */
        done[slot].ready = 0;
        cap.fd = fd; cap.slot = slot; cap.n = 0; cap.t0 = icap_ticks();
#if PERIOD == 60
        cap.hh = key / 60; cap.mm = key % 60; cap.late = late;
#else
        cap.hh = key * PERIOD / 3600; cap.mm = key * PERIOD / 60 % 60; cap.ss = key * PERIOD % 60; cap.late = late;
#endif
        cap.werr = 0; cap.ovr = 0; cap.gap_ms = 0;
#ifdef DIGI_CAPBUF
        cbn = 0;
#endif
        return;
    }
}
static void cap_put(const int16_t *x, uint32_t k)
{
    if (cap.n + k > CAP_SAMP) k = CAP_SAMP - cap.n;
    if (!k) return;
#ifdef DIGI_CAPBUF
    while (k && !cap.werr) {                     /* x == 0: zeros for lost audio, so later samples keep their time */
        uint32_t m = DIGI_CAPBUF - cbn; if (m > k) m = k;
        for (uint32_t i = 0; i < m; i++) cbuf[cbn + i] = x ? x[i] : 0;
        cbn += m; cap.n += m; k -= m; if (x) x += m;
        if (cbn == DIGI_CAPBUF) cap_flush();
    }
    return;
#endif
    if (x) { if (icap_write(cap.fd, x, k * 2) != (int)(k * 2)) { cap.werr = 1; return; } }   /* keep n == file */
    else {                                       /* zeros for lost audio, so later samples keep their time */
        static const int16_t z[200];
        uint32_t left = k;
        while (left) { uint32_t m = left > 200 ? 200 : left; if (icap_write(cap.fd, z, m * 2) != (int)(m * 2)) { cap.werr = 1; return; } left -= m; cap.n += m; }
        return;
    }
    cap.n += k;
}
static void pump(void)
{
    for (int guard = 0; guard < 64; guard++) {   /* bounded: never spin on a stuck overrun */
        int r = icap_audio_read(abuf, (int)(sizeof abuf / sizeof abuf[0]));
        if (r == ICAP_E_OVERRUN) { gaps++; continue; }
        if (r <= 0) {
            /* ring drained: the file must hold ~(now - start) of audio.  More than 250 ms missing (overrun,
             * stall, TX) -> fill the hole with zeros so later samples keep their time (DT stays right) */
            if (cap.fd >= 0 && !cap.werr) {
                uint32_t want = (icap_ticks() - cap.t0) * (FS / 1000u);
                if (want > cap.n + 2000u) {
                    uint32_t pad = want - cap.n - 80u;
#ifdef DIGI_CAPBUF
                    if (pad > CAP_SAMP - cap.n) pad = CAP_SAMP - cap.n;   /* capture already complete: no gap to report */
#endif
                    cap_put(0, pad); cap.gap_ms += (int)(pad / (FS / 1000u));
                }
            }
            break;
        }
        if (cap.fd >= 0 && !cap.werr) cap_put(abuf, (uint32_t)r);
    }
    /* end of a capture: by samples, or by time (no audio while transmitting) */
    if (cap.fd >= 0 && (cap.n >= CAP_SAMP || icap_ticks() - cap.t0 > CAP_TMO_MS)) cap_finish();
    /* start of a capture: the second edge :(59-PREROLL) -> :(60-PREROLL) */
    struct icap_time t;
    if (icap_clock_now(&t) != 0 || t.year < 2020) { cap.clock_bad = 1; return; }
    cap.clock_bad = 0;
    /* start of a capture: from :(60-PREROLL) up to :00 (late: a missed second edge, a stall, an RTC step) */
#if PERIOD == 60
    if (cap.fd < 0 && (t.sec >= (int)(60 - PREROLL) || t.sec == 0)) {
        int key = (t.hour * 60 + t.min + (t.sec ? 1 : 0)) % 1440;   /* the period that starts at that :00 */
        if (key != cap.last_key) {
            float late = 0.0f;
            int ds = (t.sec - (int)(60 - PREROLL) + 60) % 60;      /* whole seconds past the ideal start */
            if (!(ds == 0 && cap.last_sec == (int)(59 - PREROLL))) late = (float)ds + 0.5f;   /* edge not seen */
            cap.last_key = key;
            cap_start(key, late);
        }
    }
    cap.last_sec = t.sec;
#else                                            /* the same per PERIOD: sp = second within the period */
    int sp = t.sec % PERIOD;
    if (cap.fd < 0 && (sp >= (int)(PERIOD - PREROLL) || sp == 0)) {
        int key = (t.hour * (3600 / PERIOD) + t.min * (60 / PERIOD) + t.sec / PERIOD + (sp ? 1 : 0)) % NKEYS;
        if (key != cap.last_key) {
            float late = 0.0f;
            int ds = (sp - (int)(PERIOD - PREROLL) + PERIOD) % PERIOD;
            if (!(ds == 0 && cap.last_sec == (int)(PERIOD - 1 - PREROLL))) late = (float)ds + 0.5f;
            cap.last_key = key;
            cap_start(key, late);
        }
    }
    cap.last_sec = sp;
#endif
}

/* ---- SD-backed sample source for the decoder (no seek in the API: reopen + skip when reading backwards) */
static struct { int fd; uint32_t pos, len; int slot; } rd = { -1, 0, 0, 0 };
#ifndef DIGI_RDBUF
static int16_t rbuf[512];
#endif
#ifdef DIGI_RDBUF
/* IC-9700 (slow SD cards): the capture is read in DIGI_RDBUF-sample calls through a forward cache instead of
 * 512-sample calls (the per-call cost dominates on such a card); the same samples reach the decoder. */
static int16_t rcache[DIGI_RDBUF];
static uint32_t rclen;                           /* the cache holds file samples [rd.pos - rclen, rd.pos) */
static int src_read(void *ctx, uint32_t start, float *out, uint32_t n)
{
    (void)ctx;
    if (start >= rd.len) return 0;
    if (start + n > rd.len) n = rd.len - start;
    if (rd.fd < 0 || start < rd.pos - rclen) {   /* first read / backwards: reopen (no seek in the API) */
        if (rd.fd >= 0) icap_close(rd.fd);
        rd.fd = icap_open(F_CAP[rd.slot], ICAP_O_READ); rd.pos = 0; rclen = 0;
        if (rd.fd < 0) return -1;
    }
    uint32_t got_n = 0;
    while (got_n < n) {
        uint32_t idx = start + got_n;
        if (idx >= rd.pos) {                     /* next chunk (also skips forward) */
            int r = icap_read(rd.fd, rcache, DIGI_RDBUF * 2);
            if (r < 2) return got_n ? (int)got_n : -1;
            rclen = (uint32_t)r / 2; rd.pos += rclen;
            pump();                              /* keep capturing the next minute while decoding */
            continue;
        }
        const int16_t *c = rcache + (idx - (rd.pos - rclen));
        uint32_t m = rd.pos - idx; if (m > n - got_n) m = n - got_n;
        for (uint32_t i = 0; i < m; i++) out[got_n + i] = (float)c[i] * (1.0f / 32768.0f);
        got_n += m;
    }
    return (int)got_n;
}
#else
static int src_read(void *ctx, uint32_t start, float *out, uint32_t n)
{
    (void)ctx;
    if (start >= rd.len) return 0;
    if (start + n > rd.len) n = rd.len - start;
    if (rd.fd < 0 || start < rd.pos) {
        if (rd.fd >= 0) icap_close(rd.fd);
        rd.fd = icap_open(F_CAP[rd.slot], ICAP_O_READ); rd.pos = 0;
        if (rd.fd < 0) return -1;
    }
    while (rd.pos < start) {                     /* skip forward */
        uint32_t k = start - rd.pos; if (k > 512) k = 512;
        int r = icap_read(rd.fd, rbuf, k * 2); if (r <= 0) return -1;
        rd.pos += (uint32_t)r / 2;
        pump();
    }
    uint32_t got_n = 0;
    while (got_n < n) {
        uint32_t k = n - got_n; if (k > 512) k = 512;
        int r = icap_read(rd.fd, rbuf, k * 2); if (r <= 0) break;
        uint32_t got = (uint32_t)r / 2;
        for (uint32_t i = 0; i < got; i++) out[got_n + i] = (float)rbuf[i] * (1.0f / 32768.0f);
        got_n += got; rd.pos += got;
        pump();                                  /* keep capturing the next minute while decoding */
    }
    return (int)got_n;
}
#endif
#ifndef DIGI_NO_BEACON
#include "beacon.h"                              /* beacon TX driver + the runtime arm state (bcn_want_poll) */
#define WANT_BEACON(ms) bcn_want_poll(cfg_beacon, ms)
#else
#define WANT_BEACON(ms) 0
#endif
static uint32_t last_yield;
static int timed_out;
static int dec_abort(void)
{
    pump();
    if (icap_ticks() - last_yield > 100) { icap_yield(); last_yield = icap_ticks(); }   /* stay polite (rule 4) */
    if (stop()) return 1;
    if (decoding >= 0 && done[decoding ^ 1].ready) { timed_out = 1; return 1; }   /* next minute complete */
    if (WANT_BEACON(1000)) return 1;             /* the owner armed the beacon: cut the decode short, switch */
    return 0;
}

/* ================================================================= beacon TX ================
 * Activation is a RUNTIME state (v6): the owner arms / disarms the beacon on the radio (long press of the MODE
 * tile); the loader publishes it in @TX and the app follows it (bcn_want in beacon.h): while decoding it polls once a
 * second, and on "armed" it leaves the decode loop and transmits; when disarmed it re-initialises and decodes again.
 * beacon=1 in <MODE>.CFG stays the dev/bench override (the loader then arms at app start; an older loader without
 * @TX falls back to it).  DIGI_NO_BEACON compiles the beacon out entirely. */
#ifndef DIGI_NO_BEACON
/* v4 heap union: the beacon encoder state lives in the decoder arena (icap_heap, set in beacon_session), never a
 * permanent static reservation - decode and beacon are mutually exclusive. */
#ifdef DIGI_FT8
#include "ft8enc.h"
#include "msg77.h"
static ft8_tx_t *g_tx;
static int bcn_fill(void *c, int16_t *b, int n) { (void)c; return ft8_tx_fill(g_tx, b, n); }
static int bcn_prepare(const char *call, const char *loc, bcn_fill_t *f, void **c, const char **err)
{
    char msg[28]; int e = msg77_beacon_text("DE", call, loc, msg);     /* owner decision: DE CALL LOC4 (not CQ) */
    if (!e) e = ft8_tx_init(g_tx, msg, cfg_txfreq ? (double)cfg_txfreq : 1500.0, 0);
    if (e) { *err = e == TXE_CALL ? "call needs call= in CFG" : e == TXE_GRID ? "bad grid" : "encode error"; return -1; }
    *f = bcn_fill; *c = 0; return 0;
}
#elif defined(DIGI_JT4)
#include "jt4enc.h"
static jt4_tx_t *g_tx;
static int bcn_fill(void *c, int16_t *b, int n) { (void)c; return jt4_tx_fill(g_tx, b, n); }
static int bcn_prepare(const char *call, const char *loc, bcn_fill_t *f, void **c, const char **err)
{
    char msg[24]; int e = jt4_beacon_text(call, loc, msg);             /* owner decision: CALL LOC4 (free text) */
    if (!e) e = jt4_tx_init(g_tx, msg, cfg_txfreq ? (double)cfg_txfreq : 1500.0, cfg_sub, 0, 0);
    if (e) { *err = e == TXE_LEN ? "call too long for JT4" : e == TXE_MSG ? "use call= in CFG" : "encode error"; return -1; }
    *f = bcn_fill; *c = 0; return 0;
}
#elif defined(DIGI_PI4)
#include "pi4enc.h"
static pi4_tx_t *g_tx;
static int bcn_fill(void *c, int16_t *b, int n) { (void)c; return pi4_tx_fill(g_tx, b, n); }
static int bcn_prepare(const char *call, const char *loc, bcn_fill_t *f, void **c, const char **err)
{                                                                      /* PI4 frame + FSK-CW ID + carrier */
    double t0 = cfg_txfreq ? (double)cfg_txfreq - PI4_CARRIER_OFFSET_HZ : PI4_TONE0_DEFAULT_HZ;
    int e = pi4_tx_init(g_tx, call, call, loc, t0, (double)cfg_cwshift, 0, 0);
    if (e) { *err = e == TXE_LEN ? "CW ID too long" : e == TXE_CHAR ? "call not PI4-encodable" : "encode error"; return -1; }
    *f = bcn_fill; *c = 0; return 0;
}
#endif
static int16_t txbuf[160];
#endif /* !DIGI_NO_BEACON */

/* fresh capture / decode state: at start and when the app comes back from (or goes to) beacon mode.  A capture in
 * progress is dropped (its minute is lost anyway while the radio transmits). */
static void dec_reset(void)
{
    if (cap.fd >= 0) icap_close(cap.fd);
    if (rd.fd >= 0) icap_close(rd.fd);
    cap.fd = -1; cap.last_key = -1; cap.last_sec = -1; cap.werr = 0; cap.ovr = 0; cap.gap_ms = 0;
    rd.fd = -1; done[0].ready = done[1].ready = 0; decoding = -1; lost = 0; open_err = 0;
#ifdef DIGI_CAPBUF
    cbn = 0;
#endif
}

int app_main(const struct icap_svc *svc)
{
    icap_use(svc);
    read_cfg();
    if (cfg_flo >= cfg_fhi) { cfg_flo = 200.0f; cfg_fhi = 3000.0f; }
    char line[100], *q;
    void *work = icap_heap(); uint32_t wlen = icap_heap_size();
#ifdef DIGI_FT8
    ft8_state_t *st = (ft8_state_t *)work;       /* callsign hashes for the whole run (the heap is zeroed at launch) */
    {
        uint32_t sb = ((uint32_t)sizeof(ft8_state_t) + 7u) & ~7u;
        if (work && wlen > sb) { work = (char *)work + sb; wlen -= sb; }
    }
#endif
    int ok = icap_audio_rate() == (int)FS && work && wlen >= 64 * 1024, loaded = 0, clock_warned = 0;

    for (;;) {
#ifndef DIGI_NO_BEACON
        if (WANT_BEACON(0)) {                        /* armed: transmit every period until the owner disarms */
            dec_reset();
            g_tx = icap_heap();                      /* heap union: encoder state from the arena the decoder uses */
            bcn_follow = cfg_beacon;
            bcn_run(MODE_NAME, bcn_prepare, cfg_call, cfg_loc, txbuf, (int)(sizeof txbuf / sizeof txbuf[0]));
            if (stop()) break;
#ifdef DIGI_FT8
            memset(icap_heap(), 0, sizeof(ft8_state_t));   /* the encoder overwrote the callsign hash table */
#endif
            icap_clear();
        }
#endif
        q = cat(line, MODE_NAME " monitor");
#ifdef DIGI_JT4
        q = cat(q, " (JT4"); *q++ = (char)('A' + cfg_sub); *q++ = ')'; *q = 0;
#endif
        if (!ok) {
            icap_set_line(2, MODE_NAME ": needs 8 kHz audio + app region (firmware too old)");
            while (!stop() && !WANT_BEACON(1000)) icap_sleep_ms(200);
            if (stop()) return 1;
            continue;
        }
        if (!loaded) { log_load(); loaded = 1; }
        icap_set_line(0, line);
        icap_audio_read(abuf, 1);                    /* start the capture service */
        dec_reset();

    while (!stop()) {
        if (WANT_BEACON(1000)) break;                /* the owner armed the beacon: leave decoding */
        pump();
        if (open_err == 1) { note(MODE_NAME ": SD error - cannot create the capture file (card full / locked?)"); open_err = 2; }
        if (lost) { note(MODE_NAME ": a " PERIOD_WORD " was skipped (decoder behind)"); lost = 0; }
        if (cap.clock_bad && !clock_warned) { note(MODE_NAME ": radio clock not set - set the time"); clock_warned = 1; }
        int s = done[0].ready ? 0 : done[1].ready ? 1 : -1;
        if (s >= 0 && done[s ^ 1].ready) {      /* both waiting: decode the older one only if it is not stale */
#if PERIOD == 60
            int a = done[s].hh * 60 + done[s].mm, b = done[s ^ 1].hh * 60 + done[s ^ 1].mm;
            if ((b - a + 1440) % 1440 > 720) s ^= 1;   /* s = the older of the two */
#else
            int a = (done[s].hh * 3600 + done[s].mm * 60 + done[s].ss) / PERIOD, b = (done[s ^ 1].hh * 3600 + done[s ^ 1].mm * 60 + done[s ^ 1].ss) / PERIOD;
            if ((b - a + NKEYS) % NKEYS > NKEYS / 2) s ^= 1;
#endif
            q = STAMP(line, done[s].hh, done[s].mm, done[s].ss); cat(q, " " MODE_NAME ": skipped (decoder behind)");
            note(line); done[s].ready = 0; s ^= 1;
        }
        if (s < 0) { icap_sleep_ms(20); continue; }
        /* decode slot s while the next minute is captured into the other slot */
        decoding = s; done[s].ready = 0;
        rd.slot = s; rd.len = done[s].n; rd.pos = 0;
        int hh = done[s].hh, mm = done[s].mm, werr = done[s].werr, gap_ms = done[s].gap_ms;
#if PERIOD != 60
        int ss = done[s].ss;
#endif
        float nom = done[s].nom;
        dsrc_t src = { 0, src_read, done[s].n, FS };
        timed_out = 0;
#ifdef DIGI_JT4
        jt4_result_t res[MAXOUT];
        int nd = jt4_decode(&src, cfg_sub, cfg_flo, cfg_fhi, nom, res, MAXOUT, work, wlen, dec_abort);
#elif defined(DIGI_PI4)
        pi4_result_t res[MAXOUT];
        int nd = pi4_decode(&src, cfg_flo, cfg_fhi, nom, res, MAXOUT, work, wlen, dec_abort);
#else
        static ft8_result_t res[MAXOUT];
        int nd = ft8_decode(&src, cfg_flo, cfg_fhi, nom, res, MAXOUT, work, wlen, dec_abort, st);
#endif
        if (rd.fd >= 0) { icap_close(rd.fd); rd.fd = -1; }
        int late = timed_out;
        decoding = -1;
        if (stop()) break;
        if (werr || gap_ms >= 100) {
            q = STAMP(line, hh, mm, ss);
            if (werr) cat(q, " " MODE_NAME ": SD write error (capture cut short)");
            else { q = cat(q, " " MODE_NAME ": audio gap "); q = num(q, gap_ms, 0, ' '); cat(q, " ms (zero-filled)"); }
            note(line);
        }
        if (nd < 0 && (nd != E_ABORT || late)) {
            q = STAMP(line, hh, mm, ss);
            q = cat(q, nd == E_ABORT ? " " MODE_NAME ": decode out of time" : nd == E_IO ? " " MODE_NAME ": SD read error" : " " MODE_NAME ": decoder error ");
            if (nd != E_ABORT && nd != E_IO) num(q, nd, 0, ' ');
            note(line);
        }
        for (int i = 0; i < nd && i < MAXOUT; i++) {
#ifdef DIGI_FT8
            int dup = 0;                         /* the same text twice in a period: show it once */
            for (int j = 0; j < i && !dup; j++) { int k = 0; while (res[i].msg[k] && res[i].msg[k] == res[j].msg[k]) k++; dup = res[i].msg[k] == res[j].msg[k]; }
            if (dup) continue;
#endif
            q = STAMP(line, hh, mm, ss);
            q = num(q, (int)(res[i].snr_db + (res[i].snr_db < 0 ? -0.5f : 0.5f)), 4, ' ');   /* WSJT-X: %4d%5.1f%5d c  msg */
            q = fix1(q, res[i].dt_s, 5);
            q = num(q, (int)(res[i].freq_hz + 0.5f), 5, ' ');
            *q++ = ' '; *q++ = MODE_CHAR; *q++ = ' '; *q++ = ' ';
            for (int k = 0; k < (int)sizeof res[i].msg - 1 && res[i].msg[k]; k++) *q++ = res[i].msg[k];
            *q = 0;
#ifdef DIGI_FT8
            say_nf(line);
#else
            say(line);
#endif
            {   /* report this decode to the loader @SPOT service (loader adds dial + UTC + gate) */
                int psnr = (int)(res[i].snr_db + (res[i].snr_db < 0 ? -0.5f : 0.5f));
                spot_emit(MODE_NAME, res[i].msg, res[i].freq_hz, psnr);
            }
        }
#ifdef DIGI_FT8
        if (nd > 0) log_flush();
#endif
        if (nd > 0 && late) { q = STAMP(line, hh, mm, ss); cat(q, " " MODE_NAME ": (partial, decode out of time)"); note(line); }
    }
        if (stop()) break;                           /* else: armed -> the beacon session at the loop top */
    }
    if (cap.fd >= 0) icap_close(cap.fd);
    if (rd.fd >= 0) icap_close(rd.fd);
    return 0;
}
