/* SPDX-License-Identifier: GPL-3.0-or-later
 * jt4dec.c - JT4 decoder (Fano path, no deep search) for the IC-705 ICAP digi app.
 *
 * Signal model (WSJT-X lib/gen4.f90, jt4.f90): 206 4-FSK symbols at 11025/2520 baud,
 * tone = 2*data + sync, sync = npr(2:207) (inverted for some report messages),
 * tone spacing 4.375 Hz * (1,2,4,9,18,36,72) for A..G. Data = 72-bit JT message,
 * K=32 r=1/2 convolutional code (103 bits incl. tail), bit-reversal interleave,
 * Fano decoding with the getmet4 metric table.
 *
 * Receiver (own streaming design, WSJT-X structure):
 *  1. noise spectrum from a sparse pre-pass;
 *  2. half-symbol-step spectrogram (symbol-length window, 4096-pt zero-padded FFT)
 *     streamed from SD, sync CCF against npr accumulated on the fly for every
 *     (tone-0 bin, lag) pair (xcor4 metric, noise-normalised); frequency blocks
 *     that do not fit the work area are processed in extra passes;
 *  3. per sync candidate: coherent per-symbol tone correlation (decode4 style) on a
 *     3 (dt) x 5 (df) refinement grid, using block prefix sums of 20 continuous-phase
 *     mixers; ALL candidates that fit the work area are extracted in ONE sequential
 *     read; soft symbols -> extract4 normalisation -> deinterleave -> Fano
 *     (10000 cycles/bit) -> unpack.
 *
 * Speed (Cortex-A9): the arithmetic of every stage is the one of the first version, operation for
 * operation, so the decodes are the same (two exceptions: the SNR estimate adds its terms in another
 * order, and the sync candidates no longer depend on where the band is split into frequency blocks:
 * the result is the one of the first version with unlimited memory). Only the cost changed:
 *  - FFT: bit-reversed load, twiddle tables per stage, 4 butterflies per NEON step, only the
 *    bins of the search band are untangled;
 *  - CCF: the sync observable of KBUF steps is buffered, so each lag row is loaded and stored once
 *    per KBUF steps (small hot set, 4 bins per NEON step);
 *  - mixers: 4 per NEON step; one SD pass for all candidates instead of one per candidate.
 * The scalar code (x86 host, -DJT4_NO_NEON) performs the same operations in the same order as the
 * NEON code: no fused multiply-add anywhere (VFPv3 / NEON VMLA and VMLS round the product).
 * Cost on the A9 (240 KiB work, 55 s capture, 200..3000 Hz): 3 SD passes (JT4A 4) and 0.05..1.4 G
 * instructions; the top is 10 sync candidates x 3 failed Fano runs.
 * Work layout: reader buffer, FFT buffer and tables first (the hot set, 39 KiB; JT4A 70 KiB), then
 * the CCF rows (touched once per KBUF steps) / the per-candidate extraction state.
 * Derived from WSJT-X (C) K1JT et al., GPL-3.0.
 */
#include "jt4dec.h"
#include "fano232.h"
#include "jt72msg.h"

#if defined(__ARM_NEON) && !defined(JT4_NO_NEON)
#include <arm_neon.h>
#define JT4_NEON 1
#else
#define JT4_NEON 0
#endif

#ifdef JT4_DEBUG
extern void jt4_dbg(const char *fmt, ...);
#define DBG(...) jt4_dbg(__VA_ARGS__)
#else
#define DBG(...) do { } while (0)
#endif

#define NSYM   206
#ifndef JT4_NFFT_A
#define JT4_NFFT_A    4096 /* sync FFT for JT4A: 1.95 Hz bins (tones 4.375 Hz apart) */
#endif
#ifndef JT4_NFFT_WIDE
#define JT4_NFFT_WIDE 2048 /* B..G: 3.9 Hz bins, half the FFT work and bins per pass */
#endif
#define BAUD   (11025.0f / 2520.0f)
#define MAXC   10          /* sync candidates kept */
#define MAXFAIL 40         /* failed Fano runs per call (~33M ARM instr each); MAXC * NTRY = 30 is the real bound */
#define NDT    3           /* time refinement trials (+-DTSTEP) */
#define NDF    5           /* frequency refinement trials (+-2*DFSTEP: 3.9 Hz sync bins for B..G) */
#define NTR    (NDT * NDF)
#define NTRY   3           /* Fano attempts per candidate (best in-parity energy first) */
#define RING   48          /* block prefix-sum ring: a symbol is 46 blocks (+1 for the start) */
#define NMIX   (NDF * 4)   /* mixers per candidate */
#define KBUF   4           /* sync steps buffered before the CCF rows are updated */
#define MARG   2           /* bins a frequency block computes beyond its edge (peak test reach) */
#define RDCAP  2048        /* reader cache (samples) */
#define ZMIN   5.0f        /* minimum sync z-score to attempt a decode (noise max ~4-5 over 200-3000 Hz) */
#define DFSTEP 0.6f        /* Hz */
#define DT0    (NDT / 2)
#define DF0    (NDF / 2)
#define DTSTEP 0.03f       /* s */
#define FANO_LIMIT 10000
#ifndef JT4_DT_LO
#define JT4_DT_LO (-1.0f)  /* start-time search window around nominal_start_s (s); */
#endif
#ifndef JT4_DT_HI
#define JT4_DT_HI 5.0f     /* +5 s so EME echoes (2.5 s moon delay) are still found */
#endif

/* WSJT-X jt4.f90 npr(1:207); TX symbol i (0-based) uses npr[i+1] */
static const uint8_t npr[207] = {
  0,0,0,0,1,1,0,0,0,1,1,0,1,1,0,0,1,0,1,0,0,0,0,0,0,0,1,1,0,0,
  0,0,0,0,0,0,0,0,0,0,1,0,1,1,0,1,1,0,1,0,1,1,1,1,1,0,1,0,0,0,
  1,0,0,1,0,0,1,1,1,1,1,0,0,0,1,0,1,0,0,0,1,1,1,1,0,1,1,0,0,1,
  0,0,0,1,1,0,1,0,1,0,1,0,1,0,1,1,1,1,1,0,1,0,1,0,1,1,0,1,0,1,
  0,1,1,1,0,0,1,0,1,1,0,1,1,1,1,0,0,0,0,1,1,0,1,1,0,0,0,1,1,1,
  0,1,1,1,0,1,1,1,0,0,1,0,0,0,1,1,0,1,1,0,0,1,0,0,0,1,1,1,1,1,
  1,0,0,1,1,0,0,0,0,1,1,0,0,0,1,0,1,1,0,1,1,1,1,0,1,0,1,
};
static const uint8_t nch[7] = { 1, 2, 4, 9, 18, 36, 72 };

