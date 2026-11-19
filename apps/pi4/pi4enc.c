/* SPDX-License-Identifier: MIT
 * pi4enc.c - PI4 beacon encoder, see pi4enc.h.  Written from the PI4 specification text
 * (rudius.net/oz2m/ngnb/pi4.htm); the sync vector is the one published there (DJ5HG).
 */
#include "pi4enc.h"

#define POLY1 0xF2D05351u
#define POLY2 0xE4613C47u
#define CW_T0     (PI4_CW_START_MS * 8u)       /* 200000 */
#define UNIT      (PI4_CW_DOT_MS * 8u)         /* 800 samples */
#define T_END     PI4_TX_SAMPLES               /* 476000 */
#define MAX_UNITS ((PI4_CARRIER_END_MS - PI4_CW_START_MS) / PI4_CW_DOT_MS)   /* 345 */

/* sync vector, MSB first */
static const uint8_t pi4sync[19] = {
    0x27, 0xaa, 0x44, 0x67, 0x9f, 0x37, 0xad, 0xa0, 0xfa, 0x83, 0xe9, 0x28, 0x4c, 0x18,
    0x67, 0x76, 0xa8, 0x70, 0xc0 };

/* Morse: leading 1 = start marker, then 0 = dot, 1 = dash */
static const uint8_t morse_d[10] = { 0x3f, 0x2f, 0x27, 0x23, 0x21, 0x20, 0x30, 0x38, 0x3c, 0x3e };
static const uint8_t morse_l[26] = { 0x05, 0x18, 0x1a, 0x0c, 0x02, 0x12, 0x0e, 0x10, 0x04, 0x17,
    0x0d, 0x14, 0x07, 0x06, 0x0f, 0x16, 0x1d, 0x0a, 0x08, 0x03, 0x09, 0x11, 0x0b, 0x19, 0x1b, 0x1c };

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static uint32_t parity32(uint32_t x)
{
    x ^= x >> 16; x ^= x >> 8; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1;
    return x & 1u;
}

int pi4enc_symbols(const char *msg, uint8_t sym[PI4_NSYM])
{
    uint32_t hi = 0, lo = 0, reg = 0;   /* 42-bit source value as hi:lo */
    uint8_t cb[PI4_NSYM];
    int i, k, end = 0;
    if (!msg || !sym) return TXE_ARG;
    for (i = 0; i < 8; i++) {
        char c = end ? ' ' : up(msg[i]);
        uint32_t v, a, b;
        if (!c) { end = 1; c = ' '; }
        if (c >= '0' && c <= '9') v = (uint32_t)(c - '0');
        else if (c >= 'A' && c <= 'Z') v = (uint32_t)(c - 'A' + 10);
        else if (c == ' ') v = 36;
        else if (c == '/') v = 37;
        else return TXE_CHAR;
        /* (hi:lo) = (hi:lo) * 38 + v in 16-bit limbs */
        a = (lo & 0xffffu) * 38u + v;
        b = (lo >> 16) * 38u + (a >> 16);
        lo = (a & 0xffffu) | (b << 16);
        hi = hi * 38u + (b >> 16);
    }
    if (!end && msg[8]) return TXE_LEN;
    /* convolutional code: 42 data bits MSB first, then 31 zeros */
    for (k = 0; k < 73; k++) {
        uint32_t bit = k < 10 ? (hi >> (9 - k)) & 1u : k < 42 ? (lo >> (41 - k)) & 1u : 0u;
        reg = (reg << 1) | bit;
        cb[2 * k] = (uint8_t)parity32(reg & POLY1);
        cb[2 * k + 1] = (uint8_t)parity32(reg & POLY2);
    }
    /* interleave: i = 0..255, J = bit-reverse(i); J < 146 takes the next coded bit */
    for (i = 0, k = 0; i < 256; i++) {
        int j = 0, b;
        for (b = 0; b < 8; b++) j |= ((i >> b) & 1) << (7 - b);
        if (j < PI4_NSYM) {
            sym[j] = (uint8_t)(2 * cb[k++] + ((pi4sync[j >> 3] >> (7 - (j & 7))) & 1));
        }
    }
    return 0;
}

static int keybit(const pi4_tx_t *t, uint32_t u)
{
    return u < t->units && ((t->key[u >> 3] >> (u & 7)) & 1);
}

