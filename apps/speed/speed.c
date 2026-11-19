/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Dawid SQ6EMM
 *
 * SPEED - ICAP v1rc memory speed test: the same kernels on a buffer in the app window (cacheable),
 * in the ABI >= 2 data region (svc->heap_base; Normal non-cacheable on the IC-9700 v1rc map) and on
 * the app stack.  Built only against the public SDK header icap.h (no firmware addresses).
 *
 * Kernels (4 KiB working set in every location, identical code - only the base pointer differs):
 *   set  word memset of 4096 B                       -> MB/s
 *   cpy  word memcpy 2048 B (first half -> second)   -> MB/s
 *   axy  y[i] = y[i]*a + x[i], 512 floats            -> M elements/s
 *   fft  256-point complex radix-2 FFT + inverse     -> pairs/s
 *   fir  64-tap FIR, 448 outputs                     -> M taps/s
 *   big  word memcpy 8192 B (16 KiB buffer, window and region only) -> MB/s
 * Timing: svc->tick_ms (1 ms).  A run starts on a tick edge and repeats sub-batches of m calls (~0.1 ms,
 * calibrated per location) until 5 ms of ticks have passed; it is credited (ticks) + half a sub-batch
 * (end-phase correction, <= 0.5 ms), so the tick quantisation is < ~1 %.  Each run is followed by a yield;
 * the three locations are interleaved run by run so background load hits them alike.  Each location
 * runs until >= 300 ms are accumulated.
 * Output: 8-line panel table + SPEED.TXT (app folder).
 */
#include "icap.h"

typedef uint32_t u32; typedef uint8_t u8;

#define NLOC      3
#define WS        4096u           /* working set per kernel (bytes) - stack array is this size  */
#define BIG       16384u          /* "big" buffer in the window (and region)                    */
#define TARGET_MS 300.0
#define WALL_MS   6000u           /* per kernel, all locations together                         */
#define BATCH_LO  3u              /* calibrate n until one batch takes >= 3 ms (measured)       */
#define RUN_MS    5u              /* one timed run                                              */
#define SUB_MS    0.1             /* target sub-batch length between tick reads                 */
#define NMAX      65536u
#define MAXBATCH  2000u

#define BARRIER(p) __asm__ volatile("" : : "r"(p) : "memory")

static u32 g_win[BIG / 4] __attribute__((aligned(64)));      /* app window (.bss)          */
static float g_tw[256] __attribute__((aligned(64)));          /* 128 complex twiddles (src) */
static char g_txt[3072]; static u32 g_tlen;
static char g_ln[64];
static int g_stop, g_notick;

static const char *const LOCN[NLOC] = { "win", "reg", "stk" };