typedef struct { float f, t, z; int flip; } cand_t;

typedef struct {                     /* real FFT of size n as a complex FFT of size m = n/2 */
    int n, m;
    float *z;                        /* 2*m */
    float *twr, *twi;                /* m-1 each: the stage with half-length h uses [h-1 .. 2h-2] */
    float *ur, *ui;                  /* n/4+1 each: untangle twiddles exp(-i*2*pi*k/n) */
    uint16_t *rev;                   /* m: bit reversal */
} fftc_t;

typedef struct {
    const dsrc_t *src;
    int (*abort)(void);
    dg_rdr_t rd;
    float fs, nsps, hs, t0, df;
    int nspsi, nl, ns, mode4, nfft, nh;
    int toff[4];
    int ncand;
    cand_t cand[MAXC];
    fftc_t f;
    float *pw, *inv;                 /* nh+2 each */
} ctx_t;

static int aborted(ctx_t *c) { return c->abort && c->abort(); }

static void cand_insert(ctx_t *c, float f, float t, float z, int flip)
{
    int i, j;
    for (i = 0; i < c->ncand; i++) {       /* same signal found twice (block edge / sidelobe) */
        float dfr = c->cand[i].f - f, dtt = c->cand[i].t - t;
        if (dfr < 0) dfr = -dfr;
        if (dtt < 0) dtt = -dtt;
        if (c->cand[i].flip == flip && dfr < 2.5f * c->df && dtt < 1.5f * c->hs) {
            if (z <= c->cand[i].z) return;
            for (j = i; j < c->ncand - 1; j++) c->cand[j] = c->cand[j + 1];
            c->ncand--;
            break;
        }
    }
    for (i = c->ncand; i > 0 && c->cand[i - 1].z < z; i--)
        if (i < MAXC) c->cand[i] = c->cand[i - 1];
    if (i >= MAXC) return;
    c->cand[i].f = f; c->cand[i].t = t; c->cand[i].z = z; c->cand[i].flip = flip;
    if (c->ncand < MAXC) c->ncand++;
}

/* ---- FFT ---------------------------------------------------------------------------------- */

/* exp(-i*2*pi*k/n) for 0<=k<n/2 from the quarter-wave sine table */
static inline void tw(const float *q, uint32_t n, uint32_t k, float *c, float *s)
{
    uint32_t n4 = n >> 2;
    if (k <= n4) { *c = q[n4 - k]; *s = -q[k]; }
    else { *c = -q[k - n4]; *s = -q[(n >> 1) - k]; }
}

/* q: scratch, n/4+1 floats (may be f->z) */
static void fft_setup(fftc_t *f, float *q)
{
    uint32_t n = (uint32_t)f->n, m = (uint32_t)f->m, h, s0, i, k;
    dg_fft_table(q, n);
    for (h = 1; h < m; h <<= 1)
        for (s0 = 0; s0 < h; s0++) tw(q, n, s0 * (n / (2 * h)), &f->twr[h - 1 + s0], &f->twi[h - 1 + s0]);
    for (k = 0; k <= n / 4; k++) tw(q, n, k, &f->ur[k], &f->ui[k]);
    for (i = 0; i < m; i++) {
        uint32_t r = 0, b;
        for (b = 1, k = m >> 1; b < m; b <<= 1, k >>= 1) if (i & b) r |= k;
        f->rev[i] = (uint16_t)r;
    }
}

/* |X[k]|^2 and |X[m-k]|^2 from Z[k], Z[m-k] (1 <= k <= m/2) */
static inline void untangle1(const float *z, int m, int k, float wr, float wi, float *pw)
{
    int k2 = m - k;
    float ar = z[2 * k], ai = z[2 * k + 1], br = z[2 * k2], bi = z[2 * k2 + 1];
    float er = 0.5f * (ar + br), ei = 0.5f * (ai - bi);
    float dr = 0.5f * (ar - br), di = 0.5f * (ai + bi);
    float tr = wr * dr - wi * di, ti = wr * di + wi * dr;
    float xr = er + ti, xi = ei - tr, yr = er - ti, yi = ei + tr;
    pw[k] = xr * xr + xi * xi;
    pw[k2] = yr * yr + yi * yi;       /* after pw[k]: at k = m/2 this one stays */
}

/* Power spectrum of x[0..cnt) zero-padded to n points: pw[k] = |X[k]|^2 for k0 <= k <= k1
 * (0 <= k0 <= k1 <= m; other entries of pw[0..m] are undefined). */
