/* SPDX-License-Identifier: MIT
 * ft8dec.c - FT8 decoder for the IC-705 ICAP digi app (see ft8dec.h).
 *
 * Per call (one 15 s period, audio streamed from the SD capture through dsrc_t):
 *   1. Waterfall like ft8_lib's monitor.c (Hann over 2 symbols, 2x time and 2x frequency oversampling, uint8
 *      magnitudes in 0.5 dB steps) - in the caller's work area, only for the requested band, with an own
 *      mixed-radix FFT (2^a x odd, 2560 = 512 x 5 at 8 kHz) driven by one quarter-wave table; a band that does
 *      not fit is done in several passes over the file.
 *   2. Costas sync search (ft8_lib's ftx_find_candidates, copied with an abort poll) -> per candidate: hard-sync
 *      check, LLRs from the waterfall, BP (ft8_lib's bp_decode + WSJT-X early stop), OSD (order 1 + pairs of the
 *      60 least reliable info bits), CRC-14; if that fails, once more with the tone powers interpolated towards
 *      the true time / frequency (parabola on the Costas tones).
 *   3. Like WSJT-X: decoded signals are subtracted from the audio and the search is repeated (3 passes).  The
 *      subtraction streams the file again: each signal's GFSK reference (BT 2) is correlated symbol by symbol,
 *      the complex amplitude is interpolated between symbol centres and the signal is removed; the residual
 *      feeds the new waterfall directly (no extra file, 1.5 symbols of lag).  After a subtraction only the
 *      bins next to the newly removed signals are searched.
 *   4. DT / frequency from the candidate grid refined by parabolic interpolation (time calibrated); SNR in
 *      2500 Hz from the decoded tone power over a WSJT-X-style noise baseline (low percentile of the period's
 *      average spectrum).
 * Duplicates (same 77-bit payload) are dropped within the call.  Callsign hashes are kept in the caller's
 * ft8_state_t across periods.  should_abort is polled every few ms of A9 CPU (max ~0.6 ms apart on x86).
 * All buffers are in `work` (peak ~232 KB for 200..3000 Hz and a 14.6 s capture at 8 kHz; ft8_work_peak()),
 * except a few KB of stack (BP / OSD / LLR arrays).  ARM and x86 builds decode bit-identically.
 */
#include "ft8dec.h"
#include "ft8/decode.h"
#include "ft8/encode.h"
#include "ft8/message.h"
#include "ft8/constants.h"
#include "ft8/crc.h"

#define TOSR        2          /* time subdivisions per symbol */
#define FOSR        2          /* frequency subdivisions per 6.25 Hz */
#define MAX_SIG     64         /* distinct messages per period */
#define MAX_BLOCKS  93         /* 15 s / 0.16 s */
#define NSYM        FT8_NN     /* 79 */

/* decoder knobs (fixed in the app; the host test driver may override them: test/ft8_host.c, FT8T=...) */
typedef struct { int llr, max_cand, min_score, ldpc_iters, osd, osd_nt, nhmax, passes, nsync_min; float scale, dmax, snr_off; int pct, skip, osd_min, near, interp; } ft8_tune_t;
ft8_tune_t ft8_tune = { 2, 300, 8, 25, 2, 60, 36, 3, 7, 4.9f, 1e9f, -26.0f, 10, 2, 0, 1, 1 };

#ifdef FT8_DEBUG
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
#define DBG(...) ((void)0)
#endif

static uint32_t peak;
uint32_t ft8_work_peak(void) { return peak; }