/* ---- formatting --------------------------------------------------------- */
static char *s_cat(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char *s_u(char *p, u32 v) { char t[11]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) *p++ = t[--n]; *p = 0; return p; }
static char *s_hex(char *p, u32 v) { p = s_cat(p, "0x"); for (int i = 28; i >= 0; i -= 4) *p++ = "0123456789ABCDEF"[(v >> i) & 15]; *p = 0; return p; }
/* fixed-point with d decimals (v >= 0) */
static char *s_f(char *p, double v, int d)
{
    if (v < 0) v = 0;
    u32 m = 1; for (int i = 0; i < d; i++) m *= 10;
    if (v > 4.0e9 / m) v = 4.0e9 / m;
    u32 x = (u32)(v * m + 0.5);
    p = s_u(p, x / m);
    if (d) { *p++ = '.'; u32 f = x % m; for (u32 q = m / 10; q; q /= 10) { *p++ = (char)('0' + f / q); f %= q; } *p = 0; }
    return p;
}
/* right-aligned into w chars: one decimal below 100, else integer */
static char *s_col(char *p, double v, int w, int ok)
{
    char t[16], *e;
    if (!ok) e = s_cat(t, "-");
    else if (v < 9.995) e = s_f(t, v, 2);
    else if (v < 99.95) e = s_f(t, v, 1);
    else e = s_f(t, v, 0);
    int n = (int)(e - t);
    for (int i = n; i < w; i++) *p++ = ' ';
    for (int i = 0; i < n; i++) *p++ = t[i];
    *p = 0; return p;
}
static u32 z_len(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
static void txt(const char *s) { while (*s && g_tlen < sizeof g_txt - 2) g_txt[g_tlen++] = *s++; }
static void txtl(const char *s) { txt(s); txt("\r\n"); }

/* ---- kernels (noinline, base pointer only) ------------------------------ */
typedef void (*kfn)(u8 *b);

__attribute__((noinline)) static void k_set(u8 *b)
{
    u32 *w = (u32 *)b; static u32 v; v++;
    for (u32 i = 0; i < WS / 4; i++) w[i] = v;
    BARRIER(b);
}
__attribute__((noinline)) static void cpy_words(u32 *d, const u32 *s, u32 nw)
{
    for (u32 i = 0; i < nw; i++) d[i] = s[i];
    BARRIER(d);
}
__attribute__((noinline)) static void k_cpy(u8 *b) { cpy_words((u32 *)(b + WS / 2), (const u32 *)b, WS / 8); }
__attribute__((noinline)) static void k_big(u8 *b) { cpy_words((u32 *)(b + BIG / 2), (const u32 *)b, BIG / 8); }

#define AXN 512u
__attribute__((noinline)) static void k_axy(u8 *b)
{
    const float *x = (const float *)b; float *y = (float *)b + AXN; const float a = 0.999f;
    for (u32 i = 0; i < AXN; i++) y[i] = y[i] * a + x[i];
    BARRIER(b);
}

/* 256-point in-place radix-2 DIT; data 256 complex (2 KiB) then 128 twiddles (1 KiB) in the buffer */
#define FN 256u
static void fft(float *d, const float *tw, float sg)
{
    for (u32 i = 1, j = 0; i < FN; i++) {               /* bit reversal */
        u32 bit = FN >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = d[2*i]; d[2*i] = d[2*j]; d[2*j] = t; t = d[2*i+1]; d[2*i+1] = d[2*j+1]; d[2*j+1] = t; }
    }
    for (u32 len = 2; len <= FN; len <<= 1) {
        u32 half = len >> 1, step = FN / len;
        for (u32 i = 0; i < FN; i += len)
            for (u32 k = 0; k < half; k++) {
                float wr = tw[2*k*step], wi = sg * tw[2*k*step+1];
                float *a = d + 2*(i+k), *c = d + 2*(i+k+half);
                float xr = c[0]*wr - c[1]*wi, xi = c[0]*wi + c[1]*wr;
                c[0] = a[0] - xr; c[1] = a[1] - xi; a[0] += xr; a[1] += xi;
            }
    }
}
__attribute__((noinline)) static void k_fft(u8 *b)
{
    float *d = (float *)b, *tw = (float *)b + 2 * FN;
    fft(d, tw, 1.0f);
    fft(d, tw, -1.0f);
    const float s = 1.0f / FN;
    for (u32 i = 0; i < 2 * FN; i++) d[i] *= s;
    BARRIER(b);
}

#define FX 512u
#define FT 64u
__attribute__((noinline)) static void k_fir(u8 *b)
{
    const float *x = (const float *)b, *h = x + FX; float *o = (float *)b + FX + FT;
    for (u32 i = 0; i < FX - FT; i++) {
        float acc = 0.0f;
        for (u32 k = 0; k < FT; k++) acc += h[k] * x[i + k];
        o[i] = acc;
    }
    BARRIER(b);
}

/* ---- per-kernel buffer init (same contents in every location) ----------- */
static void fill_float(float *f, u32 n, u32 seed)
{
    for (u32 i = 0; i < n; i++) { seed = seed * 1664525u + 1013904223u; f[i] = (float)((int)(seed >> 9) - (1 << 22)) * (1.0f / (1 << 22)); }
}
static void init_buf(int k, u8 *b)
{
    for (u32 i = 0; i < (k == 5 ? BIG : WS); i++) b[i] = (u8)(i * 7u + 3u);
    if (k == 2) { fill_float((float *)b, 2 * AXN, 1); }
    if (k == 3) { fill_float((float *)b, 2 * FN, 2); float *tw = (float *)b + 2 * FN; for (u32 i = 0; i < FN; i++) tw[i] = g_tw[i]; }
    if (k == 4) { fill_float((float *)b, FX + FT, 3); }
}
static void make_twiddles(void)
{
    double th = 6.283185307179586 / FN, t2 = th * th;   /* cos/sin(2pi/256) by Taylor, then rotate */
    double c = 1 - t2/2 + t2*t2/24 - t2*t2*t2/720 + t2*t2*t2*t2/40320;
    double s = th * (1 - t2/6 + t2*t2/120 - t2*t2*t2/5040 + t2*t2*t2*t2/362880);
    double wr = 1, wi = 0;
    for (u32 k = 0; k < FN / 2; k++) {
        g_tw[2*k] = (float)wr; g_tw[2*k+1] = (float)-wi;    /* exp(-i 2pi k/N) */
        double nr = wr * c - wi * s; wi = wr * s + wi * c; wr = nr;
    }
}

/* ---- timing -------------------------------------------------------------- */
/* wait for the next tick edge; returns the new tick (or the current one if the tick never moves) */
static u32 tick_edge(void)
{
    u32 t0 = icap_ticks(), t;
    for (u32 g = 0; g < 4000000u; g++) if ((t = icap_ticks()) != t0) return t;
    g_notick = 1; return t0;
}
/* one batch: n kernel calls, starting on a tick edge; returns credited ms (0.5 ms resolution) */
static double batch(kfn f, u8 *b, u32 n)
{
    u32 t0 = tick_edge();
    for (u32 i = 0; i < n; i++) f(b);
    u32 t1 = icap_ticks();
    return (double)(t1 - t0) + 0.5;
}