static void spectrum(const fftc_t *f, const float *x, int cnt, float *pw, int k0, int k1)
{
    const int m = f->m;
    float *z = f->z;
    int i, h;
    /* bit-reversed load of z[i] = x[2i] + j x[2i+1] */
#if JT4_NEON
    { const float32x4_t zero = vdupq_n_f32(0.f); for (i = 0; i < 2 * m; i += 4) vst1q_f32(z + i, zero); }
#else
    for (i = 0; i < 2 * m; i++) z[i] = 0.f;
#endif
    for (i = 0; i + 1 < cnt; i += 2) {
        float *d = z + 2 * f->rev[i >> 1];
        d[0] = x[i]; d[1] = x[i + 1];
    }
    if (i < cnt) z[2 * f->rev[i >> 1]] = x[i];
    /* stages 1 and 2 (twiddles 1 and -j: no products) */
    for (i = 0; i < 2 * m; i += 8) {
        float *p = z + i;
        float a0r = p[0] + p[2], a0i = p[1] + p[3], a1r = p[0] - p[2], a1i = p[1] - p[3];
        float a2r = p[4] + p[6], a2i = p[5] + p[7], a3r = p[4] - p[6], a3i = p[5] - p[7];
        p[0] = a0r + a2r; p[1] = a0i + a2i; p[4] = a0r - a2r; p[5] = a0i - a2i;
        p[2] = a1r + a3i; p[3] = a1i - a3r; p[6] = a1r - a3i; p[7] = a1i + a3r;
    }
    /* stages with half-length h >= 4 */
    for (h = 4; h < m; h <<= 1) {
        const float *wr = f->twr + h - 1, *wi = f->twi + h - 1;
        int i0, s;
        for (i0 = 0; i0 < m; i0 += 2 * h) {
            float *a = z + 2 * i0, *b = a + 2 * h;
#if JT4_NEON
            for (s = 0; s < h; s += 4) {
                float32x4x2_t A = vld2q_f32(a + 2 * s), Bv = vld2q_f32(b + 2 * s);
                const float32x4_t cr = vld1q_f32(wr + s), ci = vld1q_f32(wi + s);
                const float32x4_t tr = vmlsq_f32(vmulq_f32(Bv.val[0], cr), Bv.val[1], ci);
                const float32x4_t ti = vmlaq_f32(vmulq_f32(Bv.val[0], ci), Bv.val[1], cr);
                Bv.val[0] = vsubq_f32(A.val[0], tr); Bv.val[1] = vsubq_f32(A.val[1], ti);
                A.val[0] = vaddq_f32(A.val[0], tr); A.val[1] = vaddq_f32(A.val[1], ti);
                vst2q_f32(a + 2 * s, A); vst2q_f32(b + 2 * s, Bv);
            }
#else
            for (s = 0; s < h; s++) {
                float tr = b[2 * s] * wr[s] - b[2 * s + 1] * wi[s];
                float ti = b[2 * s] * wi[s] + b[2 * s + 1] * wr[s];
                b[2 * s] = a[2 * s] - tr; b[2 * s + 1] = a[2 * s + 1] - ti;
                a[2 * s] += tr; a[2 * s + 1] += ti;
            }
#endif
        }
    }
    /* untangle the pairs (k, m-k) that hold a wanted bin */
    {
        int lo = m, hi = 0, k;
        if (k0 <= m / 2) { lo = k0 > 1 ? k0 : 1; hi = k1 < m / 2 ? k1 : m / 2; }
        if (k1 >= m / 2) {
            int l2 = m - (k1 < m - 1 ? k1 : m - 1), h2 = m - (k0 > m / 2 ? k0 : m / 2);
            if (l2 < lo) lo = l2;
            if (h2 > hi) hi = h2;
        }
        k = lo;
#if JT4_NEON
        {
            const float32x4_t half = vdupq_n_f32(0.5f);
            for (; k + 3 <= hi; k += 4) {
                const float32x4x2_t A = vld2q_f32(z + 2 * k), Bq = vld2q_f32(z + 2 * (m - k - 3));
                float32x4_t br = vrev64q_f32(Bq.val[0]), bi = vrev64q_f32(Bq.val[1]);
                const float32x4_t wr = vld1q_f32(f->ur + k), wi = vld1q_f32(f->ui + k);
                float32x4_t er, ei, dr, di, tr, ti, xr, xi, yr, yi, py;
                br = vextq_f32(br, br, 2); bi = vextq_f32(bi, bi, 2);
                er = vmulq_f32(half, vaddq_f32(A.val[0], br)); ei = vmulq_f32(half, vsubq_f32(A.val[1], bi));
                dr = vmulq_f32(half, vsubq_f32(A.val[0], br)); di = vmulq_f32(half, vaddq_f32(A.val[1], bi));
                tr = vmlsq_f32(vmulq_f32(wr, dr), wi, di); ti = vmlaq_f32(vmulq_f32(wr, di), wi, dr);
                xr = vaddq_f32(er, ti); xi = vsubq_f32(ei, tr); yr = vsubq_f32(er, ti); yi = vaddq_f32(ei, tr);
                vst1q_f32(pw + k, vmlaq_f32(vmulq_f32(xr, xr), xi, xi));
                py = vrev64q_f32(vmlaq_f32(vmulq_f32(yr, yr), yi, yi));
                vst1q_f32(pw + (m - k - 3), vextq_f32(py, py, 2));
            }
        }
#endif
        for (; k <= hi; k++) untangle1(z, m, k, f->ur[k], f->ui[k], pw);
        if (k0 == 0) pw[0] = (z[0] + z[1]) * (z[0] + z[1]);
        if (k1 == m) pw[m] = (z[0] - z[1]) * (z[0] - z[1]);
    }
}

/* power spectrum of the symbol-length window starting at step s: pw[k0..k1] */
static int step_spectrum(ctx_t *c, int s, int k0, int k1)
{
    int32_t a = (int32_t)(c->t0 + (float)s * c->hs + (c->t0 + (float)s * c->hs >= 0 ? 0.5f : -0.5f));
    const float *x = dg_rdr_span(&c->rd, a, (uint32_t)c->nspsi);
    if (!x) return -1;
    spectrum(&c->f, x, c->nspsi, c->pw, k0, k1);
    return 0;
}

/* ---- sync search -------------------------------------------------------------------------- */

/* row[j] += (neg ? -a[j] : a[j]) for one or two buffered steps, in step order */
static void row_add(float *row, int nb, const float *a1, int neg1, const float *a2, int neg2)
{
    int j = 0;
#if JT4_NEON
    const uint32x4_t m1 = vdupq_n_u32(neg1 ? 0x80000000u : 0u), m2 = vdupq_n_u32(neg2 ? 0x80000000u : 0u);
    if (a2) {
        for (; j + 4 <= nb; j += 4) {
            float32x4_t r = vld1q_f32(row + j);
            r = vaddq_f32(r, vreinterpretq_f32_u32(veorq_u32(vreinterpretq_u32_f32(vld1q_f32(a1 + j)), m1)));
            r = vaddq_f32(r, vreinterpretq_f32_u32(veorq_u32(vreinterpretq_u32_f32(vld1q_f32(a2 + j)), m2)));
            vst1q_f32(row + j, r);
        }
    } else {
        for (; j + 4 <= nb; j += 4) {
            float32x4_t r = vld1q_f32(row + j);
            r = vaddq_f32(r, vreinterpretq_f32_u32(veorq_u32(vreinterpretq_u32_f32(vld1q_f32(a1 + j)), m1)));
            vst1q_f32(row + j, r);
        }
    }
#endif
    for (; j < nb; j++) {
        float r = row[j];
        if (neg1) r -= a1[j]; else r += a1[j];
        if (a2) { if (neg2) r -= a2[j]; else r += a2[j]; }
        row[j] = r;
    }
}

