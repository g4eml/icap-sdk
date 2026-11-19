/* SPDX-License-Identifier: GPL-3.0-or-later
 * pi4dec.c - PI4 beacon decoder, portable freestanding C (no libc, no malloc).
 *
 * Spec: https://rudius.net/oz2m/ngnb/pi4.htm (OZ2M / DJ5HG).
 * Build (radio): arm-none-eabi-gcc -O2 -ffreestanding -nostdlib -fno-math-errno
 *                -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard
 *
 * Speed (Cortex-A9): one FFT pass over the file for the whole search band (the lag x bin table
 * takes the work area; candidate statistics still per PI4_STATBINS-bin block), then all candidates
 * that fit the work area are mixed down in one more pass (two for 12 candidates in 240 KiB).
 * FFT butterflies, tone mixers and the refinement grid run 4 lanes per NEON step; the scalar code
 * (x86 host, -DPI4_NO_NEON) performs the same operations in the same order, so results are the
 * same on both (no fused multiply-add: VFPv3 / NEON VMLA and VMLS round the product).
 *
 * Method
 *  1. Coarse search: one-symbol rectangular windows (1/6 s, zero-padded to a
 *     2048-point FFT; two real windows per complex FFT) every 1/4 symbol over
 *     start = nominal-1 .. nominal+2 s. Per window and tone-0 bin b the
 *     sync observable D = (P1+P3)-(P0+P2) (the sync bit is the LSB of each
 *     symbol) is accumulated directly into a lag x bin correlation table
 *     S[lag][b] = sum_k sign(sync_k) * D(lag + 4k, b). Local maxima -> candidates.
 *  2. Refinement per candidate: the audio is mixed down at the 4 tone
 *     frequencies and integrated in 1/8-symbol sub-blocks; per-symbol DFTs at
 *     +-2.5 Hz (0.25 Hz steps) and +-1/4 symbol (1/8 steps) are formed from
 *     the sub-blocks, maximising the sync correlation.
 *  3. Soft bits: only the 2 tones allowed by the known sync bit carry data;
 *     the other 2 give the local noise level. Exact noncoherent LLR
 *     ln I0(2 sqrt(Es p1)) - ln I0(2 sqrt(Es p0)).
 *  4. Fano sequential decoding of the K=32 r=1/2 code (42 info + 31 tail bits).
 *  5. Gate: re-encode, soft correlation z-score with the received symbols.
 */
#include "pi4dec.h"
#if defined(__ARM_NEON) && !defined(PI4_NO_NEON)
#include <arm_neon.h>
#define PI4_NEON 1
#else
#define PI4_NEON 0
#endif
#ifdef PI4_DEBUG
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
#define DBG(...) do { } while (0)
#endif

/* ------------------------------------------------------------------ */
/* constants from the spec                                             */

#define NSYM      146
#define NBITS     42
#define NNODE     73          /* 42 + 31 tail */
#define POLY1     0xF2D05351u
#define POLY2     0xE4613C47u
#define TONESP    234.375f    /* 40 * 12000 / 2048 */
#define SYMRATE   6.0f        /* 12000 / 2000 */

static const char pi4_chars[38] = {
    '0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F','G','H','I',
    'J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z',' ','/' };

static const uint8_t pi4_sync[NSYM] = {
    0,0,1,0,0,1,1,1,1,0,1,0,1,0,1,0,0,1,0,0,0,1,0,0,0,1,1,0,0,1,
    1,1,1,0,0,1,1,1,1,1,0,0,1,1,0,1,1,1,1,0,1,0,1,1,0,1,1,0,1,0,
    0,0,0,0,1,1,1,1,1,0,1,0,1,0,0,0,0,0,1,1,1,1,1,0,1,0,0,1,0,0,
    1,0,1,0,0,0,0,1,0,0,1,1,0,0,0,0,0,1,1,0,0,0,0,1,1,0,0,1,1,1,
    0,1,1,1,0,1,1,0,1,0,1,0,1,0,0,0,0,1,1,1,0,0,0,0,1,1 };

/* ------------------------------------------------------------------ */
/* tunables                                                            */

#define NFFT      2048
#define LOGNFFT   11
#define STATBINS  PI4_STATBINS /* candidate statistics (mean, sd, local maxima) per block of this many bins */
#ifndef NCMAX
#define NCMAX 12          /* candidates kept */
#endif
#ifndef CAND_Z
#define CAND_Z 3.0f        /* min coarse z to try */
#endif
#define NSUBSYM   8           /* sub-blocks per symbol in refinement */
#define NSHIFT    5           /* refinement time shifts (1/8 symbol) */
#define NSUB      (NSYM * NSUBSYM + NSHIFT - 1)
#define NDF       21          /* refinement freq steps of 0.25 Hz: -2.5..+2.5 */
#ifndef FANO_DELTA
#define FANO_DELTA 4.0f
#endif
#ifndef FANO_BIAS
#define FANO_BIAS 0.5f
#endif
#ifndef FANO_CYCLES
#define FANO_CYCLES (NNODE * 8000u)
#endif
#ifndef GATE_Z
#define GATE_Z 6.0f
#endif
#ifndef LLR_MAX
#define LLR_MAX 30.0f
#endif
#ifndef NOISE_W
#define NOISE_W 10       /* +- symbols for local noise estimate */
#endif

/* ------------------------------------------------------------------ */
/* freestanding helpers                                                */

__attribute__((weak, optimize("no-tree-loop-distribute-patterns"))) void *memset(void *d, int c, __SIZE_TYPE__ n)
{
    uint8_t *p = (uint8_t *)d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}
__attribute__((weak, optimize("no-tree-loop-distribute-patterns"))) void *memcpy(void *d, const void *s, __SIZE_TYPE__ n)
{
    uint8_t *p = (uint8_t *)d; const uint8_t *q = (const uint8_t *)s;
    while (n--) *p++ = *q++;
    return d;
}

static void fzero(float *p, int32_t n) { while (n-- > 0) *p++ = 0.0f; }

static float pi4_sqrtf(float x) { return x > 0.0f ? __builtin_sqrtf(x) : 0.0f; }