/* one timed run: from a tick edge, sub-batches of m calls until RUN_MS ticks passed */
static double timed_run(kfn f, u8 *b, u32 m, double sub_ms, double *calls)
{
    u32 t0 = tick_edge(), t = 0, c = 0;
    for (u32 g = 0; g < 2000000u && !g_notick; g++) {
        for (u32 i = 0; i < m; i++) f(b);
        c += m;
        if ((t = icap_ticks() - t0) >= RUN_MS) break;
    }
    *calls = c;
    return (double)t + (sub_ms < 1.0 ? sub_ms * 0.5 : 0.5);
}

struct kdef { const char *tag, *unit; kfn f; double units; double scale; int nstk; };
/* units per kernel call; rate shown = units / ms / scale */
static const struct kdef K[] = {
    { "set", "MB/s",   k_set, WS,          1000.0, 1 },
    { "cpy", "MB/s",   k_cpy, WS / 2,      1000.0, 1 },
    { "axy", "Mel/s",  k_axy, AXN,         1000.0, 1 },
    { "fft", "pair/s", k_fft, 1,           0.001,  1 },
    { "fir", "Mtap/s", k_fir, (FX-FT)*FT,  1000.0, 1 },
    { "big", "MB/s",   k_big, BIG / 2,     1000.0, 0 },
};
#define NK ((int)(sizeof K / sizeof K[0]))

struct res { double ms, units, rate, sub; u32 n, nb; int ok; };
static struct res R[NK][NLOC];

static int should_stop(void) { if (!g_stop && icap_should_stop()) g_stop = 1; return g_stop; }

static void run_kernel(int k, u8 *loc[NLOC])
{
    const struct kdef *d = &K[k];
    int act[NLOC];
    for (int l = 0; l < NLOC; l++) {
        struct res *r = &R[k][l];
        r->ms = r->units = r->rate = r->sub = 0; r->n = 1; r->nb = 0; r->ok = 0;
        act[l] = loc[l] != 0 && (l != 2 || d->nstk);
        if (!act[l]) continue;
        init_buf(k, loc[l]);
        if (g_notick) continue;
        /* calibrate: double n until one batch measures >= BATCH_LO ms (also warms the cache) */
        while (r->n < NMAX && !should_stop()) {
            double ms = batch(d->f, loc[l], r->n);
            icap_yield();
            if (g_notick || ms >= BATCH_LO + 0.5) {       /* -> sub-batch of ~SUB_MS */
                double per = ms / r->n;
                r->n = per >= SUB_MS ? 1u : (u32)(SUB_MS / per);
                r->sub = per * r->n;
                break;
            }
            r->n <<= 1;
        }
        if (r->sub <= 0) r->sub = 1.0;                    /* never reached BATCH_LO: n = NMAX */
    }
    u32 w0 = icap_ticks();
    for (;;) {
        int more = 0;
        for (int l = 0; l < NLOC; l++) {
            struct res *r = &R[k][l];
            if (!act[l] || should_stop()) continue;
            if (g_notick) { if (r->nb == 0) { d->f(loc[l]); r->nb = 1; r->units = d->units; } continue; }
            if (r->ms >= TARGET_MS || r->nb >= MAXBATCH) continue;
            double calls;
            r->ms += timed_run(d->f, loc[l], r->n, r->sub, &calls); r->nb++;
            r->units += d->units * calls;
            icap_yield();
            more = 1;
        }
        if (!more || should_stop() || icap_ticks() - w0 > WALL_MS) break;
    }
    for (int l = 0; l < NLOC; l++) {
        struct res *r = &R[k][l];
        if (act[l] && r->ms > 0) { r->rate = r->units / r->ms / d->scale; r->ok = 1; }
    }
}

/* the stack location lives only while the kernels run (file and panel calls happen outside) */
__attribute__((noinline)) static void run_all(u8 *win, u8 *reg)
{
    u8 stk[WS] __attribute__((aligned(64)));
    u8 *loc[NLOC] = { win, reg, stk };
    for (int k = 0; k < NK && !should_stop(); k++) {
        char *p = s_cat(g_ln, "running "); p = s_cat(p, K[k].tag); s_cat(p, " ...");
        icap_set_line(7, g_ln);
        run_kernel(k, loc);
    }
    BARRIER(stk);
}

static void ratio_col(char *p, int k, int l)
{
    struct res *a = &R[k][l], *w = &R[k][0];
    s_col(p, a->ok && w->ok && w->rate > 0 ? a->rate / w->rate : 0, 5, a->ok && w->ok && w->rate > 0);
}