/* 1+2: sync search. abuf: KBUF*fper floats, ccf: nl*fper floats. returns 0, JT4_ERR_ABORT or JT4_ERR_IO */
static int sync_search(ctx_t *c, int blo, int bhi, float *abuf, float *ccf, int fper)
{
    int s, b, j;
    float *pw = c->pw, *inv = c->inv;
    /* --- noise spectrum (sparse pre-pass, smoothed over +-8 bins) --- */
    {
        const int bn = bhi + c->toff[3];          /* last bin whose noise level is used */
        const int kn = bn + 8;                    /* <= nh - 2 */
        int nst = 0;
        for (b = 0; b <= kn; b++) inv[b] = 0.f;
        for (s = 0; s < c->ns; s += 6) {
            if (aborted(c)) return JT4_ERR_ABORT;
            if (step_spectrum(c, s, 0, kn)) return JT4_ERR_IO;
            for (b = 0; b <= kn; b++) inv[b] += pw[b];
            nst++;
        }
        {
            float acc = 0.f;
            int lo = 0, hi = -1;
            for (b = 0; b <= bn; b++) {       /* moving average -> pw[] */
                while (hi < b + 8) acc += inv[++hi];
                while (lo < b - 8) acc -= inv[lo++];
                pw[b] = acc / (float)((hi - lo + 1) * nst);
            }
            for (b = 0; b <= bn; b++) inv[b] = pw[b] > 0.f ? 1.f / pw[b] : 0.f;
        }
    }
    /* --- streamed sync CCF, one pass per frequency block --- */
    {
        const float zs = 1.f / (1.58f * 14.3527f);       /* 1/(std(a)*sqrt(206)) */
        const int t1 = c->toff[1], t2 = c->toff[2], t3 = c->toff[3];
        int bi;
        /* A block that does not reach the band edge also computes MARG bins beyond it, so the peak
         * test and the frequency interpolation at a block edge see the same neighbours as inside a
         * block: the candidates do not depend on how the band was split. */
        const int bstep = bhi - blo + 1 <= fper ? fper : fper - 2 * MARG;
        for (bi = blo; bi <= bhi; bi += bstep) {
            int nbi = bhi - bi + 1, ml, mr, b0, nb, L;
            if (nbi > bstep) nbi = bstep;
            ml = bi - blo < MARG ? bi - blo : MARG;
            mr = bhi - (bi + nbi - 1) < MARG ? bhi - (bi + nbi - 1) : MARG;
            b0 = bi - ml; nb = nbi + ml + mr;
            for (j = 0; j < nb * c->nl; j++) ccf[j] = 0.f;
            for (s = 0; s < c->ns; s++) {
                float *a = abuf + (s & (KBUF - 1)) * nb;
                const float *p = pw + b0;
                if ((s & 3) == 0 && aborted(c)) return JT4_ERR_ABORT;
                if (step_spectrum(c, s, b0, b0 + nb - 1 + t3)) return JT4_ERR_IO;
                j = b0;
#if JT4_NEON
                for (; j + 4 <= b0 + nb + t3; j += 4) vst1q_f32(pw + j, vmulq_f32(vld1q_f32(pw + j), vld1q_f32(inv + j)));
#endif
                for (; j < b0 + nb + t3; j++) pw[j] = pw[j] * inv[j];
                j = 0;
#if JT4_NEON
                for (; j + 4 <= nb; j += 4)
                    vst1q_f32(a + j, vsubq_f32(vmaxq_f32(vld1q_f32(p + j + t1), vld1q_f32(p + j + t3)),
                                               vmaxq_f32(vld1q_f32(p + j), vld1q_f32(p + j + t2))));
#endif
                for (; j < nb; j++) {
                    float v0 = p[j], v1 = p[j + t1], v2 = p[j + t2], v3 = p[j + t3];
                    a[j] = (v1 > v3 ? v1 : v3) - (v0 > v2 ? v0 : v2);
                }
                /* every KBUF steps: add the buffered steps to their lag rows (step s feeds the lags L
                 * with s-L even, 0 <= s-L <= 2*(NSYM-1); sign from the sync vector) */
                if ((s & (KBUF - 1)) == KBUF - 1 || s == c->ns - 1) {
                    const int sa = s & ~(KBUF - 1);
                    for (L = 0; L < c->nl; L++) {
                        const float *a1 = 0, *a2 = 0;
                        int n1 = 0, n2 = 0, q;
                        for (q = sa + ((sa ^ L) & 1); q <= s; q += 2) {
                            if (q < L || q - L > 2 * (NSYM - 1)) continue;
                            if (!a1) { a1 = abuf + (q & (KBUF - 1)) * nb; n1 = !npr[(q - L) / 2 + 1]; }
                            else { a2 = abuf + (q & (KBUF - 1)) * nb; n2 = !npr[(q - L) / 2 + 1]; }
                        }
                        if (a1) row_add(ccf + L * nb, nb, a1, n1, a2, n2);
                    }
                }
            }
            /* peaks: best |ccf| over lag per bin, local max over +-2 bins */
            {
                float *best = abuf;                   /* reuse */
                float *bl = abuf + nb;
                for (j = 0; j < nb; j++) {   /* best[j] = signed z at the best lag */
                    float m = 0.f, sv = 0.f; int lb = 0;
                    for (L = 0; L < c->nl; L++) {
                        float v = ccf[L * nb + j], av = v < 0 ? -v : v;
                        if (av > m) { m = av; sv = v; lb = L; }
                    }
                    best[j] = sv * zs; bl[j] = (float)lb;
                }
                for (j = ml; j < ml + nbi; j++) {
                    /* local max among same-polarity neighbours only: a signal's tone-shifted
                     * alias (opposite sync polarity, ~75% CCF) must not suppress it */
                    int k, ok = 1, lb = (int)bl[j], flip;
                    float z = best[j] < 0 ? -best[j] : best[j], sg, fo = 0.f, lo = 0.f;
                    if (z < ZMIN) continue;
                    sg = best[j] < 0 ? -1.f : 1.f;
                    for (k = -2; k <= 2 && ok; k++)
                        if (k && j + k >= 0 && j + k < nb) {
                            float zn = sg * best[j + k];
                            if (zn > z || (zn == z && k < 0)) ok = 0;
                        }
                    if (!ok) continue;
                    flip = sg < 0;
                    if (j > 0 && j < nb - 1) {
                        float ym = sg * ccf[lb * nb + j - 1], y0 = sg * ccf[lb * nb + j], yp = sg * ccf[lb * nb + j + 1];
                        float den = ym - 2.f * y0 + yp;
                        if (den < 0.f) { fo = 0.5f * (ym - yp) / den; if (fo > 0.5f) fo = 0.5f; if (fo < -0.5f) fo = -0.5f; }
                    }
                    if (lb > 0 && lb < c->nl - 1) {
                        float ym = sg * ccf[(lb - 1) * nb + j], y0 = sg * ccf[lb * nb + j], yp = sg * ccf[(lb + 1) * nb + j];
                        float den = ym - 2.f * y0 + yp;
                        if (den < 0.f) { lo = 0.5f * (ym - yp) / den; if (lo > 0.5f) lo = 0.5f; if (lo < -0.5f) lo = -0.5f; }
                    }
                    DBG("cand f=%.2f L=%d+%.2f z=%.2f flip=%d\n", (b0 + j + fo) * c->df, lb, lo, z, flip);
                    cand_insert(c, ((float)(b0 + j) + fo) * c->df, c->t0 + ((float)lb + lo) * c->hs, z, flip);
                }
            }
        }
    }
    return 0;
}