static float pi4_logf(float x)
{
    union { float f; uint32_t u; } v;
    int e; float m, s, s2;
    if (!(x > 0.0f)) return -1e30f;
    v.f = x;
    e = (int)((v.u >> 23) & 255u) - 127;
    v.u = (v.u & 0x7fffffu) | 0x3f800000u;
    m = v.f;
    if (m > 1.41421356f) { m *= 0.5f; e++; }
    s = (m - 1.0f) / (m + 1.0f); s2 = s * s;
    return 2.0f * s * (1.0f + s2 * (0.33333333f + s2 * (0.2f + s2 * (0.14285714f + s2 * 0.11111111f))))
           + (float)e * 0.69314718f;
}

static float pi4_expf(float x)
{
    union { float f; uint32_t u; } v;
    float y, f, g, p; int n;
    if (x < -87.0f) return 0.0f;
    if (x > 88.0f) x = 88.0f;
    y = x * 1.44269504f;
    n = (int)y; if (y < (float)n) n--;
    f = y - (float)n;
    g = f * 0.69314718f;
    p = 1.0f + g * (1.0f + g * (0.5f + g * (0.16666667f + g * (0.041666667f + g * (0.0083333333f + g * (0.0013888889f + g * 0.00019841270f))))));
    v.u = (uint32_t)(n + 127) << 23;
    return p * v.f;
}

/* ln(1 + e^x) */
static float softplus(float x)
{
    if (x > 20.0f) return x;
    if (x < -20.0f) return pi4_expf(x);
    return pi4_logf(1.0f + pi4_expf(x));
}

/* sin/cos in double, |x| any (range-reduced to +-pi/4) */
static void pi4_sincos(double x, float *s, float *c)
{
    const double pio2 = 1.57079632679489661923;
    double q = x * (1.0 / pio2), r, r2, sn, cs;
    int32_t n = (int32_t)(q >= 0 ? q + 0.5 : q - 0.5);
    r = x - (double)n * pio2; r2 = r * r;
    sn = r * (1 - r2 / 6 * (1 - r2 / 20 * (1 - r2 / 42 * (1 - r2 / 72 * (1 - r2 / 110 * (1 - r2 / 156))))));
    cs = 1 - r2 / 2 * (1 - r2 / 12 * (1 - r2 / 30 * (1 - r2 / 56 * (1 - r2 / 90 * (1 - r2 / 132)))));
    switch (n & 3) {
    case 0: *s = (float)sn;  *c = (float)cs;  break;
    case 1: *s = (float)cs;  *c = (float)-sn; break;
    case 2: *s = (float)-sn; *c = (float)-cs; break;
    default:*s = (float)-cs; *c = (float)sn;  break;
    }
}

/* ln I0(x), Abramowitz & Stegun 9.8.1 / 9.8.2 */
static float lni0(float x)
{
    float t, p;
    if (x < 3.75f) {
        t = x * (1.0f / 3.75f); t *= t;
        p = 1.0f + t * (3.5156229f + t * (3.0899424f + t * (1.2067492f + t * (0.2659732f + t * (0.0360768f + t * 0.0045813f)))));
        return pi4_logf(p);
    }
    t = 3.75f / x;
    p = 0.39894228f + t * (0.01328592f + t * (0.00225319f + t * (-0.00157565f + t * (0.00916281f
        + t * (-0.02057706f + t * (0.02635537f + t * (-0.01647633f + t * 0.00392377f)))))));
    return x - 0.5f * pi4_logf(x) + pi4_logf(p);
}

static uint32_t parity32(uint32_t v)
{
    v ^= v >> 16; v ^= v >> 8; v ^= v >> 4;
    return (0x6996u >> (v & 15u)) & 1u;
}

static int32_t iround(float x) { return (int32_t)(x >= 0.0f ? x + 0.5f : x - 0.5f); }

/* ------------------------------------------------------------------ */
/* spec: packing, convolutional code, interleaver                      */

/* p-th convolutional output bit goes to channel symbol perm[p] */
static void make_perm(uint8_t perm[NSYM])
{
    int i, b, p = 0;
    for (i = 0; i < 256 && p < NSYM; i++) {
        int r = 0;
        for (b = 0; b < 8; b++) if (i & (1 << b)) r |= 1 << (7 - b);
        if (r < NSYM) perm[p++] = (uint8_t)r;
    }
}

/* message value (42 bits as hi:lo, hi = upper 10 bits) -> 146 coded bits */
static void conv_encode(uint32_t hi, uint32_t lo, uint8_t cb[NSYM])
{
    uint32_t n = 0; int j;
    for (j = 0; j < NNODE; j++) {
        uint32_t bit = 0;
        if (j < NBITS) bit = (j < 10) ? (hi >> (9 - j)) & 1u : (lo >> (31 - (j - 10))) & 1u;
        n = (n << 1) | bit;
        cb[2 * j]     = (uint8_t)parity32(n & POLY1);
        cb[2 * j + 1] = (uint8_t)parity32(n & POLY2);
    }
}

static int char_index(char ch)
{
    int i;
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    for (i = 0; i < 38; i++) if (pi4_chars[i] == ch) return i;
    return 36;  /* space */
}

int pi4_encode(const char *msg, uint8_t sym[PI4_NSYM])
{
    uint32_t hi = 0, lo = 0;   /* 42-bit value as (hi:10, lo:32) */
    uint8_t cb[NSYM], perm[NSYM];
    int i, end = 0;
    for (i = 0; i < 8; i++) {
        int c = 36;
        if (!end && msg[i]) c = char_index(msg[i]); else end = 1;
        /* N = N*38 + c in 64-bit via 32-bit halves (lo carries into hi) */
        {
            uint32_t l0 = lo & 0xffffu, l1 = lo >> 16;
            uint32_t a = l0 * 38u + (uint32_t)c;
            uint32_t b = l1 * 38u + (a >> 16);
            lo = (a & 0xffffu) | (b << 16);
            hi = hi * 38u + (b >> 16);
        }
    }
    conv_encode(hi, lo, cb);
    make_perm(perm);
    for (i = 0; i < NSYM; i++) sym[perm[i]] = (uint8_t)(cb[i] << 1);
    for (i = 0; i < NSYM; i++) sym[i] = (uint8_t)(sym[i] | pi4_sync[i]);
    return 0;
}