/* ---- callsign hash table (ftx_callsign_hash_interface_t has no context pointer: the table of the current call) */
static ft8_state_t *cur_st;
static int hcpy(char *d, const char *s, int n) { int i = 0; for (; i < n - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; return i; }
static int heq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static bool h_lookup(ftx_callsign_hash_type_t type, uint32_t hash, char *call)
{
    int sh = type == FTX_CALLSIGN_HASH_10_BITS ? 12 : type == FTX_CALLSIGN_HASH_12_BITS ? 10 : 0, best = -1;
    call[0] = 0;
    if (!cur_st) return false;
    for (int i = 0; i < FT8_HASH_SLOTS; i++)                 /* the most recently heard call wins */
        if (cur_st->e[i].used && (cur_st->e[i].h22 >> sh) == hash && (best < 0 || cur_st->e[i].age < cur_st->e[best].age)) best = i;
    if (best < 0) return false;
    hcpy(call, cur_st->e[best].call, 12);
    return true;
}
static void h_save(const char *call, uint32_t h22)
{
    int fr = -1, old = -1;
    if (!cur_st) return;
    for (int i = 0; i < FT8_HASH_SLOTS; i++) {
        if (!cur_st->e[i].used) { if (fr < 0) fr = i; continue; }
        if (cur_st->e[i].h22 == h22 && heq(cur_st->e[i].call, call)) { cur_st->e[i].age = 0; return; }
        if (old < 0 || cur_st->e[i].age > cur_st->e[old].age) old = i;
    }
    if (fr < 0) fr = old; else cur_st->n++;                  /* full: replace the oldest */
    hcpy(cur_st->e[fr].call, call, 12);
    cur_st->e[fr].h22 = h22 & 0x3FFFFFu; cur_st->e[fr].age = 0; cur_st->e[fr].used = 1;
}
static ftx_callsign_hash_interface_t hash_if = { h_lookup, h_save };

/* ---- helpers */
static uintptr_t al8(uintptr_t p) { return (p + 7u) & ~(uintptr_t)7u; }
static void zero(void *p, uint32_t n) { uint8_t *q = p; while (n--) *q++ = 0; }
static float mx(float a, float b) { return a > b ? a : b; }

/* GFSK (BT = 2) frequency step: S(d) = (1 + erf(pi*sqrt(2/ln2)*2*d)) / 2, d = j/128 symbol from the boundary */
static const float gstep[33] = {
    0.5000000f, 0.5469344f, 0.5932215f, 0.6382405f, 0.6814223f, 0.7222704f, 0.7603782f, 0.7954389f, 0.8272514f,
    0.8557185f, 0.8808406f, 0.9027049f, 0.9214715f, 0.9373570f, 0.9506183f, 0.9615363f, 0.9704009f, 0.9774992f,
    0.9831046f, 0.9874701f, 0.9908230f, 0.9933627f, 0.9952600f, 0.9966577f, 0.9976732f, 0.9984008f, 0.9989150f,
    0.9992733f, 0.9995196f, 0.9996865f, 0.9997981f, 0.9998717f, 0.9999195f };

typedef struct {                 /* a decoded signal (kept for the subtraction passes) */
    float f0;                    /* tone 0, Hz */
    int32_t t0;                  /* first sample of symbol 0 (src index) */
    uint8_t tones[NSYM];
    uint8_t pad;
} sig_t;

typedef struct {                 /* per signal while streaming */
    uint32_t phc, phs;           /* reference phase at the correlation / subtraction sample */
    float cr[4], ci[4];          /* symbol correlations, ring by symbol index & 3 */
    float pr, pi;                /* sum c[k] conj(c[k-1]) -> frequency correction */
    int started_c, started_s;    /* reference phase initialised (a signal may start before the stream) */
} sst_t;
#define STAB 512                 /* reference oscillator sine table (linear interpolation: error < 2e-5) */

typedef struct {
    const dsrc_t *src;
    int (*ab)(void);
    int aborted;
    int fs, block, sub, nfft, N, P, m, nblk;
    int32_t s0;                  /* sample index of the waterfall origin (may be < 0: zeros) */
    float nom, df;
    const float *qs;             /* quarter-wave sine table of nfft points: qs[j] = sin(2 pi j / nfft), j <= nfft/4 */
    float lin[256], amp[256];    /* uint8 waterfall value -> linear power / amplitude */
    float sn[STAB + STAB / 4 + 1]; /* sin(2 pi i / STAB), with a quarter extra for cos */
    uint8_t gidx[1920];          /* sample within symbol -> 1/128 symbol units (block <= 1920) */
    uint32_t gc[FTX_LDPC_N][3];  /* systematic generator by columns (OSD) */
    uint8_t mpos[FTX_LDPC_M][7]; /* BP: for check m, edge i: which of bit Nm[m][i]'s 3 checks it is */
    uint32_t (*w)[6];            /* OSD scratch [FTX_LDPC_K][6] (in the scratch area, after the candidates) */
#ifdef FT8_ARENA_SCRATCH
    struct ft8_scratch *sc;      /* bp/osd/bp_osd working arrays, in the app arena (not .bss) - see FT8_ARENA_SCRATCH */
#endif
    ftx_message_t dec[MAX_SIG];  /* payloads already decoded */
    sig_t sig[MAX_SIG];
    int nsig;
    const float *base;           /* mean noise power per bin, current band pass */
    ft8_result_t *out; int maxout, nout;
} ctx_t;

static int poll(ctx_t *c)
{
    if (!c->aborted && c->ab && c->ab()) c->aborted = 1;
    return c->aborted;
}

/* cos/sin of 2 pi j / nfft, 0 <= j < nfft */
static void tsc(const ctx_t *c, int j, float *co, float *si)
{
    int q = c->nfft >> 2;
    if (j < q)          { *si =  c->qs[j];             *co =  c->qs[q - j]; }
    else if (j < 2 * q) { *si =  c->qs[2 * q - j];     *co = -c->qs[j - q]; }
    else if (j < 3 * q) { *si = -c->qs[j - 2 * q];     *co = -c->qs[3 * q - j]; }
    else                { *si = -c->qs[4 * q - j];     *co =  c->qs[j - 3 * q]; }
}
/* cos/sin of a 32-bit phase (2^32 = 2 pi): STAB-entry table, linear interpolation */
static inline void psc(const ctx_t *c, uint32_t ph, float *co, float *si)
{
    uint32_t j = ph >> 23;
    float fr = (float)(ph & 0x7FFFFFu) * (1.0f / 8388608.0f);
    const float *t = c->sn + j;
    *si = t[0] + (t[1] - t[0]) * fr;
    *co = t[STAB / 4] + (t[STAB / 4 + 1] - t[STAB / 4]) * fr;
}

/* ---- FFT: X[k] = sum x[n] e^{-2 pi i k n / nfft} of nfft real samples, via an N = nfft/2 point complex FFT,
 * N = P * m (P = 2^a, m odd): m radix-2 FFTs of P points + an m-point DFT.  z, y: N complex each. */
static void cfft_p(const ctx_t *c, float *s, int P)          /* in place, radix 2 */
{
    for (int i = 1, j = 0; i < P; i++) {
        int bit = P >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float tr = s[2 * i], ti = s[2 * i + 1]; s[2 * i] = s[2 * j]; s[2 * i + 1] = s[2 * j + 1]; s[2 * j] = tr; s[2 * j + 1] = ti; }
    }
    for (int len = 2; len <= P; len <<= 1) {
        int h = len >> 1, st = c->nfft / len;
        for (int k = 0; k < h; k++) {
            float wr, wi;
            tsc(c, k * st, &wr, &wi); wi = -wi;              /* e^{-2 pi i k / len} */
            for (int i = k; i < P; i += len) {
                float *a = s + 2 * i, *b = s + 2 * (i + h);
                float tr = b[0] * wr - b[1] * wi, ti = b[0] * wi + b[1] * wr;
                b[0] = a[0] - tr; b[1] = a[1] - ti; a[0] += tr; a[1] += ti;
            }
        }
    }
}
static void rfft(const ctx_t *c, float *z, float *y)          /* z: windowed input as N complex -> Z in z */
{
    int N = c->N, P = c->P, m = c->m;
    for (int r = 0; r < m; r++)
        for (int q = 0; q < P; q++) { y[2 * (r * P + q)] = z[2 * (q * m + r)]; y[2 * (r * P + q) + 1] = z[2 * (q * m + r) + 1]; }
    for (int r = 0; r < m; r++) cfft_p(c, y + 2 * r * P, P);
    if (m == 1) { for (int i = 0; i < 2 * N; i++) z[i] = y[i]; return; }
    float wr[16], wi[16], dr[16], di[16];                   /* W^(r k) by rotation, re-synced from the table */
    for (int r = 1; r < m; r++) { tsc(c, 2 * r, &dr[r], &di[r]); di[r] = -di[r]; }
    for (int k = 0; k < N; k++) {
        int kq = k & (P - 1);
        if (!(k & 63))
            for (int r = 1; r < m; r++) { tsc(c, (int)((2u * (uint32_t)r * (uint32_t)k) % (uint32_t)c->nfft), &wr[r], &wi[r]); wi[r] = -wi[r]; }
        float sr = y[2 * kq], si = y[2 * kq + 1];
        for (int r = 1; r < m; r++) {
            const float *v = y + 2 * (r * P + kq);
            sr += v[0] * wr[r] - v[1] * wi[r]; si += v[0] * wi[r] + v[1] * wr[r];
            float t = wr[r] * dr[r] - wi[r] * di[r]; wi[r] = wr[r] * di[r] + wi[r] * dr[r]; wr[r] = t;
        }
        z[2 * k] = sr; z[2 * k + 1] = si;
    }
}
static float rpow(const ctx_t *c, const float *Z, int k)      /* |X[k]|^2 from the packed complex FFT, 0 < k < N */
{
    int N = c->N, k2 = N - k;
    float ar = Z[2 * k], ai = Z[2 * k + 1], br = Z[2 * k2], bi = -Z[2 * k2 + 1];
    float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi);
    float dr = 0.5f * (ar - br), di = 0.5f * (ai - bi);       /* O = (Zk - conj Z(N-k)) / 2i = (di, -dr) */
    float orr = di, oi = -dr, wr, wi;
    tsc(c, k, &wr, &wi); wi = -wi;
    float xr = er + orr * wr - oi * wi, xi = ei + orr * wi + oi * wr;
    return xr * xr + xi * xi;
}

/* ---- streaming: read the audio, subtract the known signals (lag), build the waterfall of the residual */
#define GZ 24                    /* GFSK transition zone, 1/128 symbol each side (1 - gstep[24] = 0.002) */
/* phase increment (2^32 / fs units) for symbol k at 1/128-symbol position d */
static uint32_t ph_inc(const ctx_t *c, const sig_t *s, int k, int d, float fscale)
{
    float t = (float)s->tones[k];
    if (d < GZ) { int p = k > 0 ? s->tones[k - 1] : s->tones[0]; t += ((float)p - t) * (1.0f - gstep[d]); }
    else if (d >= 128 - GZ) { int nx = k < NSYM - 1 ? s->tones[k + 1] : s->tones[NSYM - 1]; t += ((float)nx - t) * (1.0f - gstep[128 - d]); }
    return (uint32_t)(int32_t)((s->f0 + c->df * t) * fscale);
}

/* reference phase at sample u of the signal (phase 0 at u = 0) */
static uint32_t ph_at(const ctx_t *c, const sig_t *s, int32_t u, float fscale)
{
    uint32_t ph = 0;
    for (int32_t v = 0; v < u && v < NSYM * c->block; v++) {
        int ks = v / c->block, d = c->gidx[v - ks * c->block];
        ph += ph_inc(c, s, ks, d, fscale);
    }
    return ph;
}

static uint32_t stream_bytes(const ctx_t *c, int nsub)
{
    return (uint32_t)(sizeof(float) * (uint32_t)(3 * c->nfft + 4 * c->sub) + sizeof(sst_t) * (uint32_t)(nsub + 1) + 48);
}

/* correlate (input chunk) or subtract (output chunk, lagged) one signal over samples [a, a+n) of the stream;
 * x: the chunk's samples (ring slots for subtraction: x[j] is at ring index (ri0 + j) mod RL) */
