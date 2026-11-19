/* SPDX-License-Identifier: GPL-3.0-or-later
 * q65enc.c - Q65 encoder, see q65enc.h.
 * Derived from WSJT-X: lib/qra/q65/genq65.f90 (symbol layout, sync positions), lib/qra/q65/q65.c
 * (q65_encode, _q65_crc12) and lib/qra/q65/qracodes.c (qra_encode) with the code tables of
 * lib/qra/q65/qra15_65_64_irr_e23.c.  QRA code (C) 2020 Nico Palermo IV3NWV, WSJT-X (C) K1JT
 * et al.; both GPL-3.0-or-later.
 */
#include "q65enc.h"
#include "msg77.h"

/* qra15_65_64_irr_e23: accumulator chain (NC = 50) */
static const uint8_t acc_idx[50] = {
    13, 1, 3, 4, 8, 12, 9, 14, 10, 5, 0, 7, 1, 11, 8, 9, 12, 6, 3, 10,
    7, 5, 2, 13, 12, 4, 8, 0, 1, 11, 2, 9, 14, 5, 6, 13, 7, 12, 11, 2,
    9, 0, 10, 4, 7, 14, 8, 11, 3, 6 };
static const uint8_t acc_wlog[50] = {
    0, 14, 0, 0, 13, 37, 0, 27, 56, 62, 29, 0, 52, 34, 62, 4, 3, 22, 25, 0,
    22, 0, 20, 10, 0, 43, 53, 60, 0, 0, 0, 62, 0, 5, 0, 61, 36, 31, 61, 59,
    10, 0, 29, 39, 25, 18, 0, 14, 11, 50 };
/* GF(64) log / exp */
static const uint8_t gflog[64] = {
    0, 0, 1, 6, 2, 12, 7, 26, 3, 32, 13, 35, 8, 48, 27, 18, 4, 24, 33, 16,
    14, 52, 36, 54, 9, 45, 49, 38, 28, 41, 19, 56, 5, 62, 25, 11, 34, 31, 17, 47,
    15, 23, 53, 51, 37, 44, 55, 40, 10, 61, 46, 30, 50, 22, 39, 43, 29, 60, 42, 21,
    20, 59, 57, 58 };
static const uint8_t gfexp[63] = {
    1, 2, 4, 8, 16, 32, 3, 6, 12, 24, 48, 35, 5, 10, 20, 40, 19, 38, 15, 30,
    60, 59, 53, 41, 17, 34, 7, 14, 28, 56, 51, 37, 9, 18, 36, 11, 22, 44, 27, 54,
    47, 29, 58, 55, 45, 25, 50, 39, 13, 26, 52, 43, 21, 42, 23, 46, 31, 62, 63, 61,
    57, 49, 33 };
/* genq65 isync (1-based) */
static const uint8_t isync[22] = { 1, 9, 12, 13, 15, 22, 23, 26, 27, 33, 35, 38, 46, 50, 55, 60,
                                   62, 66, 69, 74, 76, 85 };

int q65_encode(const char *msg, uint8_t tones[Q65_NSYM])
{
    uint8_t b[10], x[15], y[50];
    int i, k, j, rc;
    uint32_t sr = 0;
    uint8_t chk = 0;
    if (!tones) return TXE_ARG;
    if ((rc = msg77_pack(msg, b)) != 0) return rc;
    /* 77 bits + one 0 bit -> 13 x 6 bits (MSB first) */
    for (i = 0; i < 13; i++) {
        int v = 0;
        for (k = 0; k < 6; k++) {
            int bit = i * 6 + k;
            v = (v << 1) | ((b[bit >> 3] >> (7 - (bit & 7))) & 1);
        }
        x[i] = (uint8_t)v;
    }
    /* CRC-12, g(x) = x^12+x^11+x^3+x^2+x+1, LSB-first over the 6-bit symbols */
    for (i = 0; i < 13; i++) {
        uint32_t t = x[i];
        for (k = 0; k < 6; k++) {
            sr = ((t ^ sr) & 1u) ? (sr >> 1) ^ 0xF01u : sr >> 1;
            t >>= 1;
        }
    }
    x[13] = (uint8_t)(sr & 0x3F);
    x[14] = (uint8_t)(sr >> 6);
    /* QRA accumulate: chk(k+1) = chk(k) + x[idx(k)] * alpha^wlog(k) over GF(64) */
    for (k = 0; k < 50; k++) {
        uint8_t t = x[acc_idx[k]];
        if (t) chk ^= gfexp[(gflog[t] + acc_wlog[k]) % 63];
        y[k] = chk;
    }
    /* codeword (CRC punctured): x[0..12], y[0..49]; sync = tone 0, data tone = symbol + 1 */
    for (i = 0, j = 0, k = 0; i < Q65_NSYM; i++) {
        if (j < 22 && i + 1 == isync[j]) { tones[i] = 0; j++; }
        else { tones[i] = (uint8_t)((k < 13 ? x[k] : y[k - 13]) + 1); k++; }
    }
    return 0;
}

/* nsps at 12 kHz: 1800, 3600, 7200, 16000, 41472  ->  at 8 kHz *2/3 (exact as num/den) */
static uint32_t nsps12(int period_s)
{
    switch (period_s) {
    case 15: return 1800; case 30: return 3600; case 60: return 7200;
    case 120: return 16000; case 300: return 41472;
    default: return 0;
    }
}

uint32_t q65_period_ms(int period_s) { return nsps12(period_s) ? (uint32_t)period_s * 1000u : 0; }
uint32_t q65_tx_start_ms(int period_s) { uint32_t n = nsps12(period_s); return !n ? 0 : n <= 3600 ? 500u : 1000u; }
uint32_t q65_tx_samples(int period_s) { return (Q65_NSYM * nsps12(period_s) * 2u + 2u) / 3u; }

int q65_tx_init(q65_tx_t *t, const char *msg, double f0_hz, int period_s, int submode,
                int level, uint32_t ramp)
{
    uint32_t n12 = nsps12(period_s);
    int rc;
    if (!t || !n12 || submode < 0 || submode > 4) return TXE_ARG;
    if ((rc = q65_encode(msg, t->tones)) != 0) return rc;
    /* baud = 12000 / nsps12; spacing = 2^submode * baud;  sps(8 kHz) = nsps12 * 2 / 3 */
    return txs_mfsk_init(&t->s, t->tones, Q65_NSYM, n12 * 2u, 3u, f0_hz,
                         (double)(1u << submode) * 12000.0 / (double)n12, 65, 0, level, ramp);
}