/* ---- stage 2 ------------------------------------------------------------------------------ */

typedef struct {                     /* per candidate, ~20.7 KiB */
    float wr[NMIX], wi[NMIX], pr[NMIX], pi[NMIX], ar[NMIX], ai[NMIX];
    float cum[RING][2 * NMIX];       /* prefix sums at block ends: re x NMIX, im x NMIX */
    float soft[NTR][NSYM];
    float nacc[NTR], eacc[NTR], psacc[NTR];
    int32_t s0;                      /* first sample of block 0 */
    int nblk, i0, off;               /* block k = global step i0+k, sample offset off in it */
    int nexti[NDT];
} cst_t;

typedef struct {                     /* decode scratch */
    fano232_work_t fw;
    int8_t sym[NSYM];
    uint8_t j0[NSYM];
    uint8_t cb[NSYM];
    uint8_t data[13];
} dsc_t;

static void make_j0(uint8_t *j0)
{
    int i, k = 0;
    for (i = 0; i < 256; i++) {
        int n = 0, m;
        for (m = 0; m < 8; m++) n |= ((i >> m) & 1) << (7 - m);
        if (n <= 205) j0[k++] = (uint8_t)n;
    }
}

/* one block of B samples through the NMIX continuous-phase mixers; prefix sums -> cum[slot] */
static void mix_block(cst_t *w, const float *x, int B, int slot)
{
    float *cum = w->cum[slot];
    int k, n;
#if JT4_NEON
    const float32x4_t c15 = vdupq_n_f32(1.5f), c05 = vdupq_n_f32(0.5f);
    for (k = 0; k + 8 <= NMIX; k += 8) {       /* two vectors per pass over the block */
        float32x4_t r = vld1q_f32(w->pr + k), im = vld1q_f32(w->pi + k);
        float32x4_t ar = vld1q_f32(w->ar + k), ai = vld1q_f32(w->ai + k), g;
        float32x4_t r2 = vld1q_f32(w->pr + k + 4), im2 = vld1q_f32(w->pi + k + 4);
        float32x4_t ar2 = vld1q_f32(w->ar + k + 4), ai2 = vld1q_f32(w->ai + k + 4);
        const float32x4_t cr = vld1q_f32(w->wr + k), ci = vld1q_f32(w->wi + k);
        const float32x4_t cr2 = vld1q_f32(w->wr + k + 4), ci2 = vld1q_f32(w->wi + k + 4);
        for (n = 0; n < B; n++) {
            const float32x4_t xv = vld1q_dup_f32(x + n);
            const float32x4_t nr = vmlsq_f32(vmulq_f32(r, cr), im, ci);
            const float32x4_t nr2 = vmlsq_f32(vmulq_f32(r2, cr2), im2, ci2);
            ar = vmlaq_f32(ar, xv, r); ai = vmlaq_f32(ai, xv, im);
            ar2 = vmlaq_f32(ar2, xv, r2); ai2 = vmlaq_f32(ai2, xv, im2);
            im = vmlaq_f32(vmulq_f32(r, ci), im, cr);
            im2 = vmlaq_f32(vmulq_f32(r2, ci2), im2, cr2);
            r = nr; r2 = nr2;
        }
        g = vmlsq_f32(c15, c05, vmlaq_f32(vmulq_f32(r, r), im, im));      /* keep |p| = 1 */
        vst1q_f32(w->pr + k, vmulq_f32(r, g)); vst1q_f32(w->pi + k, vmulq_f32(im, g));
        vst1q_f32(w->ar + k, ar); vst1q_f32(w->ai + k, ai);
        vst1q_f32(cum + k, ar); vst1q_f32(cum + NMIX + k, ai);
        g = vmlsq_f32(c15, c05, vmlaq_f32(vmulq_f32(r2, r2), im2, im2));
        vst1q_f32(w->pr + k + 4, vmulq_f32(r2, g)); vst1q_f32(w->pi + k + 4, vmulq_f32(im2, g));
        vst1q_f32(w->ar + k + 4, ar2); vst1q_f32(w->ai + k + 4, ai2);
        vst1q_f32(cum + k + 4, ar2); vst1q_f32(cum + NMIX + k + 4, ai2);
    }
    for (; k < NMIX; k += 4) {
        float32x4_t r = vld1q_f32(w->pr + k), im = vld1q_f32(w->pi + k);
        float32x4_t ar = vld1q_f32(w->ar + k), ai = vld1q_f32(w->ai + k), g;
        const float32x4_t cr = vld1q_f32(w->wr + k), ci = vld1q_f32(w->wi + k);
        for (n = 0; n < B; n++) {
            const float32x4_t xv = vld1q_dup_f32(x + n);
            const float32x4_t nr = vmlsq_f32(vmulq_f32(r, cr), im, ci);
            ar = vmlaq_f32(ar, xv, r); ai = vmlaq_f32(ai, xv, im);
            im = vmlaq_f32(vmulq_f32(r, ci), im, cr);
            r = nr;
        }
        g = vmlsq_f32(c15, c05, vmlaq_f32(vmulq_f32(r, r), im, im));      /* keep |p| = 1 */
        vst1q_f32(w->pr + k, vmulq_f32(r, g)); vst1q_f32(w->pi + k, vmulq_f32(im, g));
        vst1q_f32(w->ar + k, ar); vst1q_f32(w->ai + k, ai);
        vst1q_f32(cum + k, ar); vst1q_f32(cum + NMIX + k, ai);
    }
#else
    for (k = 0; k < NMIX; k++) {
        float ar = w->ar[k], ai = w->ai[k], r = w->pr[k], im = w->pi[k];
        const float cr = w->wr[k], ci = w->wi[k];
        for (n = 0; n < B; n++) {
            float nr;
            ar += x[n] * r; ai += x[n] * im;
            nr = r * cr - im * ci; im = r * ci + im * cr; r = nr;
        }
        {   /* keep |p| = 1 */
            float g = 1.5f - 0.5f * (r * r + im * im);
            w->pr[k] = r * g; w->pi[k] = im * g;
        }
        w->ar[k] = ar; w->ai[k] = ai;
        cum[k] = ar; cum[NMIX + k] = ai;
    }
#endif
}