/* append a CW word; returns new unit count or < 0 */
static int cw_word(pi4_tx_t *t, const char *w, int u)
{
    int i;
    for (i = 0; w[i]; i++) {
        char c = up(w[i]);
        uint8_t m;
        int nb, b;
        if (c >= '0' && c <= '9') m = morse_d[c - '0'];
        else if (c >= 'A' && c <= 'Z') m = morse_l[c - 'A'];
        else if (c == '/') m = 0x32;
        else return TXE_CHAR;
        for (nb = 7; !((m >> nb) & 1); nb--) ;           /* marker bit */
        for (b = nb - 1; b >= 0; b--) {
            int len = ((m >> b) & 1) ? 3 : 1, j;
            for (j = 0; j < len; j++, u++) {
                if (u >= (int)MAX_UNITS) return TXE_LEN;
                t->key[u >> 3] |= (uint8_t)(1u << (u & 7));
            }
            u += 1;                                      /* element gap */
        }
        u += 2;                                          /* -> character gap of 3 */
    }
    u += 2;                                              /* -> 500 ms = 5 units after the word */
    if (u > (int)MAX_UNITS) return TXE_LEN;
    return u;
}

int pi4_tx_init(pi4_tx_t *t, const char *msg, const char *cw_call, const char *cw_loc,
                double tone0_hz, double cw_fsk_shift_hz, int level, uint32_t ramp)
{
    int rc, u = 0, i;
    double fc = tone0_hz + PI4_CARRIER_OFFSET_HZ;
    if (!t) return TXE_ARG;
    if ((rc = pi4enc_symbols(msg, t->sym)) != 0) return rc;
    if ((rc = txs_mfsk_init(&t->s, t->sym, PI4_NSYM, PI4_SPS_NUM, PI4_SPS_DEN, tone0_hz,
                            PI4_TONE_SPACING_HZ, 4, 0, level, ramp)) != 0) return rc;
    if (cw_fsk_shift_hz < 0.0 || (cw_fsk_shift_hz > 0.0 && !(fc - cw_fsk_shift_hz > 0.0))) return TXE_ARG;
    for (i = 0; i < (int)sizeof t->key; i++) t->key[i] = 0;
    t->cw = cw_call != 0;
    if (t->cw) {
        if (!cw_call[0]) return TXE_ARG;
        if ((u = cw_word(t, cw_call, 0)) < 0) return u;
        if (cw_loc && cw_loc[0]) {
            int n = 0;
            while (cw_loc[n]) n++;
            if (n != 4 && n != 6) return TXE_GRID;
            for (i = 0; i < n; i++) {
                char c = up(cw_loc[i]);
                if (i < 2 ? (c < 'A' || c > 'R') : i < 4 ? (c < '0' || c > '9') : (c < 'A' || c > 'X'))
                    return TXE_GRID;
            }
            if ((u = cw_word(t, cw_loc, u)) < 0) return u;
        }
    }
    t->units = (uint16_t)u;
    t->fsk = cw_fsk_shift_hz > 0.0;
    t->n = 0;
    t->phase = 0;
    t->inc_c = txs_hz_to_inc(fc);
    t->inc_sp = txs_hz_to_inc(fc - cw_fsk_shift_hz);
    t->level = t->s.level;
    t->ramp = t->s.ramp;
    return 0;
}

uint32_t pi4_carrier_start_ms(const pi4_tx_t *t)
{
    return (t && t->cw) ? PI4_CW_START_MS + (uint32_t)t->units * PI4_CW_DOT_MS : 0;
}

int pi4_tx_fill(pi4_tx_t *t, int16_t *buf, int n)
{
    int w = 0;
    uint32_t end, car0;
    if (!t || !buf || n <= 0) return 0;
    end = t->cw ? T_END : PI4_FRAME_SAMPLES;
    car0 = CW_T0 + (uint32_t)t->units * UNIT;
    while (w < n && t->n < end) {
        uint32_t i = t->n;
        float g = 0.0f;
        uint32_t inc = t->inc_c;
        if (i < PI4_FRAME_SAMPLES) {
            int m = txs_mfsk_fill(&t->s, buf + w, (int)(PI4_FRAME_SAMPLES - i) < n - w ? (int)(PI4_FRAME_SAMPLES - i) : n - w);
            w += m;
            t->n += (uint32_t)m;
            continue;
        }
        if (i < CW_T0) {
            g = 0.0f;
        } else if (t->fsk) {                               /* F1A: continuous 25.0 .. 59.5 s */
            if (i < car0 && !keybit(t, (i - CW_T0) / UNIT)) inc = t->inc_sp;
            g = txs_ramp_gain(i - CW_T0, T_END - CW_T0, t->ramp);
        } else if (i < car0) {                             /* on/off CW, ramped per element */
            uint32_t u = (i - CW_T0) / UNIT, us = u, ue = u + 1;
            if (keybit(t, u)) {
                while (us > 0 && keybit(t, us - 1)) us--;
                while (keybit(t, ue)) ue++;
                g = txs_ramp_gain(i - CW_T0 - us * UNIT, (ue - us) * UNIT, t->ramp);
            }
        } else {
            g = txs_ramp_gain(i - car0, T_END - car0, t->ramp);
        }
        buf[w++] = g == 0.0f ? 0 : txs_q16(t->level * g * txs_sin(t->phase));
        t->phase += inc;
        t->n++;
    }
    return w;
}
