/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * WSPR-2 decoder + beacon app for the ICAP loader (IC-705 E/J and IC-9700, one relocatable image).  Registered as
 * decoder mode WSPR (mkapp --mode WSPR): the loader starts it when the owner selects the mode (radio in USB-D,
 * DECODE screen) and stops it when the mode is left.  Like the FT8/JT4/PI4/Q65 apps it DECODES until the owner arms
 * the beacon (long press of the MODE tile -> loader @TX beacon=1, polled once a second) and then TRANSMITS (every
 * second period, see bcn_prepare) until the beacon is disarmed; decode and beacon never run at once.
 *
 * WSPR period = 120 s from the even UTC minute; the signal is 110.6 s long and starts at +1 s.
 *
 * Capture.  The other decoders store the raw 8 kHz audio of a period on the SD card and the decoder reads it back.
 * For WSPR that is 1.9 MB per period and the decoder would have to read it more than once, so this app runs the
 * decoder's down-converter WHILE the period is received (wspr_dc_*: mix at 1500 Hz, polyphase FIR, 8000 -> 375
 * samples/s) and writes only the s16 I/Q: 114 s = 42750 points = 171 KB per period, to WSPRIQA.BIN / WSPRIQB.BIN in
 * turn, in 8 KB write calls (slow IC-9700 cards).  When a period is complete its file is read straight into the
 * decoder arena (wspr_decode_iq16_cb) and decoded while the next period is already being captured into the other
 * file: the decoder's abort poll keeps pumping audio.  A decode still running when the next capture is complete is
 * cut off ("decode out of time"; decodes found so far are kept).
 *
 * Memory.  Everything large is in the loader's app data region (icap_heap): the decoder arena (WSPR_WORK_MIN,
 * 212 KiB) at its start and the capture state (down-converter 6.8 KB + 8 KB write buffer) at its end; needs
 * 226.7 KiB (232112 B) of the 240 KiB (705) / 243.9 KiB (9700).  The I/Q file is de-interleaved in place inside the arena, so
 * there is no read buffer.  The beacon encoder state uses the start of the same region (decode XOR beacon).
 *
 * Output.  Decodes -> DECODE screen + WSPRDEC.TXT (newest ~2 KB), WSJT-X WSPR layout with the audio frequency:
 *     "HHMM SNR  DT FREQ DR  CALL GRID DBM"      e.g. "0918 -24  0.5 1496  0  K1ABC FN42 33"
 * (HHMM = start of the period, FREQ = audio Hz of the signal centre, DR = drift Hz over the transmission), sorted
 * by frequency.  Status and errors -> app window (line 2/3) and the log.  Each decode with a plain callsign is
 * reported to the loader's @SPOT service (call + grid + tone Hz; the loader adds the dial frequency and UTC).
 *
 * Settings, as in the other decoder apps: the radio's own for this mode first - the loader's "@MODECFG" line
 * (mode=WSPR sub=- pskr=<0|1> [flo=<Hz> fhi=<Hz>] [scope=..] gen=<n>), read at start, every 2 s while idle (so a
 * changed gen= is followed) and before a period's decodes are reported - then the optional override WSPR.CFG on top
 * (pskr=, flo=, fhi=; it wins).  A missing key, a loader without @MODECFG and a line for another mode leave the
 * WSPR.CFG value or the default: pskr=1, 1390..1610 Hz (= wsprd's +-110 Hz).  pskr=0: no @SPOT.  flo/fhi = audio
 * Hz of the signal centre; the decoder can search 1350..1650 Hz at most, a wider range is clipped to that.
 * WSPR.CFG also holds the beacon keys: beacon= (dev override), call=, loc=, dbm= (default 23), txfreq= (audio
 * centre Hz, default 1500); txlevel/txmax/txlimit are read by the loader.
 *
 * Built only against the public SDK (icap.h) + wsprdec / wsprenc / txsynth.  Clock: the radio RTC must be set (UTC
 * minute parity decides the period); "radio clock not set" shows until it is.
 */
#include "icap.h"
#include "wsprdec.h"
#include "wsprenc.h"

#define FS          8000u
#define MODE_NAME   "WSPR"
#define IQ_TARGET   42750u                /* 114 s of I/Q per period (wsprd reads 114 s too): TX +1 .. +111.6 s + DT */
#define CAP_TMO_MS  118000u               /* hard stop before the next period edge */
#define IQ_WBUF     2048u                 /* I/Q points per SD write call (8 KB) */
#define IQ_RCHUNK   2048u                 /* I/Q points per SD read call (8 KB)  */
#define F_DEC       "WSPRDEC.TXT"
#define F_CFG       "WSPR.CFG"
#define MAXOUT      48                    /* decodes kept per period */
#define HIST_SZ     2048
#define F_CENTRE    1500                  /* audio Hz of the down-converter centre */
#define F_DEF_HALF  110                   /* default search half-width, Hz (wsprd) */
#define F_MAX_HALF  150                   /* widest search the decoder supports    */

#ifdef DIGI_HOST
#include <string.h>
#else                                            /* freestanding on the radio; the host harness links libc */
void *memcpy(void *d, const void *s, unsigned n) { char *p = d; const char *q = s; while (n--) *p++ = *q++; return d; }
void *memset(void *d, int c, unsigned n) { char *p = d; while (n--) *p++ = (char)c; return d; }
#endif

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
static int rnd(float f) { return (int)(f + (f < 0 ? -0.5f : 0.5f)); }
static int stop(void) { return icap_should_stop(); }

#include "spot.h"                                /* @SPOT client (spot_emit_cg) */
#include "beacon.h"                              /* beacon TX driver + the runtime arm state + bcn_field */

/* ---- settings: WSPR.CFG (file) < @MODECFG (loader, live) ---- */
static int cfg_beacon = 0, cfg_dbm = 23, cfg_txfreq = 0;
static int cfg_pskr = -1, cfg_flo = -1, cfg_fhi = -1;                                       /* WSPR.CFG values, -1 = not given */
static char cfg_call[16], cfg_loc[12];
static int g_pskr = 1, g_flo = F_CENTRE - F_DEF_HALF, g_fhi = F_CENTRE + F_DEF_HALF;         /* in force */
static int g_shown;                              /* the settings line shows the values in force */

static int kmatch(const char *s, const char *k) { int i = 0; while (k[i] && s[i] == k[i]) i++; return k[i] == 0 && s[i] == '='; }
static int kint(const char *s)                   /* integer after '=', -1000 = none */
{
    int v = 0, nd = 0, neg = 0;
    while (*s && *s != '=') s++;
    if (*s) s++;
    if (*s == '-') { neg = 1; s++; }
    for (; *s >= '0' && *s <= '9' && nd < 5; s++, nd++) v = v * 10 + (*s - '0');
    return nd ? (neg ? -v : v) : -1000;
}
static void ktok(const char *s, char *out, int cap)  /* token after '=' (stops at blank/eol), upper-cased */
{
    int k = 0;
    while (*s && *s != '=') s++;
    if (*s) s++;
    while (*s == ' ') s++;
    for (; *s > ' ' && k < cap - 1; s++) out[k++] = (*s >= 'a' && *s <= 'z') ? (char)(*s - 32) : *s;
    out[k] = 0;
}
static void read_cfg(void)
{
    char b[200]; int fd = icap_open(F_CFG, ICAP_O_READ);
    if (fd < 0) return;
    int n = icap_read(fd, b, (int)sizeof b - 1); icap_close(fd);
    if (n <= 0) return;
    b[n] = 0;
    for (char *s = b; *s; s++) {
        if (s != b && s[-1] != '\n' && s[-1] != '\r' && s[-1] != ' ' && s[-1] != '\t') continue;   /* token start only */
        if (kmatch(s, "beacon")) { int v = kint(s); if (v != -1000) cfg_beacon = v != 0; }
        else if (kmatch(s, "call")) ktok(s, cfg_call, sizeof cfg_call);
        else if (kmatch(s, "loc"))  ktok(s, cfg_loc, sizeof cfg_loc);
        else if (kmatch(s, "dbm"))  { int v = kint(s); if (v >= 0 && v <= 60) cfg_dbm = v; }
        else if (kmatch(s, "txfreq")) { int v = kint(s); if (v >= 100 && v <= 3800) cfg_txfreq = v; }
        else if (kmatch(s, "pskr")) { int v = kint(s); if (v != -1000) cfg_pskr = v != 0; }
        else if (kmatch(s, "flo"))  { int v = kint(s); if (v >= 0 && v <= 4000) cfg_flo = v; }
        else if (kmatch(s, "fhi"))  { int v = kint(s); if (v >= 0 && v <= 4000) cfg_fhi = v; }
    }
}
/* put a requested range (audio Hz) into force: clipped to what the decoder can search; a range that misses it
 * altogether (e.g. the 200..3000 Hz default of the wide modes clips fine, 2000..3000 does not) selects the default */
static void set_range(int lo, int hi)
{
    if (lo < F_CENTRE - F_MAX_HALF) lo = F_CENTRE - F_MAX_HALF;
    if (hi > F_CENTRE + F_MAX_HALF) hi = F_CENTRE + F_MAX_HALF;
    if (lo >= hi) { lo = F_CENTRE - F_DEF_HALF; hi = F_CENTRE + F_DEF_HALF; }
    if (lo != g_flo || hi != g_fhi) g_shown = 0;
    g_flo = lo; g_fhi = hi;
}
/* defaults < @MODECFG < WSPR.CFG -> the values in force.  Every loader without @MODECFG answers the open with an
 * error.  A busy file table (ICAP_E_NOFD) keeps the previous values. */
static uint32_t mc_t;
static void mcfg_read(void)
{
    char b[128]; int pskr = 1, lo = F_CENTRE - F_DEF_HALF, hi = F_CENTRE + F_DEF_HALF;
    int fd = icap_open("@MODECFG", ICAP_O_READ);
    mc_t = icap_ticks();
    if (fd == ICAP_E_NOFD) return;
    if (fd >= 0) {
        int n = icap_read(fd, b, (int)sizeof b - 1);
        icap_close(fd);
        if (n > 0) {
            b[n] = 0;
            for (int i = 0; i < n; i++) if (b[i] == '\r' || b[i] == '\n') { b[i] = 0; break; }
            if (b[0] == 'm' && b[1] == 'o' && b[2] == 'd' && b[3] == 'e' && b[4] == '=' && b[5] == 'W' && b[6] == 'S'
                && b[7] == 'P' && b[8] == 'R' && (b[9] == ' ' || b[9] == 0)) {       /* the line is for this mode */
                pskr = bcn_field(b, "pskr", pskr) != 0;
                lo = bcn_field(b, "flo", lo); hi = bcn_field(b, "fhi", hi);
            }
        }
    }
    if (cfg_pskr >= 0) pskr = cfg_pskr;          /* the override file wins */
    if (cfg_flo >= 0) lo = cfg_flo;
    if (cfg_fhi >= 0) hi = cfg_fhi;
    if (pskr != g_pskr) g_shown = 0;
    g_pskr = pskr;
    set_range(lo, hi);
}
static void mcfg_poll(uint32_t ms) { if (icap_ticks() - mc_t >= ms) mcfg_read(); }
static void show_cfg(void)                       /* app window line 1: "1390-1610 Hz, spots on" */
{
    char l[40], *q;
    if (g_shown) return;
    q = num(l, g_flo, 0, ' '); *q++ = '-'; q = num(q, g_fhi, 0, ' '); cat(q, g_pskr ? " Hz, spots on" : " Hz, spots off");
    icap_set_line(1, l); g_shown = 1;
}

/* ---- log (no append mode in the API: keep the newest ~2 KB in RAM, loaded at start, and rewrite) ---- */
static char hist[HIST_SZ]; static uint32_t hl;
static void log_load(void)
{
    int fd = icap_open(F_DEC, ICAP_O_READ); if (fd < 0) return;
    static char t[256]; int r;
    while ((r = icap_read(fd, t, sizeof t)) > 0) {
        if (hl + (uint32_t)r > sizeof hist) { uint32_t cut = hl + (uint32_t)r - sizeof hist; while (cut < hl && hist[cut] != '\n') cut++; if (cut < hl) cut++; memcpy(hist, hist + cut, hl - cut); hl -= cut; }
        if ((uint32_t)r > sizeof hist - hl) r = (int)(sizeof hist - hl);
        memcpy(hist + hl, t, (unsigned)r); hl += (uint32_t)r;
    }
    icap_close(fd);
    if (hl && hist[hl - 1] != '\n') { if (hl + 2 > sizeof hist) hl = sizeof hist - 2; hist[hl++] = '\r'; hist[hl++] = '\n'; }
}
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
static void note(const char *s) { icap_set_line(2, s); log_add(s); log_flush(); }   /* status / errors */

/* ---- capture: a state machine driven by pump(), which runs from the main loop AND from inside a decode ---- */
typedef struct { wspr_dc_t dc; int16_t iqw[IQ_WBUF * 2]; uint32_t iqwn; } capscr_t;
static capscr_t *CS;                             /* at the end of the app data region */
static int16_t abuf[800];                        /* 0.1 s */
static struct {
    int fd, slot;                                /* file being written (-1 none), its slot 0/1 */
    uint32_t np, t0;                             /* points stored (buffered ones too), tick of the even minute */
    int hh, mm;                                  /* the even minute this capture belongs to */
    int last_sp, last_key, clock_bad, werr;
    int gap_ms;                                  /* audio missing, zero-filled */
} cap = { -1, 0, 0, 0, 0, 0, -1, -1, 0, 0, 0 };
static struct { int ready, hh, mm; uint32_t np; int werr, gap_ms; uint32_t clip; } done[2];
static int open_err, lost, decoding = -1;
static int apk; static uint32_t aseen;           /* audio peak / samples heard since the last capture start */
static const char *const F_IQ[2] = { "WSPRIQA.BIN", "WSPRIQB.BIN" };

static void iqw_flush(void)
{
    if (CS->iqwn && cap.fd >= 0 && !cap.werr && icap_write(cap.fd, CS->iqw, CS->iqwn * 2) != (int)(CS->iqwn * 2)) {
        cap.werr = 1; cap.np -= CS->iqwn / 2;    /* np == points in the file */
    }
    CS->iqwn = 0;
}
static void iq_put(void *ctx, int16_t i, int16_t q)
{
    (void)ctx;
    if (cap.np >= IQ_TARGET || cap.werr) return;
    CS->iqw[CS->iqwn++] = i; CS->iqw[CS->iqwn++] = q; cap.np++;
    if (CS->iqwn >= IQ_WBUF * 2) iqw_flush();
}
static void cap_finish(void)
{
    iqw_flush();
    icap_close(cap.fd); cap.fd = -1;
    done[cap.slot].ready = 1; done[cap.slot].hh = cap.hh; done[cap.slot].mm = cap.mm; done[cap.slot].np = cap.np;
    done[cap.slot].werr = cap.werr; done[cap.slot].gap_ms = cap.gap_ms; done[cap.slot].clip = CS->dc.clip;
}
/* late_ms: how long ago the even minute was (0 = its second edge was seen) */
static void cap_start(int emin, uint32_t late_ms)
{
    for (int tries = 0; tries < 2; tries++) {
        int slot = cap.slot ^ 1;
        if (slot == decoding) slot ^= 1;         /* never overwrite the file being decoded */
        if (slot == decoding) return;
        int fd = icap_open(F_IQ[slot], ICAP_O_WRITE);
        if (fd < 0) { open_err = 1; cap.slot = slot; continue; }   /* try the other slot next */
        if (done[slot].ready) lost++;            /* an undecoded period is being replaced */
        done[slot].ready = 0;
        cap.fd = fd; cap.slot = slot; cap.np = 0; cap.werr = 0; cap.gap_ms = 0; CS->iqwn = 0;
        cap.hh = emin / 60; cap.mm = emin % 60;
        cap.t0 = icap_ticks() - late_ms;
        /* I/Q scale for this period from the audio level of the last one: peak -> 4000..8000 (x1..x64, power of
         * two), so that very quiet audio stays above the s16 step; 12 dB of headroom, clipping is counted */
        float g = 1.0f;
        if (aseen >= FS && apk > 0) while (g < 64.0f && (float)apk * g * 2.0f <= 8000.0f) g *= 2.0f;
        apk = 0; aseen = 0;
        wspr_dc_reset(&CS->dc, g, IQ_TARGET);
        if (late_ms) wspr_dc_push(&CS->dc, 0, late_ms * (FS / 1000u), iq_put, 0);   /* sample 0 = the even minute */
        return;
    }
}
static void pump(void)
{
    for (int guard = 0; guard < 64; guard++) {   /* bounded: never spin on a stuck overrun */
        int r = icap_audio_read(abuf, (int)(sizeof abuf / sizeof abuf[0]));
        if (r == ICAP_E_OVERRUN) continue;       /* the lost samples are zero-filled by time below */
        if (r <= 0) {
            /* ring drained: the stream must hold ~(now - even minute) of audio.  More than 250 ms missing (overrun,
             * stall, TX) -> zeros, so that later samples keep their time (DT stays right) */
            if (cap.fd >= 0 && !cap.werr && cap.np < IQ_TARGET) {
                uint32_t want = (icap_ticks() - cap.t0) * (FS / 1000u);
                if (want > CS->dc.nin + 2000u) {
                    uint32_t pad = want - CS->dc.nin - 80u;
                    wspr_dc_push(&CS->dc, 0, pad, iq_put, 0); cap.gap_ms += (int)(pad / (FS / 1000u));
                }
            }
            break;
        }
        for (int i = 0; i < r; i++) { int a = abuf[i] < 0 ? -abuf[i] : abuf[i]; if (a > apk) apk = a; }
        aseen += (uint32_t)r;
        if (cap.fd >= 0 && !cap.werr) wspr_dc_push(&CS->dc, abuf, (uint32_t)r, iq_put, 0);
    }
    /* end of a capture: by points, or by time */
    if (cap.fd >= 0 && (cap.np >= IQ_TARGET || cap.werr || icap_ticks() - cap.t0 > CAP_TMO_MS)) cap_finish();
    /* start of a capture: the even UTC minute (sp = second within the 2-minute period) */
    struct icap_time t;
    if (icap_clock_now(&t) != 0 || t.year < 2020) { cap.clock_bad = 1; return; }
    cap.clock_bad = 0;
    int sp = (t.min & 1) * 60 + t.sec;
    if (cap.fd < 0 && sp <= 1) {                 /* second 1 too: a missed edge, a stall, an RTC step */
        int emin = (t.hour * 60 + t.min) % 1440;
        if (emin != cap.last_key) {
            cap.last_key = emin;
            cap_start(emin, (sp == 0 && cap.last_sp == 119) ? 0u : (uint32_t)sp * 1000u + 500u);   /* edge seen / not */
        }
    }
    cap.last_sp = sp;
}

/* ---- decode: the finished I/Q file straight into the decoder arena ----
 * The file holds interleaved s16 I,Q.  Each chunk is read into the not yet filled part of qdst and de-interleaved
 * in place (the write index never passes the read index), so no read buffer exists (wsprdec.h: fill_iq contract). */
static int rd_slot;
static int fill_iq(void *ctx, int16_t *idst, int16_t *qdst, int np)
{
    (void)ctx;
    int fd = icap_open(F_IQ[rd_slot], ICAP_O_READ);
    if (fd < 0) return -1;
    uint32_t got = 0;
    while (got < (uint32_t)np) {
        uint32_t k = (uint32_t)np - got, room = (WSPR_NPOINTS - got) / 2u;
        if (k > room) k = room;
        if (k > IQ_RCHUNK) k = IQ_RCHUNK;
        if (!k) break;
        int16_t *t = qdst + got;
        int r = icap_read(fd, t, k * 4u);
        if (r < 4) break;
        uint32_t m = (uint32_t)r / 4u;
        for (uint32_t i = 0; i < m; i++) { int16_t a = t[2 * i], b = t[2 * i + 1]; idst[got + i] = a; qdst[got + i] = b; }
        got += m;
        pump();                                  /* keep capturing the next period while reading */
        if (m < k) break;
    }
    icap_close(fd);
    return got ? (int)got : -1;
}
#define WANT_BEACON(ms) bcn_want_poll(cfg_beacon, ms)
static uint32_t last_yield;
static int timed_out;
static int dec_abort(void)
{
    pump();
    if (icap_ticks() - last_yield > 100) { icap_yield(); last_yield = icap_ticks(); }   /* stay polite */
    if (stop()) return 1;
    if (decoding >= 0 && done[decoding ^ 1].ready) { timed_out = 1; return 1; }   /* the next period is complete */
    if (WANT_BEACON(1000)) return 1;             /* the owner armed the beacon: cut the decode short, switch */
    return 0;
}

/* ---- beacon TX: "<CALL> <GRID4> <DBM>" every period while the owner has the beacon armed ---- */
static wspr_tx_t *g_tx;                          /* encoder state at the start of the app data region */
static int16_t txbuf[160];
static int bcn_fill(void *c, int16_t *b, int n) { (void)c; return wspr_tx_fill(g_tx, b, n); }
static int bcn_prepare(const char *call, const char *loc, bcn_fill_t *f, void **c, const char **err)
{
    /* Slot parity.  The loader arms the next slot from the second within the MINUTE (it reduces the
     * clock modulo 60 s before it applies the mode period): for the 120 s WSPR period that is ":01 of this minute
     * + 2", i.e. an even (valid WSPR) minute only when tx_begin is called during an even minute - straight after a
     * transmission (which ends at :52 of an odd minute) it would arm an odd one.  So ask for a slot only inside an
     * even minute, clear of both edges; bcn_run retries once a second.  The beacon then transmits in every second
     * period (4 min cycle) and always on the even minute; a loader with a true 120 s grid arms the same slot. */
    struct icap_time t;
    if (icap_clock_now(&t) == 0 && ((t.min & 1) || t.sec < 2 || t.sec > 45)) { *err = "waits for an even minute"; return -1; }
    char g4[5]; int i; for (i = 0; i < 4 && loc[i]; i++) g4[i] = loc[i]; g4[i] = 0;      /* WSPR type 1 uses GRID4 */
    int e = wspr_tx_init(g_tx, call, g4, cfg_dbm, cfg_txfreq ? (double)cfg_txfreq : WSPR_F0_DEFAULT_HZ, 0, 0);
    if (e) { *err = e == TXE_CALL ? "compound call: set call=" : e == TXE_GRID ? "bad grid" : "encode error"; return -1; }
    *f = bcn_fill; *c = 0; return 0;
}

/* fresh capture / decode state: at start and when the app comes back from (or goes to) beacon mode.  A capture in
 * progress is dropped (its period is lost anyway while the radio transmits). */
static void dec_reset(void)
{
    if (cap.fd >= 0) icap_close(cap.fd);
    cap.fd = -1; cap.last_key = -1; cap.last_sp = -1; cap.werr = 0; cap.gap_ms = 0;
    done[0].ready = done[1].ready = 0; decoding = -1; lost = 0; open_err = 0; apk = 0; aseen = 0;
    if (CS) CS->iqwn = 0;
}

static wspr_result_t res[MAXOUT];

int app_main(const struct icap_svc *svc)
{
    icap_use(svc);
    read_cfg();
    mcfg_read();
    char line[100], *q;
    uint8_t *work = icap_heap(); uint32_t wlen = icap_heap_size();
    const uint32_t scr = ((uint32_t)sizeof(capscr_t) + 7u) & ~7u;
    int ok = icap_audio_rate() == (int)FS && work && wlen >= WSPR_WORK_MIN + scr + 8u, loaded = 0, clock_warned = 0;
    uint32_t wlen_dec = 0;

    for (;;) {
        if (WANT_BEACON(0)) {                        /* armed: transmit until the owner disarms */
            dec_reset();
            g_tx = icap_heap();
            bcn_follow = cfg_beacon;
            bcn_run(MODE_NAME, bcn_prepare, cfg_call, cfg_loc, txbuf, (int)(sizeof txbuf / sizeof txbuf[0]));
            if (stop()) break;
            icap_clear();
        }
        if (!ok) {
            icap_set_line(0, MODE_NAME " monitor");
            icap_set_line(2, MODE_NAME ": needs 8 kHz audio + app region (firmware too old)");
            while (!stop() && !WANT_BEACON(1000)) icap_sleep_ms(200);
            if (stop()) return 1;
            continue;
        }
        if (!loaded) { log_load(); loaded = 1; }
        /* the capture state at the end of the region (8-aligned), the decoder gets everything below it; built again
         * after a beacon session (the encoder used the start of the region, the arena is set up per decode anyway) */
        wlen_dec = (wlen - scr) & ~7u;
        CS = (capscr_t *)(work + wlen_dec);
        CS->iqwn = 0;
        wspr_dc_init(&CS->dc);
        icap_set_line(0, MODE_NAME " monitor");
        g_shown = 0; show_cfg();
        icap_audio_read(abuf, 1);                    /* start the capture service */
        dec_reset();

        while (!stop()) {
            if (WANT_BEACON(1000)) break;            /* the owner armed the beacon: leave decoding */
            pump();
            mcfg_poll(2000); show_cfg();
            if (open_err == 1) { note(MODE_NAME ": SD error - cannot create the capture file (card full / locked?)"); open_err = 2; }
            if (lost) { note(MODE_NAME ": a period was skipped (decoder behind)"); lost = 0; }
            if (cap.clock_bad && !clock_warned) { note(MODE_NAME ": radio clock not set - set the time"); clock_warned = 1; }
            int s = done[0].ready ? 0 : done[1].ready ? 1 : -1;
            if (s >= 0 && done[s ^ 1].ready) {       /* both waiting: drop the older one */
                int a = done[s].hh * 60 + done[s].mm, b = done[s ^ 1].hh * 60 + done[s ^ 1].mm;
                if ((b - a + 1440) % 1440 > 720) s ^= 1;   /* s = the older of the two */
                q = hhmm(line, done[s].hh, done[s].mm); cat(q, " " MODE_NAME ": skipped (decoder behind)");
                note(line); done[s].ready = 0; s ^= 1;
            }
            if (s < 0) { icap_sleep_ms(20); continue; }
            /* decode slot s while the next period is captured into the other slot */
            decoding = s; done[s].ready = 0; rd_slot = s;
            int hh = done[s].hh, mm = done[s].mm, werr = done[s].werr, gap_ms = done[s].gap_ms;
            uint32_t np = done[s].np, clip = done[s].clip, t_dec = icap_ticks();
            timed_out = 0;
            wspr_set_band((float)(g_flo - F_CENTRE), (float)(g_fhi - F_CENTRE));
            int nd = wspr_decode_iq16_cb(fill_iq, 0, (int)np, res, MAXOUT, work, wlen_dec, dec_abort);
            int late = timed_out, secs = (int)((icap_ticks() - t_dec + 500u) / 1000u);
            decoding = -1;
            if (stop()) break;
            if (werr || gap_ms >= 1000 || clip > np / 100u) {   /* (a second without audio: TX, a stall) */
                q = hhmm(line, hh, mm);
                if (werr) cat(q, " " MODE_NAME ": SD write error (capture cut short)");
                else if (gap_ms >= 1000) { q = cat(q, " " MODE_NAME ": audio gap "); q = num(q, gap_ms / 1000, 0, ' '); cat(q, " s (zero-filled)"); }
                else cat(q, " " MODE_NAME ": audio level jumped (clipped)");
                note(line);
            }
            if (nd < 0 && (nd != WSPR_ERR_ABORT || late)) {
                q = hhmm(line, hh, mm);
                q = cat(q, nd == WSPR_ERR_ABORT ? " " MODE_NAME ": decode out of time" : nd == WSPR_ERR_IO ? " " MODE_NAME ": SD read error" : " " MODE_NAME ": decoder error ");
                if (nd != WSPR_ERR_ABORT && nd != WSPR_ERR_IO) num(q, nd, 0, ' ');
                note(line);
            }
            if (nd > MAXOUT) nd = MAXOUT;
            if (nd > 0) mcfg_read();                 /* pskr= as it is now */
            int ord[MAXOUT];                         /* by frequency, like WSJT-X */
            for (int i = 0; i < nd; i++) { int j = i; while (j > 0 && res[ord[j - 1]].freq_hz > res[i].freq_hz) { ord[j] = ord[j - 1]; j--; } ord[j] = i; }
            for (int n = 0; n < nd; n++) {
                const wspr_result_t *r = &res[ord[n]];
                int tone = rnd((float)F_CENTRE + r->freq_hz), snr = rnd(r->snr_db);
                q = hhmm(line, hh, mm);              /* "HHMM SNR  DT FREQ DR  CALL GRID DBM" */
                q = num(q, snr, 4, ' ');
                q = fix1(q, r->dt_s, 5);
                q = num(q, tone, 5, ' ');
                q = num(q, (int)r->drift_hz, 3, ' ');   /* truncated like wsprd */
                *q++ = ' '; *q++ = ' ';
                for (int k = 0; k < (int)sizeof r->msg - 1 && r->msg[k]; k++) *q++ = r->msg[k];
                *q = 0;
                icap_decode_out(line); log_add(line);
                if (g_pskr && r->call[0] && r->call[0] != '<')       /* a hashed call is not reportable */
                    spot_emit_cg(MODE_NAME, r->call, r->grid, (double)tone, snr);
                /* the loader's DECODE queue holds 8 lines and the UI task takes one per main-loop pass: a period's
                 * decodes come all at once here, so give the screen time for each line (no line is dropped) */
                icap_sleep_ms(40); pump();
            }
            if (nd > 0) log_flush();
            /* app window line 3: what the last period gave ("0918 WSPR: 3 decoded, 21 s") */
            q = hhmm(line, hh, mm); q = cat(q, " " MODE_NAME ": "); q = num(q, nd > 0 ? nd : 0, 0, ' '); q = cat(q, " decoded, ");
            q = num(q, secs, 0, ' '); cat(q, late ? " s (cut)" : " s");
            icap_set_line(3, line);
        }
        if (stop()) break;                           /* else: armed -> the beacon session at the loop top */
    }
    if (cap.fd >= 0) icap_close(cap.fd);
    return 0;
}
