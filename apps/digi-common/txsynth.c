/* SPDX-License-Identifier: MIT
 * txsynth.c - streaming MFSK / GFSK synthesizer + period alignment (see txsynth.h).
 * Own code.  The GFSK pulse is the published WSJT-X/FT8 definition
 *   g(t) = (erf(c*B*(t+1/2)) - erf(c*B*(t-1/2))) / 2,  c = pi*sqrt(2/ln 2),  B = 2,
 * tabulated below (values computed offline with double-precision erf).
 */
#include "txsynth.h"

#define PI_F 3.14159265358979f

float txs_sin(uint32_t phase)
{
    int32_t p = (int32_t)phase;                     /* [-2^31, 2^31) = [-pi, pi) */
    if (p > 0x40000000 || p < -0x40000000)          /* fold into [-pi/2, pi/2] */
        p = (int32_t)(0x80000000u - (uint32_t)p);
    {
        float x = (float)p * (PI_F / 2147483648.0f);
        float x2 = x * x;
        return x * (1.0f + x2 * (-1.66666667e-1f + x2 * (8.33333333e-3f +
                    x2 * (-1.98412698e-4f + x2 * 2.75573192e-6f))));
    }
}

uint32_t txs_hz_to_inc(double hz)
{
    double v = hz * (4294967296.0 / TXS_FS);
    if (v <= 0.0) return 0;
    if (v >= 4294967295.0) return 0xffffffffu;
    return (uint32_t)(v + 0.5);
}

int16_t txs_q16(float v)
{
    int32_t i = v >= 0.0f ? (int32_t)(v + 0.5f) : -(int32_t)(0.5f - v);
    if (i > 32767) i = 32767;
    if (i < -32767) i = -32767;
    return (int16_t)i;
}

float txs_ramp_gain(uint32_t k, uint32_t len, uint32_t ramp)
{
    uint32_t r = ramp, m;
    float s;
    if (r > len / 2) r = len / 2;
    if (r == 0) return 1.0f;
    if (k < r) m = k;
    else if (k >= len - r) m = len - 1 - k;
    else return 1.0f;
    /* sin^2(pi/2 * (m + 1/2) / r) */
    s = txs_sin((uint32_t)(((double)m + 0.5) / (double)r * 1073741824.0));
    return s * s;
}

/* WSJT-X FT8 GFSK pulse (BT = 2), g(i/64) for i = 0..64; g(t) = g(-t), g(t >= 1) = 0 (< 3e-14) */
static const float gfsk_tab[65] = {
    1.000000000e+00f, 1.000000000e+00f, 1.000000000e+00f, 1.000000000e+00f, 1.000000000e+00f,
    9.999999999e-01f, 9.999999996e-01f, 9.999999981e-01f, 9.999999924e-01f, 9.999999709e-01f,
    9.999998940e-01f, 9.999996339e-01f, 9.999988021e-01f, 9.999962845e-01f, 9.999890760e-01f,
    9.999695476e-01f, 9.999194932e-01f, 9.997981075e-01f, 9.995195948e-01f, 9.989149853e-01f,
    9.976731671e-01f, 9.952599605e-01f, 9.908230068e-01f, 9.831045672e-01f, 9.704009123e-01f,
    9.506183407e-01f, 9.214714693e-01f, 8.808405612e-01f, 8.272514319e-01f, 7.603781506e-01f,
    6.814222737e-01f, 5.932215214e-01f, 5.000000000e-01f, 4.067784786e-01f, 3.185777263e-01f,
    2.396218494e-01f, 1.727485681e-01f, 1.191594388e-01f, 7.852853068e-02f, 4.938165925e-02f,
    2.959908773e-02f, 1.689543278e-02f, 9.176993218e-03f, 4.740039527e-03f, 2.326832851e-03f,
    1.085014693e-03f, 4.804052025e-04f, 2.018925359e-04f, 8.050683794e-05f, 3.045244764e-05f,
    1.092396943e-05f, 3.715457016e-06f, 1.197934576e-06f, 3.660733141e-07f, 1.060108489e-07f,
    2.908841379e-08f, 7.561772597e-09f, 1.862141830e-09f, 4.343540527e-10f, 9.595718664e-11f,
    2.007605193e-11f, 3.977540519e-12f, 7.461808949e-13f, 1.325606291e-13f, 0.0f
};