static void sig_corr(const ctx_t *c, const sig_t *s, sst_t *t, int32_t a, const float *x, int n, float fscale)
{
    const int block = c->block;
    int32_t u = a - s->t0;
    int j = 0;
    if (u < 0) { j = -u; u = 0; }
    if (u >= NSYM * block || j >= n) return;
    int ks = u / block, wi = u - ks * block;
    if (!t->started_c) { t->started_c = 1; t->phc = ph_at(c, s, u, fscale); }
    uint32_t flat = ph_inc(c, s, ks, 64, fscale), ph = t->phc;
    float co = 0, si = 0, fc = 1, fsn = 0;
    int rot = 0;                                             /* in the flat part: rotate instead of table lookups */
    psc(c, flat, &fc, &fsn);
    for (; j < n; j++) {
        if (wi == 0) {
            int q = ks & 3; t->cr[q] = t->ci[q] = 0.0f;
            flat = ph_inc(c, s, ks, 64, fscale); psc(c, flat, &fc, &fsn);
        }
        int d = c->gidx[wi], tz = d < GZ || d >= 128 - GZ;
        if (tz || !rot) { psc(c, ph, &co, &si); rot = !tz; }
        else { float t2 = co * fc - si * fsn; si = si * fc + co * fsn; co = t2; }
        t->cr[ks & 3] += x[j] * co; t->ci[ks & 3] -= x[j] * si;
        ph += tz ? ph_inc(c, s, ks, d, fscale) : flat;
        if (++wi == block) {
            if (ks > 0) {                                    /* symbol ks complete: phase step vs ks-1 */
                int qa = ks & 3, qb = (ks - 1) & 3;
                t->pr += t->cr[qa] * t->cr[qb] + t->ci[qa] * t->ci[qb];
                t->pi += t->ci[qa] * t->cr[qb] - t->cr[qa] * t->ci[qb];
            }
            wi = 0;
            if (++ks == NSYM) break;
        }
    }
    t->phc = ph;
}
static void sig_sub(const ctx_t *c, const sig_t *s, sst_t *t, int32_t a, float *ring, int ri0, int RL, int n, float fscale)
{
    const int block = c->block, half = block / 2;
    const float ab = 2.0f / (float)block, inv = 1.0f / (float)block;
    int32_t u = a - s->t0;
    int j = 0;
    if (u < 0) { j = -u; u = 0; }
    if (u >= NSYM * block || j >= n) return;
    int ks = u / block, wi = u - ks * block, ri = ri0 + j;
    if (ri >= RL) ri -= RL;
    if (!t->started_s) { t->started_s = 1; t->phs = ph_at(c, s, u, fscale); }
    uint32_t flat = ph_inc(c, s, ks, 64, fscale), ph = t->phs;
    float co = 0, si = 0, fc = 1, fsn = 0;
    int rot = 0;
    psc(c, flat, &fc, &fsn);
    float ar = 0, ai = 0, dar = 0, dai = 0;
    int seg = 1;                                             /* (re)start the amplitude ramp */
    for (; j < n; j++) {
        if (wi == 0) { flat = ph_inc(c, s, ks, 64, fscale); psc(c, flat, &fc, &fsn); }
        if (seg || wi == 0 || wi == half) {                  /* amplitude: linear between symbol centres */
            int k0, k1; float w;
            if (wi < half) { k0 = ks > 0 ? ks - 1 : 0; k1 = ks; w = ks > 0 ? (float)(wi + half) * inv : 1.0f; }
            else { k0 = ks; k1 = ks < NSYM - 1 ? ks + 1 : ks; w = ks < NSYM - 1 ? (float)(wi - half) * inv : 0.0f; }
            float dr = t->cr[k1 & 3] - t->cr[k0 & 3], di = t->ci[k1 & 3] - t->ci[k0 & 3];
            ar = ab * (t->cr[k0 & 3] + dr * w); ai = ab * (t->ci[k0 & 3] + di * w);
            int edge = (wi < half && ks == 0) || (wi >= half && ks == NSYM - 1);
            dar = edge ? 0.0f : ab * dr * inv; dai = edge ? 0.0f : ab * di * inv;
            seg = 0;
        }
        int d = c->gidx[wi], tz = d < GZ || d >= 128 - GZ;
        if (tz || !rot) { psc(c, ph, &co, &si); rot = !tz; }
        else { float t2 = co * fc - si * fsn; si = si * fc + co * fsn; co = t2; }
        ring[ri] -= ar * co - ai * si;
        ar += dar; ai += dai;
        if (++ri == RL) ri = 0;
        ph += tz ? ph_inc(c, s, ks, d, fscale) : flat;
        if (++wi == block) { wi = 0; if (++ks == NSYM) break; }
    }
    t->phs = ph;
}

static int build_wf(ctx_t *c, int b_lo, int nb, uint8_t *mag, uint8_t *scr, sig_t *sg, int nsub)
{
    const int nfft = c->nfft, sub = c->sub, LAG = 3 * sub, RL = nfft + LAG;
    uintptr_t p = (uintptr_t)scr;
    float *z = (float *)p;          p = al8(p + sizeof(float) * (uint32_t)nfft);
    float *y = (float *)p;          p = al8(p + sizeof(float) * (uint32_t)nfft);
    float *ring = (float *)p;       p = al8(p + sizeof(float) * (uint32_t)RL);
    float *rd = (float *)p;         p = al8(p + sizeof(float) * (uint32_t)sub);
    sst_t *ss = (sst_t *)p;
    const float norm = 2.0f / (float)nfft, fscale = 4294967296.0f / (float)c->fs;
    zero(ring, sizeof(float) * (uint32_t)RL);
    zero(ss, sizeof(sst_t) * (uint32_t)(nsub + 1));
    int32_t n = c->s0;                                       /* next input sample */
    int32_t m = c->s0 - LAG;                                 /* next output (residual) sample */
    int rin = LAG, rout = 0;                                 /* their ring slots (the ring starts at stream index m) */
    int off = 0, steps = c->nblk * TOSR;
    for (int step = -LAG / sub; step < steps; step++) {
        /* input chunk [n, n+sub) -> ring + correlations */
        int k = 0;
        while (k < sub && n + k < 0) rd[k++] = 0.0f;
        if (k < sub && (uint32_t)(n + k) < c->src->len) {
            int r = c->src->read(c->src->ctx, (uint32_t)(n + k), rd + k, (uint32_t)(sub - k));
            if (r < 0) return FT8_ERR_IO;
            k += r;
        }
        while (k < sub) rd[k++] = 0.0f;
        for (int j = 0; j < sub; j++) { ring[rin] = rd[j]; if (++rin == RL) rin = 0; }
        for (int i = 0; i < nsub; i++) sig_corr(c, &sg[i], &ss[i], n, rd, sub, fscale);
        n += sub;
        /* output chunk [m, m+sub): subtract */
        for (int i = 0; i < nsub; i++) sig_sub(c, &sg[i], &ss[i], m, ring, rout, RL, sub, fscale);
        m += sub; rout += sub; if (rout >= RL) rout -= RL;
        if (step < 0) { if (poll(c)) return FT8_ERR_ABORT; continue; }
        /* STFT frame = residual [m - nfft, m) = ring slots from rout - nfft */
        int ri = rout - nfft; if (ri < 0) ri += RL;
        {   /* Hann sin^2(pi i/nfft) = (1 - cos(2 pi i/nfft))/2, cos from the quarter table: i, nfft/2 -/+ i, nfft - i */
            const int q = nfft / 4; const float h = 0.5f * norm;
            for (int i = 0; i < nfft; i++) {
                float co = i < q ? c->qs[q - i] : i < 2 * q ? -c->qs[i - q] : i < 3 * q ? -c->qs[3 * q - i] : c->qs[i - 3 * q];
                z[i] = ring[ri] * h * (1.0f - co);
                if (++ri == RL) ri = 0;
            }
        }
        rfft(c, z, y);
        for (int fsub = 0; fsub < FOSR; fsub++)
            for (int bin = b_lo; bin < b_lo + nb; bin++) {
                float m2 = rpow(c, z, bin * FOSR + fsub);
                float db = 10.0f * dg_log10f(1e-12f + m2);
                int sc = (int)(2.0f * db + 240.0f);
                mag[off++] = (uint8_t)(sc < 0 ? 0 : sc > 255 ? 255 : sc);
            }
        if (poll(c)) return FT8_ERR_ABORT;
    }
    for (int i = 0; i < nsub; i++) {                         /* frequency correction from the symbol phase steps */
        float pr = ss[i].pr, pi = ss[i].pi;
        if (pr > 0 && (pi < 0 ? -pi : pi) < pr) sg[i].f0 += (pi / pr) * (float)c->fs / (6.2831853f * (float)c->block);
    }
    return 0;
}

/* ---- candidate decoding: LLRs from the waterfall, BP (ft8_lib), then OSD; CRC (ft8_lib) */
#define NW 6
#ifdef FT8_ARENA_SCRATCH
/* bp/osd/bp_osd working arrays, moved out of .bss into the app arena (frees ~7.7 KiB of the app window so the
 * FT8 beacon encoder fits on the IC-9700).  One block, allocated once per decode in the fixed arena region; every
 * member is fully written before it is read (bp/osd/bp_osd are not reentrant and run only in the decoder task),
 * so the result is bit-identical to the .bss / stack scratch of the current decoder. */