int app_main(const struct icap_svc *svc)
{
    icap_use(svc);
    icap_clear();
    char *p;
    int rid9700 = icap_is_ic9700();
    p = s_cat(g_ln, "SPEED "); p = s_cat(p, rid9700 ? "IC-9700" : icap_is_ic705() ? "IC-705" : "?");
    char *title_end = p;
    s_cat(p, " win/reg/stk"); icap_set_line(0, g_ln);

    u8 *win = (u8 *)g_win, *reg = 0;
    u32 hb = 0, hs = 0;
    if (svc->abi_version >= 2 && svc->size >= 0x50 && svc->heap_size) {
        hb = svc->heap_base; hs = svc->heap_size;
        u32 a = (hb + 63u) & ~63u;
        if (a + BIG <= hb + hs) reg = (u8 *)(uintptr_t)a;
    }
    if (!reg) { s_cat(title_end, " no data region"); icap_set_line(0, g_ln); icap_print("no data region"); }
    make_twiddles();

    u32 t0 = icap_ticks();
    run_all(win, reg);
    u32 tt = icap_ticks() - t0;
    if (g_stop) return 0;

    /* panel: 8 lines x 31 chars */
    icap_set_line(1, "      win   reg   stk  r/w  s/w");
    for (int k = 0; k < NK; k++) {
        p = s_cat(g_ln, K[k].tag);
        for (int l = 0; l < NLOC; l++) p = s_col(p, R[k][l].rate, 6, R[k][l].ok);
        ratio_col(p, k, 1); p += z_len(p);
        ratio_col(p, k, 2);
        icap_set_line(2 + k, g_ln);
    }
    if (g_notick) icap_set_line(0, "SPEED: tick frozen, no timing");

    /* SPEED.TXT */
    g_tlen = 0;
    p = s_cat(g_ln, "SPEED v1  radio "); p = s_hex(p, svc->radio_id); p = s_cat(p, "  fw "); s_hex(p, svc->fw_build); txtl(g_ln);
    p = s_cat(g_ln, "win buf "); p = s_hex(p, (u32)(uintptr_t)win); p = s_cat(p, "  region "); p = s_hex(p, hb);
    p = s_cat(p, " size "); p = s_u(p, hs); p = s_cat(p, " buf "); s_hex(p, (u32)(uintptr_t)reg); txtl(g_ln);
    struct icap_meminfo mi;
    if (icap_mem_info(&mi) == 0) {
        p = s_cat(g_ln, "window "); p = s_hex(p, mi.app_base); p = s_cat(p, ".."); p = s_hex(p, mi.app_limit);
        p = s_cat(p, "  stack "); p = s_hex(p, mi.stack_lo); p = s_cat(p, ".."); s_hex(p, mi.stack_hi); txtl(g_ln);
    }
    p = s_cat(g_ln, "total "); p = s_u(p, tt); s_cat(p, g_notick ? " ms (tick frozen: no timing)" : " ms"); txtl(g_ln);
    txtl("working set 4096 B per kernel (big: 16384 B, no stack); run = n-call sub-batches for 5 ms from a tick edge");
    txtl("kern loc      n     runs       ms         rate unit    x win");
    for (int k = 0; k < NK; k++)
        for (int l = 0; l < NLOC; l++) {
            struct res *r = &R[k][l];
            p = s_cat(g_ln, K[k].tag); p = s_cat(p, "  "); p = s_cat(p, LOCN[l]);
            if (!r->ok) { s_cat(p, "  -"); txtl(g_ln); continue; }
            char t[24]; char *e;
            #define PADN(str_end_expr, w) do { e = (str_end_expr); int n_ = (int)(e - t); for (int i_ = n_; i_ < (w); i_++) *p++ = ' '; for (int i_ = 0; i_ < n_; i_++) *p++ = t[i_]; *p = 0; } while (0)
            PADN(s_u(t, r->n), 7);
            PADN(s_u(t, r->nb), 9);
            PADN(s_f(t, r->ms, 1), 9);
            PADN(s_f(t, r->rate, 2), 13);
            p = s_cat(p, " "); p = s_cat(p, K[k].unit);
            for (int i = (int)z_len(K[k].unit); i < 7; i++) *p++ = ' ';
            PADN(s_f(t, R[k][0].ok && R[k][0].rate > 0 ? r->rate / R[k][0].rate : 0, 3), 7);
            txtl(g_ln);
        }
    int fd = icap_open("SPEED.TXT", ICAP_O_WRITE);
    if (fd >= 0) { icap_write(fd, g_txt, g_tlen); icap_close(fd); }
    else icap_set_line(0, "SPEED: SPEED.TXT open failed");
    return 0;
}