/* 3a: coherent extraction on the dt x df grid for n candidates in one sequential read */
static int extract(ctx_t *c, const cand_t *const *cds, cst_t *ws, int n)
{
    const int B = (int)(c->fs / 200.f + 0.5f);              /* 5 ms blocks */
    const int dstep = (int)(DTSTEP * c->fs / (float)B + 0.5f) * B;
    int32_t gmin = 0;
    int q, k, m, t, i, nsteps = 0;
    for (q = 0; q < n; q++) {
        const cand_t *cd = cds[q];
        cst_t *w = ws + q;
        w->s0 = (int32_t)(cd->t - (float)(DT0 * dstep)) - B;
        w->nblk = (int)(((float)NSYM * c->nsps + (float)(2 * DT0 * dstep) + 3.f * (float)B) / (float)B) + 1;
        for (k = 0; k < NMIX; k++) {
            double sn, cs;
            float f = cd->f + (float)(k / 4 - DF0) * DFSTEP + (float)(k & 3) * (float)c->mode4 * BAUD;
            dg_sincos(-6.283185307179586 * (double)f / (double)c->fs, &sn, &cs);
            w->wr[k] = (float)cs; w->wi[k] = (float)sn;
            w->pr[k] = 1.f; w->pi[k] = 0.f;
            w->ar[k] = w->ai[k] = 0.f;
            w->cum[0][k] = w->cum[0][NMIX + k] = 0.f;
        }
        for (k = 0; k < NTR; k++) { w->nacc[k] = 0.f; w->eacc[k] = 0.f; w->psacc[k] = 0.f; }
        for (k = 0; k < NDT; k++) w->nexti[k] = 0;
        if (q == 0 || w->s0 < gmin) gmin = w->s0;
    }
    for (q = 0; q < n; q++) {
        cst_t *w = ws + q;
        w->i0 = (int)((w->s0 - gmin) / B); w->off = (int)((w->s0 - gmin) % B);
        if (w->i0 + w->nblk > nsteps) nsteps = w->i0 + w->nblk;
    }
    for (i = 0; i < nsteps; i++) {
        const float *x;
        if ((i & 31) == 0 && aborted(c)) return JT4_ERR_ABORT;
        x = dg_rdr_span(&c->rd, gmin + i * B, (uint32_t)(2 * B));
        if (!x) return JT4_ERR_IO;
        for (q = 0; q < n; q++) {
            const cand_t *cd = cds[q];
            cst_t *w = ws + q;
            const int blk = i - w->i0;
            if (blk < 0 || blk >= w->nblk) continue;
            mix_block(w, x + w->off, B, (blk + 1) % RING);
            /* windows completed by this block */
            for (k = 0; k < NDT; k++) {
                while (w->nexti[k] < NSYM) {
                    int si = w->nexti[k], tr;
                    float st = cd->t + (float)((k - DT0) * dstep) + (float)si * c->nsps - (float)w->s0;
                    int wb = (int)(st / (float)B + 0.5f), we = (int)((st + c->nsps) / (float)B + 0.5f);
                    int sb = npr[si + 1] ^ cd->flip;
                    const float *e, *s;
                    if (we > blk + 1) break;
                    e = w->cum[we % RING]; s = w->cum[wb % RING];
                    for (m = 0; m < NDF; m++) {
                        float P[4];
                        for (t = 0; t < 4; t++) {
                            float dr = e[m * 4 + t] - s[m * 4 + t], di = e[NMIX + m * 4 + t] - s[NMIX + m * 4 + t];
                            P[t] = dr * dr + di * di;
                        }
                        tr = k * NDF + m;
                        w->soft[tr][si] = P[2 + sb] - P[sb];
                        w->psacc[tr] += P[2 + sb] + P[sb];
                        w->nacc[tr] += P[1 - sb] + P[3 - sb];
                        w->eacc[tr] += P[2 + sb] + P[sb] - P[1 - sb] - P[3 - sb];
                    }
                    w->nexti[k]++;
                }
            }
        }
    }
    return 0;
}