struct ft8_scratch {
    float   tov[FTX_LDPC_N][3];     /* bp */
    float   toc[FTX_LDPC_M][7];     /* bp */
    float   sum[FTX_LDPC_N];        /* bp */
    float   r[FTX_LDPC_N];          /* osd */
    float   rp[NW * 32];            /* osd */
    float   save[FTX_LDPC_N];       /* bp_osd */
    uint8_t perm[FTX_LDPC_N];       /* osd */
    uint8_t piv[FTX_LDPC_K];        /* osd */
    uint8_t plain[FTX_LDPC_N];      /* bp_osd */
};
#endif
static void gen_init(ctx_t *c)                   /* systematic generator by columns: info bits that feed bit n */
{
    for (int n = 0; n < FTX_LDPC_N; n++) {
        c->gc[n][0] = c->gc[n][1] = c->gc[n][2] = 0;
        for (int j = 0; j < FTX_LDPC_K; j++)
            if (n < FTX_LDPC_K ? n == j : (kFTX_LDPC_generator[n - FTX_LDPC_K][j >> 3] & (0x80u >> (j & 7))) != 0)
                c->gc[n][j >> 5] |= 1u << (j & 31);
    }
}


/* BP (sum-product) as ft8_lib's bp_decode, plus WSJT-X's early stop (no progress in 5 iterations after 10) */
static float th(float x)                         /* ft8_lib fast_tanh */
{
    if (x < -4.97f) return -1.0f;
    if (x > 4.97f) return 1.0f;
    float x2 = x * x;
    return x * (945.0f + x2 * (105.0f + x2)) / (945.0f + x2 * (420.0f + x2 * 15.0f));
}
static float ath(float x)                        /* ft8_lib fast_atanh */
{
    float x2 = x * x;
    return x * (945.0f + x2 * (-735.0f + x2 * 64.0f)) / (945.0f + x2 * (-1050.0f + x2 * 225.0f));
}
static void bp_init(ctx_t *c)
{
    for (int m = 0; m < FTX_LDPC_M; m++)
        for (int i = 0; i < kFTX_LDPC_Num_rows[m]; i++) {
            int n = kFTX_LDPC_Nm[m][i] - 1;
            for (int j = 0; j < 3; j++) if (kFTX_LDPC_Mn[n][j] - 1 == m) c->mpos[m][i] = (uint8_t)j;
        }
}
/* IC-9700 build (-DFT8_STATIC_SCRATCH): its loader gives every app a fixed 8 KiB task stack; the BP message
 * arrays (~5 KiB) and the OSD / bp_osd scratch (~2.5 KiB) move to .bss so the deepest chain stays well inside it.  bp() / osd() / bp_osd() are not
 * reentrant (no recursion) and run only in the decoder task; every array is fully written before it is read.  The IC-705 build (16 KiB stack) is unchanged. */
#ifdef FT8_STATIC_SCRATCH
#define FT8_SCRATCH static
#else
#define FT8_SCRATCH
#endif
static int bp(const ctx_t *c, const float *llr, int max_iters, uint8_t *plain)
{
#ifdef FT8_ARENA_SCRATCH
    float (*tov)[3] = c->sc->tov; float (*toc)[7] = c->sc->toc; float *sum = c->sc->sum;
#else
    FT8_SCRATCH float tov[FTX_LDPC_N][3], toc[FTX_LDPC_M][7], sum[FTX_LDPC_N];
#endif
    int nclast = FTX_LDPC_M, ncnt = 0;
    for (int n = 0; n < FTX_LDPC_N; n++) tov[n][0] = tov[n][1] = tov[n][2] = 0;
    for (int iter = 0; iter < max_iters; iter++) {
        int any = 0;
        for (int n = 0; n < FTX_LDPC_N; n++) {
            sum[n] = llr[n] + tov[n][0] + tov[n][1] + tov[n][2];
            plain[n] = sum[n] > 0; any |= plain[n];
        }
        if (!any) return FTX_LDPC_M;             /* all-zero: not a valid message */
        int ncheck = 0;
        for (int m = 0; m < FTX_LDPC_M; m++) {
            int x = 0;
            for (int i = 0; i < kFTX_LDPC_Num_rows[m]; i++) x ^= plain[kFTX_LDPC_Nm[m][i] - 1];
            ncheck += x;
        }
        if (ncheck == 0) return 0;
        if (iter > 0) {
            if (ncheck < nclast) ncnt = 0; else ncnt++;
            if (ncnt >= 5 && iter >= 10 && ncheck > 15) return ncheck;
        }
        nclast = ncheck;
        for (int m = 0; m < FTX_LDPC_M; m++)
            for (int i = 0; i < kFTX_LDPC_Num_rows[m]; i++) {
                int n = kFTX_LDPC_Nm[m][i] - 1;
                toc[m][i] = th(-(sum[n] - tov[n][c->mpos[m][i]]) / 2);
            }
        for (int m = 0; m < FTX_LDPC_M; m++) {
            int nr = kFTX_LDPC_Num_rows[m], small = 0;
            float all = 1.0f;
            for (int k = 0; k < nr; k++) { all *= toc[m][k]; if (toc[m][k] > -1e-6f && toc[m][k] < 1e-6f) small = 1; }
            for (int i = 0; i < nr; i++) {
                float t;
                if (!small) t = all / toc[m][i];             /* product of the others */
                else { t = 1.0f; for (int k = 0; k < nr; k++) if (k != i) t *= toc[m][k]; }
                int n = kFTX_LDPC_Nm[m][i] - 1;
                tov[n][c->mpos[m][i]] = -2 * ath(t);
            }
        }
    }
    return nclast;
}

static int crc_ok(const uint8_t *bits174, ftx_message_t *msg)
{
    uint8_t a91[FTX_LDPC_K_BYTES];
    for (int i = 0; i < FTX_LDPC_K_BYTES; i++) a91[i] = 0;
    for (int i = 0; i < FTX_LDPC_K; i++) if (bits174[i]) a91[i >> 3] |= (uint8_t)(0x80u >> (i & 7));
    uint16_t ce = ftx_extract_crc(a91);
    a91[9] &= 0xF8; a91[10] = 0;
    if (ce != ftx_compute_crc(a91, 96 - 14)) return 0;
    int z = 1;
    for (int i = 0; i < 10; i++) { msg->payload[i] = a91[i]; if (a91[i]) z = 0; }
    msg->hash = ce;
    return !z;                                   /* the all-zero message is not valid */
}

/* OSD: order 1 + pairs among the nt least reliable info bits; best codeword in cw, its hard errors and distance */
static int osd(ctx_t *c, const float *llr, uint8_t *cw, int *nhard, float *dmin)
{
#ifdef FT8_ARENA_SCRATCH
    uint8_t *perm = c->sc->perm; uint8_t *piv = c->sc->piv; float *r = c->sc->r;
#else
    FT8_SCRATCH uint8_t perm[FTX_LDPC_N], piv[FTX_LDPC_K];
    FT8_SCRATCH float r[FTX_LDPC_N];
#endif
    uint32_t (*M)[NW] = c->w, h[NW], m0[NW], cc[NW], mm[NW];
    for (int i = 0; i < FTX_LDPC_N; i++) { r[i] = llr[i] < 0 ? -llr[i] : llr[i]; perm[i] = (uint8_t)i; }
    for (int i = 1; i < FTX_LDPC_N; i++) {       /* by reliability, descending (insertion sort, stable) */
        uint8_t v = perm[i]; float rv = r[v]; int j = i - 1;
        while (j >= 0 && r[perm[j]] < rv) { perm[j + 1] = perm[j]; j--; }
        perm[j + 1] = v;
    }
    for (int j = 0; j < FTX_LDPC_K; j++) for (int w = 0; w < NW; w++) M[j][w] = 0;
    for (int i = 0; i < FTX_LDPC_N; i++) {       /* column perm[i] of the generator -> column i */
        const uint32_t *col = c->gc[perm[i]];
        for (int q = 0; q < 3; q++)
            for (uint32_t x = col[q]; x; x &= x - 1) { int j = q * 32 + __builtin_ctz(x); M[j][i >> 5] |= 1u << (i & 31); }
    }
    for (int w = 0; w < NW; w++) h[w] = 0;
    for (int i = 0; i < FTX_LDPC_N; i++) if (llr[perm[i]] > 0) h[i >> 5] |= 1u << (i & 31);
    int k = 0;
    for (int col = 0; col < FTX_LDPC_N && k < FTX_LDPC_K; col++) {    /* reduced row echelon form */
        int w = col >> 5, pr = -1; uint32_t b = 1u << (col & 31);
        for (int row = k; row < FTX_LDPC_K; row++) if (M[row][w] & b) { pr = row; break; }
        if (pr < 0) continue;
        if (pr != k) for (int q = 0; q < NW; q++) { uint32_t t = M[k][q]; M[k][q] = M[pr][q]; M[pr][q] = t; }
        for (int row = 0; row < FTX_LDPC_K; row++)
            if (row != k && (M[row][w] & b)) for (int q = 0; q < NW; q++) M[row][q] ^= M[k][q];
        piv[k++] = (uint8_t)col;
    }
    if (k < FTX_LDPC_K) return 0;
    for (int w = 0; w < NW; w++) cc[w] = 0;      /* order 0: info bits = hard decisions at the pivots */
    for (int j = 0; j < FTX_LDPC_K; j++) if (h[piv[j] >> 5] >> (piv[j] & 31) & 1u) for (int q = 0; q < NW; q++) cc[q] ^= M[j][q];
    for (int q = 0; q < NW; q++) m0[q] = cc[q] ^ h[q];
#define DIST(v, out) do { float d_ = 0; for (int q_ = 0; q_ < NW && d_ < best; q_++) { uint32_t x_ = (v)[q_]; \
        while (x_) { int b_ = __builtin_ctz(x_); x_ &= x_ - 1; d_ += rp[q_ * 32 + b_]; } } out = d_; } while (0)