static float gfsk_pulse(double t)
{
    double a = t < 0.0 ? -t : t;
    uint32_t i;
    float f;
    if (a >= 1.0) return 0.0f;
    a *= 64.0;
    i = (uint32_t)a;
    f = (float)(a - (double)i);
    return gfsk_tab[i] + f * (gfsk_tab[i + 1] - gfsk_tab[i]);
}

/* first sample of symbol k: ceil(k*num/den) */
static uint32_t sym_start(const txs_mfsk_t *s, uint32_t k)
{
    return (k * s->sps_num + s->sps_den - 1) / s->sps_den;
}

int txs_mfsk_init(txs_mfsk_t *s, const uint8_t *tones, uint32_t nsym, uint32_t sps_num,
                  uint32_t sps_den, double f0, double df, uint32_t ntones, int gfsk,
                  int level, uint32_t ramp)
{
    double ftop = f0 + df * (double)(ntones ? ntones - 1 : 0);
    if (!s || !tones || !nsym || !sps_num || !sps_den || nsym > 4096 || sps_num > 1000000u)
        return TXE_ARG;
    if (!(f0 > 0.0) || !(ftop < 4000.0) || level < 0 || level > 32767)
        return TXE_ARG;
    s->tones = tones;
    s->nsym = nsym;
    s->sps_num = sps_num;
    s->sps_den = sps_den;
    s->nsamp = sym_start(s, nsym);
    s->n = 0;
    s->phase = 0;
    s->inc0 = f0 * (4294967296.0 / TXS_FS);
    s->incstep = df * (4294967296.0 / TXS_FS);
    s->level = (float)(level ? level : TXS_LEVEL_DEFAULT);
    s->ramp = ramp == 0 ? TXS_RAMP_DEFAULT : (ramp == 0xffffffffu ? 0 : ramp);
    s->gfsk = (uint8_t)(gfsk != 0);
    return TXE_OK;
}

static uint32_t tone_at(const txs_mfsk_t *s, int32_t j)
{
    if (j < 0) j = 0;                                   /* WSJT-X dummy symbols at both ends */
    if (j >= (int32_t)s->nsym) j = (int32_t)s->nsym - 1;
    return s->tones[j];
}

int txs_mfsk_fill(txs_mfsk_t *s, int16_t *buf, int n)
{
    int w = 0;
    uint32_t sym, next;
    double rcp;
    if (!s || !buf || n <= 0 || s->n >= s->nsamp) return 0;
    rcp = (double)s->sps_den / (double)s->sps_num;      /* symbols per sample */
    /* current symbol from the sample index (exact integer arithmetic) */
    sym = (s->n * s->sps_den) / s->sps_num;
    if (sym_start(s, sym + 1) <= s->n) sym++;
    next = sym_start(s, sym + 1);
    while (w < n && s->n < s->nsamp) {
        double incd;
        uint32_t inc;
        float g;
        if (s->n >= next) { sym++; next = sym_start(s, sym + 1); }
        if (s->gfsk) {
            double t = ((double)s->n + 0.5) * rcp;      /* time in symbols at sample centre */
            float v = 0.0f;
            int32_t j;
            for (j = (int32_t)sym - 1; j <= (int32_t)sym + 1; j++)
                v += (float)tone_at(s, j) * gfsk_pulse(t - (double)j - 0.5);
            incd = s->inc0 + (double)v * s->incstep;
        } else {
            incd = s->inc0 + (double)s->tones[sym] * s->incstep;
        }
        inc = (uint32_t)(incd + 0.5);
        g = s->level * txs_ramp_gain(s->n, s->nsamp, s->ramp);
        buf[w++] = txs_q16(g * txs_sin(s->phase));
        s->phase += inc;
        s->n++;
    }
    return w;
}

uint32_t txs_ms_to_next_start(uint32_t utc_s, uint32_t ms, uint32_t period_ms, uint32_t start_ms,
                              uint32_t late_ok_ms)
{
    uint32_t pos;
    if (period_ms < 1000 || period_ms % 1000) return 0xffffffffu;
    start_ms %= period_ms;
    pos = ((utc_s % (period_ms / 1000)) * 1000 + ms) % period_ms;
    if (pos <= start_ms) return start_ms - pos;
    if (pos - start_ms <= late_ok_ms) return 0;
    return period_ms - pos + start_ms;
}