/* 3b: Fano on the best trials of one extracted candidate. returns 1 decoded, 0 not, <0 error */
static int try_candidate(ctx_t *c, const cand_t *cd, cst_t *w, dsc_t *d, jt4_result_t *res, float nominal, int *nfail)
{
    const int B = (int)(c->fs / 200.f + 0.5f);
    const int dstep = (int)(DTSTEP * c->fs / (float)B + 0.5f) * B;
    int k, tr, order[NTR], ntr;
    /* order trials by in-parity energy */
    for (k = 0; k < NTR; k++) order[k] = k;
    for (k = 1; k < NTR; k++) {
        int v = order[k], j = k;
        while (j > 0 && w->eacc[order[j - 1]] < w->eacc[v]) { order[j] = order[j - 1]; j--; }
        order[j] = v;
    }
    ntr = NTRY < NTR ? NTRY : NTR;
    for (k = 0; k < ntr; k++) {
        float mean = 0.f, sq = 0.f, rms, sc;
        int i, rc;
        uint32_t ncyc;
        tr = order[k];
        for (i = 0; i < NSYM; i++) mean += w->soft[tr][i];
        mean /= (float)NSYM;
        for (i = 0; i < NSYM; i++) { float dd = w->soft[tr][i] - mean; sq += dd * dd; }
        rms = dg_sqrtf(sq / (float)NSYM);
        if (!(rms > 0.f)) continue;
        sc = 30.f / rms;
        for (i = 0; i < NSYM; i++) {         /* deinterleave while quantising */
            int v = dg_iround((w->soft[tr][d->j0[i]] - mean) * sc);
            if (v < -127) v = -127;
            if (v > 127) v = 127;
            d->sym[i] = (int8_t)v;
        }
        if (*nfail >= MAXFAIL) return 0;
        rc = fano232(d->sym, 72 + 31, FANO_LIMIT, d->data, &d->fw, c->abort, &ncyc);
        if (rc == -2) return JT4_ERR_ABORT;
        if (rc != 0) (*nfail)++;
        DBG("  rank %d trial %d (dt%+d df%+d) E=%.3g fano=%d cyc=%u\n", k, tr, tr / NDF - DT0, tr % NDF - DF0, w->eacc[tr], rc, ncyc);
        if (rc == 0) {
            char msg[23];
            if (jt72_unpack(d->data, msg) != 0 || !msg[0]) continue;
            {
                int q;
                float ps = 0.f, nn, sig, snr;
                uint8_t cbits[NSYM];
                for (q = 9; q < 13; q++) d->data[q] = 0;
                conv232_encode(d->data, 72 + 31, cbits);
                for (i = 0; i < NSYM; i++) d->cb[d->j0[i]] = cbits[i];  /* interleave */
                /* mean power of the decoded tones: sum of 0.5*(psum +- soft) */
                for (i = 0; i < NSYM; i++) ps += (d->cb[i] ? 0.5f : -0.5f) * w->soft[tr][i];
                ps = (ps + 0.5f * w->psacc[tr]) / (float)NSYM;
                nn = w->nacc[tr] / (float)(2 * NSYM);
                sig = ps - nn;
                if (sig < 1e-3f * nn) sig = 1e-3f * nn;
                snr = nn > 0.f ? 10.f * dg_log10f(sig / nn * BAUD / 2500.f) : 0.f;
                for (q = 0; q < 23; q++) res->msg[q] = msg[q];
                res->msg[23] = 0;
                res->freq_hz = cd->f + (float)(tr % NDF - DF0) * DFSTEP;
                res->dt_s = (cd->t + (float)((tr / NDF - DT0) * dstep)) / c->fs - nominal;
                res->snr_db = snr;
                res->quality = dg_iround(cd->z * 10.f);
            }
            return 1;
        }
    }
    return 0;
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int jt4_decode(const dsrc_t *src, int submode, float f_lo, float f_hi, float nominal_start_s,
               jt4_result_t *out, int maxout, void *work, uint32_t work_len, int (*should_abort)(void))
{
    ctx_t c;
    float *w0, *wp, *rdbuf, *abuf, *ccf;
    uint32_t nflt, used;
    int blo, bhi, fper, rc, nout = 0, i;

    if (!src || !src->read || !out || maxout <= 0 || !work || submode < 0 || submode > 6) return JT4_ERR_ARGS;
    if (work_len < JT4_WORK_MIN || src->fs < 6000 || src->fs > 8960) return JT4_ERR_ARGS;
    dg_zero(&c, sizeof c);
    c.src = src;
    c.abort = should_abort;
    c.fs = (float)src->fs;
    c.mode4 = nch[submode];
    c.nsps = c.fs / BAUD;
    c.nspsi = (int)(c.nsps + 0.5f);
    c.hs = 0.5f * c.nsps;
    c.t0 = (nominal_start_s + JT4_DT_LO) * c.fs;
    c.nl = (int)((JT4_DT_HI - JT4_DT_LO) * c.fs / c.hs) + 1;
    c.ns = c.nl + 2 * (NSYM - 1);
    c.nfft = c.mode4 == 1 ? JT4_NFFT_A : JT4_NFFT_WIDE;
    c.nh = c.nfft / 2;
    if (c.nspsi > c.nfft) return JT4_ERR_ARGS;
    c.df = c.fs / (float)c.nfft;
    for (i = 0; i < 4; i++) c.toff[i] = dg_iround((float)i * (float)c.mode4 * BAUD / c.df);

    blo = (int)(f_lo / c.df + 0.999f);
    bhi = (int)(f_hi / c.df);
    if (blo < 10) blo = 10;
    if (bhi > c.nh - 10 - c.toff[3]) bhi = c.nh - 10 - c.toff[3];
    if (bhi < blo) return JT4_ERR_ARGS;

    /* work layout (floats, 8-byte aligned): the reader and the FFT set (hot, small) first */
    w0 = wp = (float *)(((uintptr_t)work + 7u) & ~(uintptr_t)7u);
    nflt = (work_len - (uint32_t)((uintptr_t)wp - (uintptr_t)work)) / 4u;
    rdbuf = wp; wp += RDCAP;
    c.f.n = c.nfft; c.f.m = c.nh;
    c.f.z = wp; wp += c.nfft;
    c.f.twr = wp; wp += c.nh - 1;
    c.f.twi = wp; wp += c.nh - 1;
    c.f.ur = wp; wp += c.nfft / 4 + 1;
    c.f.ui = wp; wp += c.nfft / 4 + 1;
    c.f.rev = (uint16_t *)(void *)wp; wp += c.nh / 2;
    c.pw = wp; wp += c.nh + 2;
    c.inv = wp; wp += c.nh + 2;
    used = (uint32_t)(wp - w0);
    if (RDCAP + (sizeof(dsc_t) + sizeof(cst_t)) / 4 + 8 > nflt) return JT4_ERR_ARGS;   /* stage 2 reuses all but rdbuf */
    if (used + (16u + 2u * MARG) * (uint32_t)(c.nl + KBUF) > nflt) return JT4_ERR_ARGS;
    fper = (int)((nflt - used) / (uint32_t)(c.nl + KBUF));
    if (fper > c.nh - 2) fper = c.nh - 2;
    abuf = wp; ccf = wp + KBUF * fper;
    DBG("bins %d..%d (%d), fper %d -> %d pass(es), nl %d ns %d\n", blo, bhi, bhi - blo + 1, fper,
        fper >= bhi - blo + 1 ? 1 : (bhi - blo) / (fper - 2 * MARG) + 1, c.nl, c.ns);

    dg_rdr_init(&c.rd, src, rdbuf, RDCAP);
    fft_setup(&c.f, c.f.z);
    rc = sync_search(&c, blo, bhi, abuf, ccf, fper);
    if (rc) return rc;

    {
        /* stage 2 reuses everything after the reader buffer */
        dsc_t *d = (dsc_t *)(void *)(((uintptr_t)(rdbuf + RDCAP) + 7u) & ~(uintptr_t)7u);
        cst_t *cs = (cst_t *)(void *)(((uintptr_t)(d + 1) + 7u) & ~(uintptr_t)7u);
        int cap = (int)(((uintptr_t)work + work_len - (uintptr_t)cs) / sizeof(cst_t));
        float dec_f[MAXC], dec_t[MAXC];
        int ndec = 0, nfail = 0, stop = 0;
        if (cap > MAXC) cap = MAXC;
        make_j0(d->j0);
        fano232_init(&d->fw);
        /* A JT4 signal also syncs with opposite polarity (~75% CCF) one tone spacing below and
         * above, and those aliases again one further out: a chain f+n*sp with alternating
         * polarity. Try first the candidates deepest inside such a chain (z order otherwise). */
        {
            float sp = (float)c.mode4 * BAUD;
            int dep[MAXC], k, j;
            for (k = 0; k < c.ncand; k++) {
                int side, dd[2];
                for (side = 0; side < 2; side++) {
                    int cur = k, n = 0;
                    for (;;) {
                        int nx = -1;
                        for (j = 0; j < c.ncand; j++) {
                            float df = c.cand[j].f - c.cand[cur].f - (side ? sp : -sp);
                            float dt = c.cand[j].t - c.cand[cur].t;
                            if (c.cand[j].flip != c.cand[cur].flip && df < 1.5f * c.df && df > -1.5f * c.df &&
                                dt < c.hs && dt > -c.hs) { nx = j; break; }
                        }
                        if (nx < 0 || n >= MAXC) break;
                        cur = nx; n++;
                    }
                    dd[side] = n;
                }
                dep[k] = dd[0] < dd[1] ? dd[0] : dd[1];
            }
            for (k = 1; k < c.ncand; k++) {      /* stable sort by depth, z order kept */
                cand_t v = c.cand[k];
                int dv = dep[k];
                j = k;
                while (j > 0 && dep[j - 1] < dv) { c.cand[j] = c.cand[j - 1]; dep[j] = dep[j - 1]; j--; }
                c.cand[j] = v; dep[j] = dv;
            }
        }
        /* candidates in try order, as many per SD pass as fit; a candidate is skipped when it is a
         * sidelobe / tone-shifted alias of a signal already decoded (checked again at its turn) */
        i = 0;
        while (i < c.ncand && nout < maxout && !stop) {
            const cand_t *grp[MAXC];
            int n = 0, g, k;
            for (; i < c.ncand && n < cap; i++) {
                int near = 0;
                for (k = 0; k < ndec; k++) {
                    float dfr = c.cand[i].f - dec_f[k], dtt = c.cand[i].t - dec_t[k];
                    if (dfr < 0) dfr = -dfr;
                    if (dtt < 0) dtt = -dtt;
                    if (dfr <= 3.f * (float)c.mode4 * BAUD + 3.f * c.df && dtt < 2.f * c.nsps) near = 1;
                }
                if (!near) grp[n++] = &c.cand[i];
            }
            if (!n || nfail >= MAXFAIL) break;
            rc = extract(&c, grp, cs, n);
            if (rc < 0) return nout ? nout : rc;
            for (g = 0; g < n && nout < maxout; g++) {
                jt4_result_t r;
                int dup = 0, near = 0;
                for (k = 0; k < ndec; k++) {
                    float dfr = grp[g]->f - dec_f[k], dtt = grp[g]->t - dec_t[k];
                    if (dfr < 0) dfr = -dfr;
                    if (dtt < 0) dtt = -dtt;
                    if (dfr <= 3.f * (float)c.mode4 * BAUD + 3.f * c.df && dtt < 2.f * c.nsps) near = 1;
                }
                if (near) continue;
                DBG("try cand f=%.2f t=%.3f z=%.2f flip=%d\n", grp[g]->f, grp[g]->t / c.fs, grp[g]->z, grp[g]->flip);
                if (nfail >= MAXFAIL) { stop = 1; break; }
                rc = try_candidate(&c, grp[g], cs + g, d, &r, nominal_start_s, &nfail);
                if (rc < 0) return nout ? nout : rc;
                if (rc != 1) continue;
                if (ndec < MAXC) { dec_f[ndec] = grp[g]->f; dec_t[ndec] = grp[g]->t; ndec++; }
                for (k = 0; k < nout; k++) if (streq(out[k].msg, r.msg)) dup = 1;
                if (!dup) out[nout++] = r;
            }
        }
    }
    return nout;
}