/* 42 bits (MSB first) -> text; returns 0 if out of range */
static int unpack(const uint8_t *bits, char *out)
{
    /* value in three 16-bit limbs (48 bits) */
    uint32_t limb[3] = { 0, 0, 0 };
    int i, j;
    for (i = 0; i < NBITS; i++) {
        int pos = NBITS - 1 - i;           /* bit position */
        if (bits[i]) limb[2 - (pos >> 4)] |= 1u << (pos & 15);
    }
    /* 38^8 = 4347792138496 = 0x3F4_5A2E_A400 (limb[0]=0x3F4,limb1=0x5A2E? -> check by compare) */
    {
        /* compare value < 38^8 : 38^8 = 0x000003F45A2EA400? computed below */
        uint32_t m0 = 0, m1 = 0, m2 = 1;     /* 38^8 in limbs, computed */
        for (j = 0; j < 8; j++) {
            uint32_t a = m2 * 38u, b = m1 * 38u + (a >> 16), c = m0 * 38u + (b >> 16);
            m2 = a & 0xffffu; m1 = b & 0xffffu; m0 = c;
        }
        if (limb[0] > m0 || (limb[0] == m0 && (limb[1] > m1 || (limb[1] == m1 && limb[2] >= m2))))
            return 0;
    }
    for (j = 7; j >= 0; j--) {          /* divide by 38, remainder -> char */
        uint32_t r = 0, cur;
        for (i = 0; i < 3; i++) {
            cur = (r << 16) | limb[i];
            limb[i] = (uint32_t)(((uint64_t)cur * 3616814566u) >> 37);   /* cur / 38 without libgcc */
            r = cur - limb[i] * 38u;
        }
        out[j] = pi4_chars[r];
    }
    out[8] = 0;
    for (j = 7; j >= 0 && out[j] == ' '; j--) out[j] = 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Fano sequential decoder (after Phil Karn, KA9Q)                     */

typedef struct {
    uint32_t enc;                 /* encoder state with the branch currently taken */
    float gamma;                  /* path metric up to (not including) this node's branch */
    uint32_t sel;                 /* symbol pair of the 0-branch | 0x100 second branch in use | 0x200 tail node */
    uint32_t flips;               /* bit l: for symbol pair l the better branch is the 1-branch */
    float best[4], sec[4];        /* per symbol pair: metric of the better / the other branch */
} fnode_t;

static uint32_t encsym(uint32_t s) { return (parity32(s & POLY1) << 1) | parity32(s & POLY2); }

/* et[n][b]: encoder output (2 bits) of state byte b at byte position n */
static void make_et(uint8_t *et)
{
    uint32_t i;
    for (i = 0; i < 256; i++) {
        et[i] = (uint8_t)encsym(i); et[256 + i] = (uint8_t)encsym(i << 8);
        et[512 + i] = (uint8_t)encsym(i << 16); et[768 + i] = (uint8_t)encsym(i << 24);
    }
}

/* m[2*k + c]: metric of coded bit k (conv order) being c; nodes[-1 .. NNODE]. Returns 0 ok, 1 fail, -1 abort.
 * The search of the first version (same decisions, same cycle count) at a lower cost per cycle: branch
 * metrics sorted per symbol pair before the search, encoder output from byte tables, nodes[-1] a sentinel
 * instead of a "first node" test, abort poll outside the inner loop. */
static int fano(fnode_t *nodes, const float *m, uint8_t *bits, const uint8_t *et, int (*ab)(void))
{
    fnode_t *np, *const last_node = nodes + NNODE, *const tail = nodes + NBITS;
    float t = 0.0f, g, cur;
    uint32_t lsym, cyc;
    int k, i;
    for (k = 0; k < NNODE; k++) {
        float met[4];
        for (i = 0; i < 4; i++) met[i] = m[2 * (2 * k) + (i >> 1)] + m[2 * (2 * k + 1) + (i & 1)];
        nodes[k].flips = 0;
        for (i = 0; i < 4; i++) {
            float m0 = met[i], m1 = met[3 ^ i];        /* 0-branch sends pair i, 1-branch the complement */
            if (k >= NBITS || m0 > m1) { nodes[k].best[i] = m0; nodes[k].sec[i] = m1; }
            else { nodes[k].best[i] = m1; nodes[k].sec[i] = m0; nodes[k].flips |= 1u << i; }
        }
    }
    nodes[-1].gamma = -1e30f;            /* sentinel: "cannot back up" */
    np = nodes;
    lsym = encsym(0);
    np->sel = lsym;
    np->enc = (np->flips >> lsym) & 1u;
    np->gamma = 0.0f;
    g = 0.0f; cur = np->best[lsym];      /* g = np->gamma, cur = metric of the branch in use */
    for (cyc = 0; cyc < FANO_CYCLES;) {
        uint32_t last = cyc | 16383u, cnt;   /* run up to the next abort poll */
        if (last > FANO_CYCLES - 1) last = FANO_CYCLES - 1;
        if ((cyc & 16383u) == 0 && ab && ab()) return -1;
        for (cnt = last - cyc + 1; cnt; cnt--) {
            const float ngamma = g + cur;
            if (ngamma >= t) {
                uint32_t e;
                if (g < t + FANO_DELTA)
                    while (ngamma >= t + FANO_DELTA) t += FANO_DELTA;
                e = np->enc << 1;
                np++;
                np->gamma = ngamma;
                g = ngamma;
                if (np == last_node) {
                    np->enc = e;
                    for (k = 0; k < NBITS; k++) bits[k] = (uint8_t)(nodes[k].enc & 1u);
                    return 0;
                }
                lsym = et[e & 255u] ^ et[256 + ((e >> 8) & 255u)] ^ et[512 + ((e >> 16) & 255u)] ^ et[768 + (e >> 24)];
                cur = np->best[lsym];
                np->enc = e | ((np->flips >> lsym) & 1u);
                np->sel = np >= tail ? lsym | 0x200u : lsym;
                continue;
            }
            for (;;) {
                const float gp = np[-1].gamma;
                uint32_t sel;
                if (gp < t) {
                    t -= FANO_DELTA;
                    sel = np->sel;
                    if (sel & 0x100u) { sel &= 3u; np->sel = sel; np->enc ^= 1u; }
                    cur = np->best[sel & 3u];            /* np may be a tail node reached by backing up */
                    break;
                }
                np--;
                g = gp;
                sel = np->sel;
                if (sel < 0x100u) { np->sel = sel | 0x100u; np->enc ^= 1u; cur = np->sec[sel]; break; }
            }
        }
        cyc = last + 1;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* sample ring buffer over dsrc_t                                       */

typedef struct { const dsrc_t *src; float *buf; int32_t pos, fill, cap, err; } rb_t;

static const float *rb_get(rb_t *r, int32_t s, int32_t n)
{
    int32_t keep = 0, a, e, i;
    float *d;
    if (s >= r->pos && s + n <= r->pos + r->fill) return r->buf + (s - r->pos);
    if (s >= r->pos && s < r->pos + r->fill) {
        keep = r->pos + r->fill - s;
        for (i = 0; i < keep; i++) r->buf[i] = r->buf[s - r->pos + i];
    }
    r->pos = s; r->fill = keep;
    a = s + keep; e = s + r->cap; d = r->buf + keep;
    while (a < e) {
        if (a < 0) {
            int32_t z = (e < 0 ? e : 0) - a;
            fzero(d, z); d += z; a += z;
        } else if ((uint32_t)a >= r->src->len) {
            fzero(d, e - a); a = e;
        } else {
            uint32_t m = (uint32_t)(e - a);
            int got;
            if ((uint32_t)a + m > r->src->len) m = r->src->len - (uint32_t)a;
            got = r->err ? -1 : r->src->read(r->src->ctx, (uint32_t)a, d, m);
            if (got < 0) { r->err = 1; got = 0; }
            if ((uint32_t)got < m) fzero(d + got, (int32_t)m - got);
            d += m; a += (int32_t)m;
        }
    }
    r->fill = r->cap;
    return r->buf;
}

/* ------------------------------------------------------------------ */
/* FFT (radix-2, complex, in place, interleaved re/im)                 */

/* twiddles exp(-i*2*pi*k/NFFT): tc[k] = cos, ts[k] = -sin for k < NFFT/2 (the last stage reads them
 * in sequence, the two before with stride 2 and 4); sc/ss: the stages with half-length h = 4..128
 * packed at [h-4 .. 2h-5]. */
typedef struct { float *tc, *ts, *sc, *ss; } tw_t;
#define TW_FLOATS (NFFT + 2 * 252)

static void tw_setup(tw_t *w, float *mem)
{
    int i, h, k;
    w->tc = mem; w->ts = mem + NFFT / 2; w->sc = mem + NFFT; w->ss = mem + NFFT + 252;
    for (i = 0; i < NFFT / 2; i++) {
        float sn, cs;
        pi4_sincos(6.283185307179586 * (double)i / (double)NFFT, &sn, &cs);
        w->tc[i] = cs; w->ts[i] = -sn;
    }
    for (h = 4; h <= 128; h <<= 1)
        for (k = 0; k < h; k++) { w->sc[h - 4 + k] = w->tc[k * (NFFT / (2 * h))]; w->ss[h - 4 + k] = w->ts[k * (NFFT / (2 * h))]; }
}

/* x: NFFT complex. In: a[0..n) and b[0..n) are the real and imaginary parts (zero padded). */
static void fft2(float *x, const float *a, const float *b, int n, const tw_t *w)
{
    int i, j, h;
#if PI4_NEON
    { const float32x4_t zero = vdupq_n_f32(0.0f); for (i = 0; i < 2 * NFFT; i += 4) vst1q_f32(x + i, zero); }
#else
    for (i = 0; i < 2 * NFFT; i++) x[i] = 0.0f;
#endif
    for (i = 0, j = 0; i < n; i++) {             /* bit-reversed load */
        int bit = NFFT >> 1;
        x[2 * j] = a[i]; x[2 * j + 1] = b[i];
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
    }
    for (i = 0; i < 2 * NFFT; i += 8) {          /* stages 1 and 2 (twiddles 1 and -j: no products) */
        float *p = x + i;
        float a0r = p[0] + p[2], a0i = p[1] + p[3], a1r = p[0] - p[2], a1i = p[1] - p[3];
        float a2r = p[4] + p[6], a2i = p[5] + p[7], a3r = p[4] - p[6], a3i = p[5] - p[7];
        p[0] = a0r + a2r; p[1] = a0i + a2i; p[4] = a0r - a2r; p[5] = a0i - a2i;
        p[2] = a1r + a3i; p[3] = a1i - a3r; p[6] = a1r - a3i; p[7] = a1i + a3r;
    }
    for (h = 4; h < NFFT; h <<= 1) {
        const int step = NFFT / (2 * h);
        int i0, s;
        for (i0 = 0; i0 < NFFT; i0 += 2 * h) {
            float *pa = x + 2 * i0, *pb = pa + 2 * h;
#if PI4_NEON
            for (s = 0; s < h; s += 4) {
                float32x4x2_t A = vld2q_f32(pa + 2 * s), B = vld2q_f32(pb + 2 * s);
                float32x4_t cr, ci, tr, ti;
                if (step == 1) { cr = vld1q_f32(w->tc + s); ci = vld1q_f32(w->ts + s); }
                else if (step == 2) { cr = vld2q_f32(w->tc + 2 * s).val[0]; ci = vld2q_f32(w->ts + 2 * s).val[0]; }
                else if (step == 4) { cr = vld4q_f32(w->tc + 4 * s).val[0]; ci = vld4q_f32(w->ts + 4 * s).val[0]; }
                else { cr = vld1q_f32(w->sc + h - 4 + s); ci = vld1q_f32(w->ss + h - 4 + s); }
                tr = vmlsq_f32(vmulq_f32(B.val[0], cr), B.val[1], ci);
                ti = vmlaq_f32(vmulq_f32(B.val[0], ci), B.val[1], cr);
                B.val[0] = vsubq_f32(A.val[0], tr); B.val[1] = vsubq_f32(A.val[1], ti);
                A.val[0] = vaddq_f32(A.val[0], tr); A.val[1] = vaddq_f32(A.val[1], ti);
                vst2q_f32(pa + 2 * s, A); vst2q_f32(pb + 2 * s, B);
            }
#else
            for (s = 0; s < h; s++) {
                const float wr = w->tc[s * step], wi = w->ts[s * step];
                float tr = pb[2 * s] * wr - pb[2 * s + 1] * wi, ti = pb[2 * s] * wi + pb[2 * s + 1] * wr;
                pb[2 * s] = pa[2 * s] - tr; pb[2 * s + 1] = pa[2 * s + 1] - ti;
                pa[2 * s] += tr; pa[2 * s + 1] += ti;
            }
#endif
        }
    }
}

/* ------------------------------------------------------------------ */
/* decoder                                                             */

typedef struct { float z; int32_t lag, bin; } cand_t;

static void cand_insert(cand_t *c, int *nc, float z, int32_t lag, int32_t bin)
{
    int i, j;
    for (i = 0; i < *nc; i++)          /* near-duplicate across chunk edges */
        if (c[i].lag - lag <= 3 && lag - c[i].lag <= 3 && c[i].bin - bin <= 2 && bin - c[i].bin <= 2) {
            if (z <= c[i].z) return;
            for (j = i; j < *nc - 1; j++) c[j] = c[j + 1];
            (*nc)--;
            break;
        }
    for (i = *nc; i > 0 && c[i - 1].z < z; i--)
        if (i < NCMAX) c[i] = c[i - 1];
    if (i >= NCMAX) return;
    c[i].z = z; c[i].lag = lag; c[i].bin = bin;
    if (*nc < NCMAX) (*nc)++;
}

/* row[j] += d[j] (neg: -=) */
static void row_add(float *row, const float *d, int n, int neg)
{
    int j = 0;
#if PI4_NEON
    if (neg) for (; j + 4 <= n; j += 4) vst1q_f32(row + j, vsubq_f32(vld1q_f32(row + j), vld1q_f32(d + j)));
    else     for (; j + 4 <= n; j += 4) vst1q_f32(row + j, vaddq_f32(vld1q_f32(row + j), vld1q_f32(d + j)));
#endif
    if (neg) for (; j < n; j++) row[j] -= d[j];
    else     for (; j < n; j++) row[j] += d[j];
}

/* refinement state of one candidate: sub-block sums cc[sub][re x4 tones, im x4 tones] */
typedef struct {
    float wr[4], wi[4], pr[4], pim[4];
    float sa;
    int32_t s0, j;
    float cc[NSUB][8];
} ref_t;

/* tone powers pk[0..3] of symbol j at shift sh with the rotation r (NSUBSYM complex) */
static inline void sym_power(const ref_t *w, const float *r, int sh, int j, float *pk)
{
    const float *cc = w->cc[sh + NSUBSYM * j];
    int q;
#if PI4_NEON
    float32x4_t yr = vdupq_n_f32(0.0f), yi = vdupq_n_f32(0.0f);
    for (q = 0; q < NSUBSYM; q++, cc += 8) {
        const float32x4_t cre = vld1q_f32(cc), cim = vld1q_f32(cc + 4);
        const float32x4_t rre = vld1q_dup_f32(r + 2 * q), rim = vld1q_dup_f32(r + 2 * q + 1);
        yr = vaddq_f32(yr, vmlsq_f32(vmulq_f32(cre, rre), cim, rim));
        yi = vaddq_f32(yi, vmlaq_f32(vmulq_f32(cre, rim), cim, rre));
    }
    vst1q_f32(pk, vmlaq_f32(vmulq_f32(yr, yr), yi, yi));
#else
    float yr[4] = { 0, 0, 0, 0 }, yi[4] = { 0, 0, 0, 0 };
    int k;
    for (q = 0; q < NSUBSYM; q++, cc += 8)
        for (k = 0; k < 4; k++) {
            yr[k] += cc[k] * r[2 * q] - cc[4 + k] * r[2 * q + 1];
            yi[k] += cc[k] * r[2 * q + 1] + cc[4 + k] * r[2 * q];
        }
    for (k = 0; k < 4; k++) pk[k] = yr[k] * yr[k] + yi[k] * yi[k];
#endif
}

int pi4_decode(const dsrc_t *src, float f_lo, float f_hi, float nominal_start_s,
               pi4_result_t *out, int maxout, void *work, uint32_t work_len,
               int (*should_abort)(void))
{
    float fs, Ns, h, df, lag0;
    int32_t Nw, NL, NT, toff[4], b_lo, b_hi, band_lo, band_hi, rbcap;
    float *wbase, *wend;
    rb_t rb;
    cand_t cand[NCMAX];
    int ncand = 0, nout = 0, ci, k, i, j;
    uint8_t perm[NSYM];

    if (!src || !src->read || !out || maxout <= 0 || !work || work_len < PI4_WORK_MIN) return PI4_ERR_ARG;
    if (src->fs < 6000 || src->fs > 12000) return PI4_ERR_ARG;
    fs = (float)src->fs;
    Ns = fs / SYMRATE;                  /* samples per symbol (1333.33 @ 8 kHz) */
    Nw = iround(Ns);
    h = Ns * 0.25f;                     /* coarse hop: 1/4 symbol */
    df = fs / (float)NFFT;
    for (k = 0; k < 4; k++) toff[k] = iround((float)k * TONESP / df);
    b_lo = (int32_t)(f_lo / df + 0.999f);
    b_hi = (int32_t)(f_hi / df);
    if (b_lo < 1) b_lo = 1;
    if (b_hi < b_lo) b_hi = b_lo;
    if (b_hi + toff[3] > NFFT / 2 - 2) return PI4_ERR_ARG;
    lag0 = (nominal_start_s - 1.0f) * fs;
    NL = (int32_t)(3.0f * fs / h + 0.5f) + 1;   /* -1 .. +2 s in 1/4-symbol steps */
    NT = NL + 4 * (NSYM - 1);
    make_perm(perm);

    wbase = (float *)(void *)(((uintptr_t)work + 7u) & ~(uintptr_t)7u);
    wend = wbase + ((uint32_t)((uint8_t *)work + work_len - (uint8_t *)wbase) >> 2);
    rbcap = Nw + (int32_t)h + 8;                 /* two windows 1/4 symbol apart */
    rb.src = src; rb.buf = wbase; rb.cap = rbcap; rb.pos = -0x40000000; rb.fill = 0; rb.err = 0;
    wbase += rbcap;

    /* ---------------- coarse search ---------------- */
    {
        float *fx = wbase;                           /* 2*NFFT: FFT, then powers, then the sync observable */
        float *S = fx + 2 * NFFT + TW_FLOATS;        /* NL x bins of this pass */
        tw_t tw;
        int32_t pb0, nbmax;
        if (S + NL * STATBINS > wend) return PI4_ERR_ARG;
        nbmax = (int32_t)((wend - S) / NL);
        if (nbmax < b_hi - b_lo + 1) nbmax = nbmax / STATBINS * STATBINS;   /* several passes: whole statistics blocks */
        tw_setup(&tw, fx + 2 * NFFT);
        band_lo = b_lo; band_hi = b_hi + toff[3];

        for (pb0 = b_lo; pb0 <= b_hi; pb0 += nbmax) {
            int32_t nbp = b_hi - pb0 + 1, t, L, cb0;
            if (nbp > nbmax) nbp = nbmax;
            fzero(S, NL * nbp);
            for (t = 0; t < NT; t += 2) {
                int32_t sa = iround(lag0 + (float)t * h), sb = iround(lag0 + (float)(t + 1) * h), tt;
                const float *x;
                float sA = 0.0f, sB = 0.0f, invA, invB;
                float *dA = fx, *dB = fx + NFFT;      /* the observable of the two windows, bins pb0.. */
                if ((t & 15) == 0 && should_abort && should_abort()) return PI4_ERR_ABORT;
                x = rb_get(&rb, sa, sb - sa + Nw);
                fft2(fx, x, x + (sb - sa), Nw, &tw);
                /* powers of the two windows (x4, irrelevant): fx[2k] and fx[2k+1] */
                k = band_lo;
#if PI4_NEON
                for (; k + 3 <= band_hi; k += 4) {
                    float32x4x2_t X = vld2q_f32(fx + 2 * k);
                    const float32x4x2_t Y = vld2q_f32(fx + 2 * (NFFT - k - 3));
                    float32x4_t yr = vrev64q_f32(Y.val[0]), yi = vrev64q_f32(Y.val[1]), ar, ai, br, bi;
                    yr = vextq_f32(yr, yr, 2); yi = vextq_f32(yi, yi, 2);
                    ar = vaddq_f32(X.val[0], yr); ai = vsubq_f32(X.val[1], yi);
                    br = vaddq_f32(X.val[1], yi); bi = vsubq_f32(X.val[0], yr);
                    X.val[0] = vmlaq_f32(vmulq_f32(ar, ar), ai, ai);
                    X.val[1] = vmlaq_f32(vmulq_f32(br, br), bi, bi);
                    vst2q_f32(fx + 2 * k, X);
                }
#endif
                for (; k <= band_hi; k++) {
                    float xr = fx[2 * k], xi = fx[2 * k + 1];
                    float yr = fx[2 * (NFFT - k)], yi = fx[2 * (NFFT - k) + 1];
                    float ar = xr + yr, ai = xi - yi, br = xi + yi, bi = xr - yr;
                    fx[2 * k] = ar * ar + ai * ai;
                    fx[2 * k + 1] = br * br + bi * bi;
                }
                for (k = band_lo; k <= band_hi; k++) { sA += fx[2 * k]; sB += fx[2 * k + 1]; }
                invA = sA > 0.0f ? (float)(band_hi - band_lo + 1) / sA : 0.0f;
                invB = sB > 0.0f ? (float)(band_hi - band_lo + 1) / sB : 0.0f;
                /* D = (P1+P3-P0-P2)/mean power, written behind the read position (dA) / above the powers (dB) */
                j = 0;
#if PI4_NEON
                {
                    const float32x4_t vA = vdupq_n_f32(invA), vB = vdupq_n_f32(invB);
                    for (; j + 4 <= nbp; j += 4) {
                        const float *p = fx + 2 * (pb0 + j);
                        const float32x4x2_t p0 = vld2q_f32(p), p1 = vld2q_f32(p + 2 * toff[1]);
                        const float32x4x2_t p2 = vld2q_f32(p + 2 * toff[2]), p3 = vld2q_f32(p + 2 * toff[3]);
                        vst1q_f32(dA + j, vmulq_f32(vsubq_f32(vsubq_f32(vaddq_f32(p1.val[0], p3.val[0]), p0.val[0]), p2.val[0]), vA));
                        vst1q_f32(dB + j, vmulq_f32(vsubq_f32(vsubq_f32(vaddq_f32(p1.val[1], p3.val[1]), p0.val[1]), p2.val[1]), vB));
                    }
                }
#endif
                for (; j < nbp; j++) {
                    const float *p = fx + 2 * (pb0 + j);
                    float a = (p[2 * toff[1]] + p[2 * toff[3]] - p[0] - p[2 * toff[2]]) * invA;
                    float b = (p[2 * toff[1] + 1] + p[2 * toff[3] + 1] - p[1] - p[2 * toff[2] + 1]) * invB;
                    dA[j] = a; dB[j] = b;
                }
                for (tt = t; tt <= t + 1 && tt < NT; tt++) {
                    const float *dv = tt == t ? dA : dB;
                    int32_t kmin, kmax;
                    kmin = tt - NL + 1; kmin = kmin < 0 ? 0 : (kmin + 3) >> 2;
                    kmax = tt >> 2; if (kmax > NSYM - 1) kmax = NSYM - 1;
                    for (k = kmin; k <= kmax; k++) row_add(S + (tt - 4 * k) * nbp, dv, nbp, !pi4_sync[k]);
                }
            }
            if (rb.err) return PI4_ERR_IO;
            /* candidates: z-score and local maxima inside each statistics block */
            for (cb0 = 0; cb0 < nbp; cb0 += STATBINS) {
                int32_t nb = nbp - cb0;
                float mean = 0.0f, var = 0.0f, sd;
                if (nb > STATBINS) nb = STATBINS;
                for (L = 0; L < NL; L++) { const float *r = S + L * nbp + cb0; for (j = 0; j < nb; j++) mean += r[j]; }
                mean /= (float)(NL * nb);
                for (L = 0; L < NL; L++) { const float *r = S + L * nbp + cb0; for (j = 0; j < nb; j++) var += (r[j] - mean) * (r[j] - mean); }
                sd = pi4_sqrtf(var / (float)(NL * nb));
                if (sd <= 0.0f) continue;
                for (L = 0; L < NL; L++)
                    for (j = 0; j < nb; j++) {
                        float v = S[L * nbp + cb0 + j], z = (v - mean) / sd;
                        int32_t l2, j2, ismax = 1;
                        if (z < CAND_Z) continue;
                        for (l2 = L - 3; l2 <= L + 3 && ismax; l2++) {
                            if (l2 < 0 || l2 >= NL) continue;
                            for (j2 = j - 2; j2 <= j + 2; j2++) {
                                if (j2 < 0 || j2 >= nb || (l2 == L && j2 == j)) continue;
                                if (S[l2 * nbp + cb0 + j2] > v) { ismax = 0; break; }
                            }
                        }
                        if (ismax) cand_insert(cand, &ncand, z, L, pb0 + cb0 + j);
                    }
            }
        }
    }

    /* ---------------- refine + decode the candidates ---------------- */
    {
        float *P   = wbase;                          /* NSYM x 4 */
        float *met = P + NSYM * 4;                   /* NSYM x 2 */
        float *lam = met + NSYM * 2;                 /* NSYM */
        float *rot = lam + NSYM;                     /* NDF x NSUBSYM complex */
        fnode_t *nodes = (fnode_t *)(void *)(rot + NDF * NSUBSYM * 2) + 1;      /* [-1] = sentinel */
        uint8_t *et = (uint8_t *)(nodes + NNODE + 1);
        uint8_t *bits = et + 1024;
        uint8_t *cb = bits + 64;
        ref_t *refs = (ref_t *)(void *)(((uintptr_t)(cb + NSYM) + 7u) & ~(uintptr_t)7u);
        int cap;
        if ((float *)(void *)(refs + 1) > wend) return PI4_ERR_ARG;
        cap = (int)((uint32_t)((uint8_t *)wend - (uint8_t *)refs) / sizeof(ref_t));
        if (cap > NCMAX) cap = NCMAX;
        make_et(et);

        for (i = 0; i < NDF; i++)
            for (j = 0; j < NSUBSYM; j++) {
                float dfi = -2.5f + 0.25f * (float)i;
                pi4_sincos(-6.283185307179586 * (double)dfi * ((double)j + 0.5) / (SYMRATE * NSUBSYM),
                           &rot[2 * (i * NSUBSYM + j) + 1], &rot[2 * (i * NSUBSYM + j)]);
            }

        ci = 0;
        while (ci < ncand && nout < maxout) {
            int grp[NCMAX], ng = 0, g, left, cnt = 0;
            /* next group: candidates not on top of an earlier decode (checked again at their turn) */
            for (; ci < ncand && ng < cap; ci++) {
                float f0 = (float)cand[ci].bin * df;
                float sa = lag0 + (float)cand[ci].lag * h - 2.0f * Ns / NSUBSYM;
                int dup = 0;
                ref_t *w = refs + ng;
                for (k = 0; k < nout; k++) {
                    float ddt = out[k].dt_s - ((sa / fs) - nominal_start_s);
                    float ddf = out[k].freq_hz - f0;
                    if (ddt < 0.5f && ddt > -0.5f && ddf < 10.0f && ddf > -10.0f) dup = 1;
                }
                if (dup) continue;
                for (k = 0; k < 4; k++) {
                    pi4_sincos(-6.283185307179586 * (double)(f0 + (float)k * TONESP) / (double)fs, &w->wi[k], &w->wr[k]);
                    w->pr[k] = 1.0f; w->pim[k] = 0.0f;
                }
                w->sa = sa; w->s0 = iround(sa); w->j = 0;
                grp[ng++] = ci;
            }
            if (!ng) break;
            /* mix down at the 4 tones, integrate 1/8-symbol sub-blocks: one sequential read for the
             * group (always the candidate whose next sub-block starts first) */
            for (left = ng; left > 0;) {
                ref_t *w = 0;
                int32_t e, n, q;
                const float *x;
                float *cc;
                for (g = 0; g < ng; g++)
                    if (refs[g].j < NSUB && (!w || refs[g].s0 < w->s0)) w = refs + g;
                if ((cnt++ & 63) == 0 && should_abort && should_abort()) return nout ? nout : PI4_ERR_ABORT;   /* keep decodes found before the abort */
                e = iround(w->sa + (float)(w->j + 1) * Ns / NSUBSYM); n = e - w->s0;
                x = rb_get(&rb, w->s0, n);
                cc = w->cc[w->j];
#if PI4_NEON
                {
                    const float32x4_t wr = vld1q_f32(w->wr), wi = vld1q_f32(w->wi);
                    float32x4_t pr = vld1q_f32(w->pr), pim = vld1q_f32(w->pim), g2;
                    float32x4_t acr = vdupq_n_f32(0.0f), aci = vdupq_n_f32(0.0f);
                    for (q = 0; q < n; q++) {
                        const float32x4_t v = vld1q_dup_f32(x + q);
                        const float32x4_t tr = vmlsq_f32(vmulq_f32(pr, wr), pim, wi);
                        acr = vmlaq_f32(acr, v, pr); aci = vmlaq_f32(aci, v, pim);
                        pim = vmlaq_f32(vmulq_f32(pr, wi), pim, wr);
                        pr = tr;
                    }
                    g2 = vmlsq_f32(vdupq_n_f32(1.5f), vdupq_n_f32(0.5f), vmlaq_f32(vmulq_f32(pr, pr), pim, pim));
                    vst1q_f32(w->pr, vmulq_f32(pr, g2)); vst1q_f32(w->pim, vmulq_f32(pim, g2));
                    vst1q_f32(cc, acr); vst1q_f32(cc + 4, aci);
                }
#else
                {
                    float acr[4] = { 0, 0, 0, 0 }, aci[4] = { 0, 0, 0, 0 };
                    for (q = 0; q < n; q++) {
                        float v = x[q];
                        for (k = 0; k < 4; k++) {
                            float tr = w->pr[k] * w->wr[k] - w->pim[k] * w->wi[k];
                            acr[k] += v * w->pr[k]; aci[k] += v * w->pim[k];
                            w->pim[k] = w->pr[k] * w->wi[k] + w->pim[k] * w->wr[k];
                            w->pr[k] = tr;
                        }
                    }
                    for (k = 0; k < 4; k++) {
                        float g2 = 1.5f - 0.5f * (w->pr[k] * w->pr[k] + w->pim[k] * w->pim[k]);
                        w->pr[k] *= g2; w->pim[k] *= g2;
                        cc[k] = acr[k]; cc[4 + k] = aci[k];
                    }
                }
#endif
                w->s0 = e;
                if (++w->j == NSUB) left--;
            }
            if (rb.err) return PI4_ERR_IO;

            for (g = 0; g < ng && nout < maxout; g++) {
                const ref_t *w = refs + g;
                const int c0 = grp[g];
                float f0 = (float)cand[c0].bin * df;
                float sa = w->sa;
                float best = -1e30f, Es, sumz, sumq, fitz, esd;
                int bi = 0, bs = 0, sh, dup = 0, rc, agree;
                char txt[12];

                /* skip candidates on top of an earlier decode */
                for (k = 0; k < nout; k++) {
                    float ddt = out[k].dt_s - ((sa / fs) - nominal_start_s);
                    float ddf = out[k].freq_hz - f0;
                    if (ddt < 0.5f && ddt > -0.5f && ddf < 10.0f && ddf > -10.0f) dup = 1;
                }
                if (dup) continue;

                /* grid: frequency offset x time shift, maximise sync correlation */
                for (i = 0; i < NDF; i++) {
                    const float *r = rot + 2 * i * NSUBSYM;
                    if (should_abort && should_abort()) return nout ? nout : PI4_ERR_ABORT;   /* keep decodes found before the abort */
                    for (sh = 0; sh < NSHIFT; sh++) {
                        float msum = 0.0f;
                        for (j = 0; j < NSYM; j++) {
                            float pk[4];
                            sym_power(w, r, sh, j, pk);
                            if (pi4_sync[j]) msum += pk[1] + pk[3] - pk[0] - pk[2];
                            else             msum += pk[0] + pk[2] - pk[1] - pk[3];
                        }
                        if (msum > best) { best = msum; bi = i; bs = sh; }
                    }
                }

                /* per-symbol tone powers at the best point */
                for (j = 0; j < NSYM; j++) sym_power(w, rot + 2 * bi * NSUBSYM, bs, j, P + 4 * j);
                /* local noise from the two tones the sync bit excludes */
                for (j = 0; j < NSYM; j++) {           /* lam[] used as temp */
                    int s = pi4_sync[j];
                    lam[j] = 0.5f * (P[4 * j + (1 - s)] + P[4 * j + (3 - s)]);
                }
                for (j = 0; j < NSYM; j++) {
                    float s = 0.0f; int n = 0, q;
                    for (q = j - NOISE_W; q <= j + NOISE_W; q++)
                        if (q >= 0 && q < NSYM) { s += lam[q]; n++; }
                    met[j] = s / (float)n;             /* met[] used as temp noise */
                }
                Es = 0.0f;
                for (j = 0; j < NSYM; j++) {
                    float inv = met[j] > 0.0f ? 1.0f / met[j] : 0.0f;
                    for (k = 0; k < 4; k++) P[4 * j + k] *= inv;
                    Es += P[4 * j + pi4_sync[j]] + P[4 * j + pi4_sync[j] + 2];
                }
                Es = Es / (float)NSYM - 2.0f;
                if (Es < 0.3f) Es = 0.3f;
                if (Es > 400.0f) Es = 400.0f;
                for (j = 0; j < NSYM; j++) {
                    int s = pi4_sync[j];
                    float l = lni0(2.0f * pi4_sqrtf(Es * P[4 * j + s + 2])) - lni0(2.0f * pi4_sqrtf(Es * P[4 * j + s]));
                    if (l > LLR_MAX) l = LLR_MAX;
                    if (l < -LLR_MAX) l = -LLR_MAX;
                    lam[j] = l;
                }
                for (j = 0; j < NSYM; j++) {          /* conv-order bit metrics */
                    float l = lam[perm[j]];
                    met[2 * j + 1] = 1.0f - softplus(-l) * 1.44269504f - FANO_BIAS;
                    met[2 * j]     = 1.0f - softplus(l) * 1.44269504f - FANO_BIAS;
                }
                rc = fano(nodes, met, bits, et, should_abort);
                DBG("cand %d z=%.2f lag=%d f0=%.2f best df=%.2f sh=%d Es=%.2f fano=%d\n", c0, cand[c0].z, cand[c0].lag, f0, -2.5f + 0.25f * (float)bi, bs, Es, rc);
                if (rc < 0) return nout ? nout : PI4_ERR_ABORT;   /* keep decodes found before the abort */
                if (rc != 0) continue;
                if (!unpack(bits, txt)) continue;
                {
                    uint32_t hi = 0, lo = 0;
                    for (k = 0; k < NBITS; k++) {
                        if (k < 10) hi = (hi << 1) | bits[k];
                        else lo = (lo << 1) | bits[k];
                    }
                    if (hi == 0 && lo == 0) continue;           /* all-zero codeword */
                    conv_encode(hi, lo, cb);
                }
                /* gate: soft correlation of the re-encoded data bits */
                sumz = 0.0f; sumq = 0.0f; agree = 0; esd = 0.0f;
                for (j = 0; j < NSYM; j++) {
                    int ch = perm[j], s = pi4_sync[ch], d = cb[j];
                    float dv = P[4 * ch + s + 2] - P[4 * ch + s];
                    sumz += d ? dv : -dv;
                    sumq += dv * dv;
                    if ((dv > 0.0f) == (d != 0)) agree++;
                    esd += P[4 * ch + s + 2 * d];
                }
                fitz = sumq > 0.0f ? sumz / pi4_sqrtf(sumq) : 0.0f;
                if (fitz < GATE_Z) continue;
                esd = esd / (float)NSYM - 1.0f;
                if (esd < 0.01f) esd = 0.01f;
                /* same text already reported (e.g. two candidates on one signal) */
                for (k = 0; k < nout; k++) {
                    int q, same = 1;
                    for (q = 0; q < 12 && same; q++) {
                        if (out[k].msg[q] != txt[q]) same = 0;
                        if (!txt[q]) break;
                    }
                    if (same) dup = 1;
                }
                if (dup) continue;
                for (k = 0; k < 12; k++) { out[nout].msg[k] = txt[k]; if (!txt[k]) break; }
                for (; k < 12; k++) out[nout].msg[k] = 0;
                out[nout].freq_hz = f0 - 2.5f + 0.25f * (float)bi;
                out[nout].dt_s = (sa + (float)bs * Ns / NSUBSYM) / fs - nominal_start_s;
                /* Es (per symbol, noise-per-tone units) -> S/N in 2500 Hz: Es * 6 / 2500 */
                out[nout].snr_db = 4.3429448f * pi4_logf(esd * SYMRATE / 2500.0f);
                out[nout].quality = (int)((float)agree * (100.0f / (float)NSYM) + 0.5f);
                nout++;
            }
        }
    }
    return nout;
}