#ifdef FT8_ARENA_SCRATCH
    float *rp = c->sc->rp;                                   /* reliabilities in permuted order (0 past the end) */
#else
    FT8_SCRATCH float rp[NW * 32];                           /* reliabilities in permuted order (0 past the end) */
#endif
    for (int i = 0; i < NW * 32; i++) rp[i] = i < FTX_LDPC_N ? r[perm[i]] : 0.0f;
    float best = 3.0e38f; DIST(m0, best);
    int b1 = -1, b2 = -1, nt = ft8_tune.osd_nt;
    if (nt > FTX_LDPC_K) nt = FTX_LDPC_K;
    for (int i = 0; i < FTX_LDPC_K; i++) {
        float d;
        for (int q = 0; q < NW; q++) mm[q] = m0[q] ^ M[i][q];
        DIST(mm, d);
        if (d < best) { best = d; b1 = i; b2 = -1; }
    }
    if (ft8_tune.osd >= 2)
        for (int i = FTX_LDPC_K - nt; i < FTX_LDPC_K; i++)
            for (int j = i + 1; j < FTX_LDPC_K; j++) {
                float d;
                for (int q = 0; q < NW; q++) mm[q] = m0[q] ^ M[i][q] ^ M[j][q];
                DIST(mm, d);
                if (d < best) { best = d; b1 = i; b2 = j; }
            }
#undef DIST
    int nh = 0;
    for (int q = 0; q < NW; q++) {
        mm[q] = m0[q]; if (b1 >= 0) mm[q] ^= M[b1][q]; if (b2 >= 0) mm[q] ^= M[b2][q];
        cc[q] = mm[q] ^ h[q]; nh += __builtin_popcount(mm[q]);
    }
    for (int i = 0; i < FTX_LDPC_N; i++) cw[perm[i]] = (uint8_t)(cc[i >> 5] >> (i & 31) & 1u);
    *nhard = nh; *dmin = best;
    return 1;
}

static void normalize(float *v, float target_sd)
{
    float s = 0, s2 = 0;
    for (int i = 0; i < FTX_LDPC_N; i++) { s += v[i]; s2 += v[i] * v[i]; }
    float var = (s2 - s * s / FTX_LDPC_N) / FTX_LDPC_N;
    if (var <= 0) var = s2 / FTX_LDPC_N;
    if (var <= 0) return;
    float f = target_sd / dg_sqrtf(var);
    for (int i = 0; i < FTX_LDPC_N; i++) v[i] *= f;
}

static const uint8_t *cand_mag(const ftx_waterfall_t *wf, const ftx_candidate_t *cd)
{
    int o = ((cd->time_offset * wf->time_osr + cd->time_sub) * wf->freq_osr + cd->freq_sub) * wf->num_bins + cd->freq_offset;
    return wf->mag + o;
}

static int hard_sync(const ftx_waterfall_t *wf, const ftx_candidate_t *cd)
{
    const uint8_t *m = cand_mag(wf, cd);
    int n = 0;
    for (int g = 0; g < FT8_NUM_SYNC; g++)
        for (int k = 0; k < FT8_LENGTH_SYNC; k++) {
            int s = g * FT8_SYNC_OFFSET + k, blk = cd->time_offset + s;
            if (blk < 0 || blk >= wf->num_blocks) continue;
            const uint8_t *p = m + s * wf->block_stride;
            int bi = 0; for (int j = 1; j < 8; j++) if (p[j] > p[bi]) bi = j;
            if (bi == kFT8_Costas_pattern[k]) n++;
        }
    return n;
}

/* power of tone j of symbol `sym` of candidate cd, waterfall shifted by dt rows / dfh half bins (0 outside) */
static float cellp(const ctx_t *c, const ftx_waterfall_t *wf, const ftx_candidate_t *cd, int sym, int j, int dt, int dfh)
{
    int fs = cd->freq_sub + dfh, fo = cd->freq_offset;
    if (fs < 0) { fs += FOSR; fo--; } else if (fs >= FOSR) { fs -= FOSR; fo++; }
    int row = (cd->time_offset + sym) * TOSR + cd->time_sub + dt, bin = fo + j;
    if (row < 0 || row >= wf->num_blocks * TOSR || bin < 0 || bin >= wf->num_bins) return 0.0f;
    return c->lin[wf->mag[(row * FOSR + fs) * wf->num_bins + bin]];
}
static float parab(float em, float e0, float ep);
static int dbg_ns, dbg_nh; static float dbg_dm;
static int bp_osd(ctx_t *c, const ftx_candidate_t *cd, float *llr, ftx_message_t *msg, int *how)
{
#ifdef FT8_ARENA_SCRATCH
    float *save = c->sc->save; uint8_t *plain = c->sc->plain;
#else
    FT8_SCRATCH float save[FTX_LDPC_N];
    FT8_SCRATCH uint8_t plain[FTX_LDPC_N];
#endif
    normalize(llr, ft8_tune.scale);
    for (int i = 0; i < FTX_LDPC_N; i++) save[i] = llr[i];
    if (bp(c, llr, ft8_tune.ldpc_iters, plain) == 0 && crc_ok(plain, msg)) { *how = 1; return 1; }
    if (!ft8_tune.osd || cd->score < ft8_tune.osd_min) return 0;
    int nh; float dm;
    if (!osd(c, save, plain, &nh, &dm)) return 0;
    dbg_nh = nh; dbg_dm = dm;
    if (nh > ft8_tune.nhmax || dm > ft8_tune.dmax) return 0;
    if (!crc_ok(plain, msg)) return 0;
    *how = 2;
    return 1;
}
static int decode_cand(ctx_t *c, const ftx_waterfall_t *wf, const ftx_candidate_t *cd, ftx_message_t *msg, int *how)
{
    float llr[FTX_LDPC_N];
    dbg_ns = hard_sync(wf, cd); dbg_nh = -1; dbg_dm = -1;
    if (dbg_ns < ft8_tune.nsync_min) return 0;
    const uint8_t *m = cand_mag(wf, cd);
    for (int k = 0; k < FT8_ND; k++) {
        int si = k + (k < 29 ? 7 : 14), blk = cd->time_offset + si, bi = 3 * k;
        if (blk < 0 || blk >= wf->num_blocks) { llr[bi] = llr[bi + 1] = llr[bi + 2] = 0; continue; }
        const uint8_t *p = m + si * wf->block_stride;
        float s[8];
        if (ft8_tune.llr == 0) for (int j = 0; j < 8; j++) s[j] = (float)p[kFT8_Gray_map[j]] * 0.5f;
        else if (ft8_tune.llr == 1) for (int j = 0; j < 8; j++) s[j] = c->amp[p[kFT8_Gray_map[j]]];
        else for (int j = 0; j < 8; j++) s[j] = dg_sqrtf(c->amp[p[kFT8_Gray_map[j]]]);
        llr[bi]     = mx(mx(s[4], s[5]), mx(s[6], s[7])) - mx(mx(s[0], s[1]), mx(s[2], s[3]));
        llr[bi + 1] = mx(mx(s[2], s[3]), mx(s[6], s[7])) - mx(mx(s[0], s[1]), mx(s[4], s[5]));
        llr[bi + 2] = mx(mx(s[1], s[3]), mx(s[5], s[7])) - mx(mx(s[0], s[2]), mx(s[4], s[6]));
    }
    if (bp_osd(c, cd, llr, msg, how)) return 1;
    if (!ft8_tune.interp) return 0;
    /* second try: powers interpolated towards the true time / frequency (parabola on the Costas tones) */
    float e[3][3];
    for (int a = -1; a <= 1; a++)
        for (int b = -1; b <= 1; b++) {
            float sum = 0;
            for (int g = 0; g < FT8_NUM_SYNC; g++)
                for (int k = 0; k < FT8_LENGTH_SYNC; k++) sum += cellp(c, wf, cd, g * FT8_SYNC_OFFSET + k, kFT8_Costas_pattern[k], a, b);
            e[a + 1][b + 1] = sum;
        }
    float ta = parab(e[0][1], e[1][1], e[2][1]), fb = parab(e[1][0], e[1][1], e[1][2]);
    int dt = ta < 0 ? -1 : 1, df = fb < 0 ? -1 : 1;
    float wa = ta < 0 ? -ta : ta, wb = fb < 0 ? -fb : fb;
    if (wa < 0.05f && wb < 0.05f) return 0;
    float w00 = (1 - wa) * (1 - wb), w10 = wa * (1 - wb), w01 = (1 - wa) * wb, w11 = wa * wb;
    for (int k = 0; k < FT8_ND; k++) {
        int si = k + (k < 29 ? 7 : 14), blk = cd->time_offset + si, bi = 3 * k;
        if (blk < 0 || blk >= wf->num_blocks) { llr[bi] = llr[bi + 1] = llr[bi + 2] = 0; continue; }
        float s[8];
        for (int j = 0; j < 8; j++) {
            int t = kFT8_Gray_map[j];
            float pw = w00 * cellp(c, wf, cd, si, t, 0, 0) + w10 * cellp(c, wf, cd, si, t, dt, 0)
                     + w01 * cellp(c, wf, cd, si, t, 0, df) + w11 * cellp(c, wf, cd, si, t, dt, df);
            s[j] = dg_sqrtf(dg_sqrtf(pw));
        }
        llr[bi]     = mx(mx(s[4], s[5]), mx(s[6], s[7])) - mx(mx(s[0], s[1]), mx(s[2], s[3]));
        llr[bi + 1] = mx(mx(s[2], s[3]), mx(s[6], s[7])) - mx(mx(s[0], s[1]), mx(s[4], s[5]));
        llr[bi + 2] = mx(mx(s[1], s[3]), mx(s[5], s[7])) - mx(mx(s[0], s[2]), mx(s[4], s[6]));
    }
    if (bp_osd(c, cd, llr, msg, how)) { *how += 2; return 1; }
    return 0;
}



static int seen(const ctx_t *c, const ftx_message_t *m)
{
    for (int i = 0; i < c->nsig; i++) {
        int j = 0;
        while (j < FTX_PAYLOAD_LENGTH_BYTES && c->dec[i].payload[j] == m->payload[j]) j++;
        if (j == FTX_PAYLOAD_LENGTH_BYTES) return 1;
    }
    return 0;
}

/* sum of the tone-bin powers of a decoded signal, waterfall rows shifted by dt (half symbols) / dfh (half bins) */
static float tone_energy(const ctx_t *c, const ftx_waterfall_t *wf, const ftx_candidate_t *cd, const uint8_t *tones, int dt, int dfh)
{
    int fs = cd->freq_sub + dfh, fo = cd->freq_offset;
    while (fs < 0) { fs += FOSR; fo--; }
    while (fs >= FOSR) { fs -= FOSR; fo++; }
    float e = 0;
    for (int k = 0; k < NSYM; k++) {
        int row = (cd->time_offset + k) * TOSR + cd->time_sub + dt;
        if (row < 0 || row >= wf->num_blocks * TOSR) continue;
        int bin = fo + tones[k];
        if (bin < 0 || bin >= wf->num_bins) continue;
        e += c->lin[wf->mag[(row * FOSR + fs) * wf->num_bins + bin]];
    }
    return e;
}
/* the time parabola is biased (the energy-vs-offset curve of a Hann-2T window is not a parabola): measured map from
 * its vertex (|d|, grid units of half a symbol) to the true offset, synthetic signals in 5 ms steps */
static const float tmap_d[9] = { 0.0f, 0.04f, 0.08f, 0.12f, 0.165f, 0.22f, 0.29f, 0.375f, 0.49f };
static float tcal(float d)
{
    float a = d < 0 ? -d : d, u = 0.5f;
    for (int i = 1; i < 9; i++)
        if (a <= tmap_d[i]) { u = 0.0625f * ((float)(i - 1) + (a - tmap_d[i - 1]) / (tmap_d[i] - tmap_d[i - 1])); break; }
    if (u > 0.5f) u = 0.5f;
    return d < 0 ? -u : u;
}
static float parab(float em, float e0, float ep)              /* vertex offset of a parabola through -1, 0, +1 */
{
    float den = em - 2.0f * e0 + ep;
    if (!(e0 >= em && e0 >= ep)) return em > ep ? -0.5f : 0.5f;   /* the neighbour row is better: half way */
    if (den >= 0) return 0.0f;
    float d = 0.5f * (em - ep) / den;
    return d < -0.5f ? -0.5f : d > 0.5f ? 0.5f : d;
}

/* noise baseline per bin, WSJT-X style (get_spectrum_baseline: lower envelope of the period's average
 * spectrum): the time-averaged power of each bin (frequency subdivision 0), then its pct-th percentile over
 * +-BW_HALF bins.  In a busy band this includes the floor of weak signals, as WSJT-X's SNR does. */
#define BW_HALF 30
static void baseline(const ctx_t *c, const ftx_waterfall_t *wf, float *b, float *tmp)
{
    int rows = wf->num_blocks * TOSR, nb = wf->num_bins;
    for (int bin = 0; bin < nb; bin++) {
        float sum = 0;
        for (int row = 0; row < rows; row++) sum += c->lin[wf->mag[(row * FOSR) * nb + bin]];
        tmp[bin] = sum / (float)rows;
    }
    float w[2 * BW_HALF + 1];
    for (int bin = 0; bin < nb; bin++) {
        int lo = bin - BW_HALF, hi = bin + BW_HALF, n = 0;
        if (lo < 0) { hi -= lo; lo = 0; }
        if (hi > nb - 1) { lo -= hi - (nb - 1); hi = nb - 1; if (lo < 0) lo = 0; }
        for (int i = lo; i <= hi; i++) {                     /* insertion into sorted w */
            float v = tmp[i]; int j = n++;
            while (j > 0 && w[j - 1] > v) { w[j] = w[j - 1]; j--; }
            w[j] = v;
        }
        b[bin] = w[n * ft8_tune.pct / 100];
    }
}


/* ---- candidate search: ft8_lib's ftx_find_candidates / ft8_sync_score (decode.c, MIT), same results, with the
 * abort poll inside (the full search is ~0.1-0.2 s on the A9) */
static int sync_score(const ftx_waterfall_t *wf, const ftx_candidate_t *cd)
{
    int score = 0, num = 0;
    const uint8_t *mc = wf->mag + (((cd->time_offset * wf->time_osr + cd->time_sub) * wf->freq_osr + cd->freq_sub) * wf->num_bins + cd->freq_offset);
    for (int m = 0; m < FT8_NUM_SYNC; ++m)
        for (int k = 0; k < FT8_LENGTH_SYNC; ++k) {
            int block = FT8_SYNC_OFFSET * m + k, abs_ = cd->time_offset + block;
            if (abs_ < 0) continue;
            if (abs_ >= wf->num_blocks) break;
            const uint8_t *p8 = mc + block * wf->block_stride;
            int sm = kFT8_Costas_pattern[k];
            if (sm > 0) { score += p8[sm] - p8[sm - 1]; ++num; }
            if (sm < 7) { score += p8[sm] - p8[sm + 1]; ++num; }
            if (k > 0 && abs_ > 0) { score += p8[sm] - p8[sm - wf->block_stride]; ++num; }
            if (k + 1 < FT8_LENGTH_SYNC && abs_ + 1 < wf->num_blocks) { score += p8[sm] - p8[sm + wf->block_stride]; ++num; }
        }
    return num > 0 ? score / num : score;
}
static void heap_down(ftx_candidate_t *h, int n)
{
    int cur = 0;
    for (;;) {
        int l = 2 * cur + 1, r = l + 1, sm = cur;
        if (l < n && h[l].score < h[sm].score) sm = l;
        if (r < n && h[r].score < h[sm].score) sm = r;
        if (sm == cur) break;
        ftx_candidate_t t = h[sm]; h[sm] = h[cur]; h[cur] = t; cur = sm;
    }
}
static void heap_up(ftx_candidate_t *h, int n)
{
    int cur = n - 1;
    while (cur > 0) {
        int par = (cur - 1) / 2;
        if (!(h[cur].score < h[par].score)) break;
        ftx_candidate_t t = h[par]; h[par] = h[cur]; h[cur] = t; cur = par;
    }
}
static int find_candidates(ctx_t *c, const ftx_waterfall_t *wf, int ncand, ftx_candidate_t *heap, int min_score, const uint8_t *binok)
{
    int n = 0;
    ftx_candidate_t cd;
    for (cd.time_sub = 0; cd.time_sub < wf->time_osr; ++cd.time_sub)
        for (cd.freq_sub = 0; cd.freq_sub < wf->freq_osr; ++cd.freq_sub)
            for (cd.time_offset = -10; cd.time_offset < 20; ++cd.time_offset) {
                if (poll(c)) return -1;
                for (cd.freq_offset = 0; cd.freq_offset + 7 < wf->num_bins; ++cd.freq_offset) {
                    if (binok && !binok[cd.freq_offset]) continue;
                    cd.score = (int16_t)sync_score(wf, &cd);
                    if (cd.score < min_score) continue;
                    if (n == ncand && cd.score > heap[0].score) { --n; heap[0] = heap[n]; heap_down(heap, n); }
                    if (n < ncand) { heap[n++] = cd; heap_up(heap, n); }
                }
            }
    for (int len = n; len > 1; ) {                           /* descending by score */
        ftx_candidate_t t = heap[len - 1]; heap[len - 1] = heap[0]; heap[0] = t;
        len--; heap_down(heap, len);
    }
    return n;
}

/* one search+decode round over a waterfall; returns 0, or FT8_ERR_ABORT */
static int round_(ctx_t *c, ftx_waterfall_t *wf, int b_lo, ftx_candidate_t *cand, int new0, int new1)
{
    uint8_t binok[1024], *bo = 0;
    if (new1 > new0 && ft8_tune.near && wf->num_bins <= 1024) {   /* after a subtraction: only where something changed */
        for (int b = 0; b < wf->num_bins; b++) binok[b] = 0;
        for (int j = new0; j < new1; j++) {
            int b0 = (int)(c->sig[j].f0 / c->df) - b_lo;
            for (int b = b0 - 9; b <= b0 + 9; b++) if (b >= 0 && b < wf->num_bins) binok[b] = 1;
        }
        bo = binok;
    }
    int nc = find_candidates(c, wf, ft8_tune.max_cand, cand, ft8_tune.min_score, bo);
    if (nc < 0 || poll(c)) return FT8_ERR_ABORT;
    for (int i = 0; i < nc; i++) {
        if (poll(c)) return FT8_ERR_ABORT;
        const ftx_candidate_t *cd = &cand[i];
        ftx_message_t msg;
        int how = 0, dup = 0;
        for (int j = 0; j < i && !dup; j++) {                /* a stronger candidate next to it decoded already */
            const ftx_candidate_t *e = &cand[j];
            if (!(e->score & 0x4000)) continue;              /* marks a decoded candidate (scores are < 0x4000) */
            int dt = (cd->time_offset * TOSR + cd->time_sub) - (e->time_offset * TOSR + e->time_sub);
            int dfq = (cd->freq_offset * FOSR + cd->freq_sub) - (e->freq_offset * FOSR + e->freq_sub);
            if (dt >= -ft8_tune.skip && dt <= ft8_tune.skip && dfq >= -ft8_tune.skip && dfq <= ft8_tune.skip) dup = 1;
        }
        if (dup) continue;
        if (new1 > new0 && ft8_tune.near) {                 /* after a subtraction: only where something changed */
            float fc = ((float)(b_lo + cd->freq_offset) + (float)cd->freq_sub / FOSR) * c->df;
            int near = 0;
            for (int j = new0; j < new1 && !near; j++) { float d = fc - c->sig[j].f0; if (d > -9.0f * c->df && d < 9.0f * c->df) near = 1; }
            if (!near) continue;
        }
        int okd = decode_cand(c, wf, cd, &msg, &how);

        DBG("CAND %.2f %.3f %d %d %d %.1f %d\n", ((float)(b_lo + cd->freq_offset) + (float)cd->freq_sub / FOSR) * c->df,
            (float)c->s0 / (float)c->fs + ((float)cd->time_offset - 1.0f + (float)cd->time_sub / TOSR) * (float)c->block / (float)c->fs - c->nom,
            cd->score, dbg_ns, dbg_nh, (double)dbg_dm, okd ? how : 0);
        if (okd) cand[i].score |= 0x4000;
        if (!okd || seen(c, &msg) || c->nsig >= MAX_SIG) continue;
        c->dec[c->nsig] = msg;
        sig_t *s = &c->sig[c->nsig++];
        ft8_encode(msg.payload, s->tones);
        /* refine time / frequency on the waterfall grid */
        float e0 = tone_energy(c, wf, cd, s->tones, 0, 0);
        float dtr = parab(tone_energy(c, wf, cd, s->tones, -1, 0), e0, tone_energy(c, wf, cd, s->tones, 1, 0));
        float dfr = parab(tone_energy(c, wf, cd, s->tones, 0, -1), e0, tone_energy(c, wf, cd, s->tones, 0, 1));
        float tsym = (float)cd->time_offset - 1.0f + ((float)cd->time_sub + tcal(dtr)) / TOSR;
        float start = (float)c->s0 + tsym * (float)c->block;          /* samples */
        s->t0 = (int32_t)(start + (start >= 0 ? 0.5f : -0.5f));
        s->f0 = ((float)(b_lo + cd->freq_offset) + ((float)cd->freq_sub + dfr) / FOSR) * c->df;
        /* SNR: mean tone-bin power over the noise baseline */
        float xs = 0, xb = 0;
        const uint8_t *mg = cand_mag(wf, cd);
        for (int k = 0; k < NSYM; k++) {
            int blk = cd->time_offset + k;
            if (blk < 0 || blk >= wf->num_blocks) continue;
            xs += c->lin[mg[k * wf->block_stride + s->tones[k]]];
            xb += c->base[cd->freq_offset + s->tones[k]];
        }
        float snr = -24.0f;
        if (xb > 0) {
            float a = xs / xb - 1.0f;
            if (a < 0.001f) a = 0.001f;
            snr = 10.0f * dg_log10f(a) + ft8_tune.snr_off;
            if (snr < -24.0f) snr = -24.0f;
        }
        char text[FTX_MAX_MESSAGE_LENGTH + 4];
        ftx_message_offsets_t offs;
        text[0] = 0;
        if (ftx_message_decode(&msg, &hash_if, text, &offs) != FTX_MESSAGE_RC_OK || !text[0]) continue;
        if (c->nout >= c->maxout) continue;
        ft8_result_t *r = &c->out[c->nout++];
        hcpy(r->msg, text, (int)sizeof r->msg);
        r->freq_hz = s->f0;
        r->dt_s = start / (float)c->fs - c->nom;
        r->snr_db = snr;
        r->quality = cd->score & 0x3FFF;
        DBG("  dec sc=%d to=%d ts=%d fo=%d fs=%d how=%d dt%+.2f df%+.2f %s\n", cd->score, cd->time_offset, cd->time_sub, cd->freq_offset, cd->freq_sub, how, (double)dtr, (double)dfr, text);
    }
    return 0;
}

int ft8_decode(const dsrc_t *src, float f_lo, float f_hi, float nominal_start_s,
               ft8_result_t *out, int maxout, void *work, uint32_t work_len, int (*should_abort)(void),
               ft8_state_t *st)
{
    peak = 0;
    if (!src || !src->read || !out || maxout <= 0 || !work || ((uintptr_t)work & 7u) || work_len < FT8_WORK_MIN) return FT8_ERR_ARGS;
    if (src->fs < 6000 || src->fs > 12000 || (src->fs * 4) % 25) return FT8_ERR_ARGS;   /* fs*0.16 integer */
    uintptr_t base = (uintptr_t)work, p = base, end = base + work_len;
    ctx_t *c = (ctx_t *)p;                     p = al8(p + sizeof(ctx_t));
    zero(c, sizeof *c);
    c->src = src; c->ab = should_abort; c->out = out; c->maxout = maxout; c->nom = nominal_start_s;
    c->fs = (int)src->fs; c->block = c->fs * 4 / 25; c->sub = c->block / TOSR; c->nfft = c->block * FOSR;
    if ((c->block & 1) || (c->nfft & 3)) return FT8_ERR_ARGS;
    c->N = c->nfft / 2; c->m = c->N;
    while (!(c->m & 1)) c->m >>= 1;
    c->P = c->N / c->m;                                      /* N = P * m, P = 2^a, m odd */
    if (c->m > 15 || c->P < 8) return FT8_ERR_ARGS;
    c->df = (float)c->fs / (float)c->block;
    c->s0 = (int32_t)((nominal_start_s - 0.5f) * (float)c->fs + (nominal_start_s >= 0.5f ? 0.5f : -0.5f));
    int32_t avail = (int32_t)src->len - c->s0;
    c->nblk = avail <= 0 ? 0 : (int)((avail + c->block - 1) / c->block);
    if (c->nblk > MAX_BLOCKS) c->nblk = MAX_BLOCKS;
    if (c->nblk < NSYM / 2) return 0;                        /* less than half a signal */

    /* band in 6.25 Hz bins: tone 0 in [f_lo, f_hi], tones 0..7 below fs/2 */
    int b_lo = (int)(f_lo / c->df), b_top = (int)(f_hi / c->df + 0.999f) + 8;
    int b_max = (c->N - FOSR) / FOSR;
    if (b_lo < 1) b_lo = 1;
    if (b_top > b_max) b_top = b_max;
    if (b_top - b_lo < 16) return FT8_ERR_ARGS;

    float *qs = (float *)p;                    p = al8(p + sizeof(float) * (uint32_t)(c->nfft / 4 + 1));
    for (int j = 0; j <= c->nfft / 4; j++) { double s, co; dg_sincos(6.283185307179586 * (double)j / (double)c->nfft, &s, &co); qs[j] = (float)s; }
    c->qs = qs;
    {   /* 0.5 dB per step from -120 dB */
        float v = 1e-12f; const float r = 1.12201845f;       /* 10^(0.5/10) */
        for (int i = 0; i < 256; i++) { c->lin[i] = v; c->amp[i] = dg_sqrtf(v); v *= r; }
    }
    gen_init(c);
    bp_init(c);
    for (int i = 0; i <= STAB + STAB / 4; i++) { double sv, cv; dg_sincos(6.283185307179586 * (double)i / STAB, &sv, &cv); c->sn[i] = (float)sv; }
    if (c->block > (int)sizeof c->gidx) return FT8_ERR_ARGS;
    for (int i = 0; i < c->block; i++) c->gidx[i] = (uint8_t)(i * 128 / c->block);
#ifdef FT8_ARENA_SCRATCH
    c->sc = (struct ft8_scratch *)p;  p = al8(p + sizeof(struct ft8_scratch));   /* bp/osd scratch in the arena (not .bss) */
#endif
    uintptr_t fixed_end = p;
    /* after the waterfall: max(streaming STFT, candidate heap) */
    uint32_t stream_len = stream_bytes(c, MAX_SIG);
    uint32_t cand_len = (uint32_t)al8((uint32_t)ft8_tune.max_cand * sizeof(ftx_candidate_t));
    uint32_t dec_len = cand_len + (uint32_t)(FTX_LDPC_K * 6 * sizeof(uint32_t));
    uint32_t scratch = stream_len > dec_len ? stream_len : dec_len;
    uint32_t row = (uint32_t)(c->nblk * TOSR * FOSR);
    if (fixed_end + scratch + row * 24 + 512 > end) return FT8_ERR_ARGS;
    int nb_max = (int)((end - fixed_end - scratch - 16) / (row + sizeof(float)));

    cur_st = st;
    if (st) for (int i = 0; i < FT8_HASH_SLOTS; i++) if (st->e[i].used && st->e[i].age < 0xFFFF) st->e[i].age++;

    int rc = 0;
    for (int pb = b_lo; pb + 8 < b_top && !rc; ) {           /* band passes (one if it fits) */
        int nb = b_top - pb;
        if (nb > nb_max) nb = nb_max;
        float *bl = (float *)fixed_end;
        uint8_t *mag = (uint8_t *)al8(fixed_end + sizeof(float) * (uintptr_t)nb);
        uint8_t *scr = (uint8_t *)al8((uintptr_t)mag + (uintptr_t)nb * row);
        uint32_t used = (uint32_t)((uintptr_t)scr + scratch - base);
        if (used > peak) peak = used;
        c->base = bl;
        int first = c->nsig;                                 /* signals of an earlier band pass are not subtracted */
        int newfrom = first;                                 /* signals subtracted for the first time in this pass */
        for (int sp = 0; sp < ft8_tune.passes; sp++) {       /* 0: raw audio; then minus everything decoded so far */
            int nsub = c->nsig - first;
            if (sp > 0 && nsub == 0) break;
            DBG("band %d..%d (%d bins) pass %d: subtract %d, nblk %d, wf %u B, peak %u\n", pb, pb + nb, nb, sp, nsub, c->nblk, nb * row, used);
            rc = build_wf(c, pb, nb, mag, scr, c->sig + first, sp ? nsub : 0);
            if (rc) break;
            ftx_waterfall_t wf;
            wf.max_blocks = c->nblk; wf.num_blocks = c->nblk; wf.num_bins = nb; wf.time_osr = TOSR; wf.freq_osr = FOSR;
            wf.mag = mag; wf.block_stride = TOSR * FOSR * nb; wf.protocol = FTX_PROTOCOL_FT8;
            if (sp == 0) baseline(c, &wf, bl, (float *)scr);
#ifdef FT8_DEBUG
            for (int i = first; i < c->nsig; i++) {          /* signal power left at each decoded signal (dB over noise) */
                const sig_t *g = &c->sig[i];
                float tq = ((float)(g->t0 - c->s0) / (float)c->block + 1.0f) * TOSR, fq = (g->f0 / c->df - (float)pb) * FOSR;
                int tr = (int)(tq + 0.5f), fr = (int)(fq + 0.5f);
                ftx_candidate_t cd; cd.time_offset = (int16_t)(tr >> 1); cd.time_sub = (uint8_t)(tr & 1); cd.freq_offset = (int16_t)(fr >> 1); cd.freq_sub = (uint8_t)(fr & 1);
                if (cd.freq_offset < 0 || cd.freq_offset + 8 > nb) continue;
                float e = tone_energy(c, &wf, &cd, g->tones, 0, 0) / NSYM, nz = bl[cd.freq_offset + 3];
                DBG("SUBCHK pass %d sig %d f %.1f: %.1f dB over noise\n", sp, i, (double)g->f0, (double)(10.0f * dg_log10f(e / nz)));
            }
#endif
            int before = c->nsig;
            c->w = (uint32_t (*)[6])(scr + cand_len);
            rc = round_(c, &wf, pb, (ftx_candidate_t *)scr, sp ? newfrom : 0, sp ? before : 0);
            newfrom = before;
            if (rc || c->nsig == before) break;
        }
        if (rc || pb + nb >= b_top) break;
        pb += nb - 7;                                        /* next band pass: tone-0 bins not covered yet */
    }
    cur_st = 0;
    if (rc == FT8_ERR_ABORT) return c->nout > 0 ? c->nout : FT8_ERR_ABORT;
    if (rc == FT8_ERR_IO) return c->nout > 0 ? c->nout : FT8_ERR_IO;
    if (rc) return rc;
    return c->nout;
}
