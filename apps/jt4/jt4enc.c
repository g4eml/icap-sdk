/* SPDX-License-Identifier: GPL-3.0-or-later
 * jt4enc.c - JT4 encoder, see jt4enc.h.
 * Derived from WSJT-X (C) K1JT et al., GPL-3.0: lib/gen4.f90, lib/encode4.f90, lib/packjt.f90
 * (packmsg/packtext/packcall), lib/fmtmsg.f90, lib/entail.f90, lib/encode232.f90 (after KA9Q),
 * lib/interleave4.f90, lib/jt4.f90 (npr sync vector).
 */
#include "jt4enc.h"

#define POLY1 0xf2d05351u
#define POLY2 0xe4613c47u

/* npr(2:207), MSB first */
static const uint8_t npr[26] = {
    0x18, 0xd9, 0x40, 0x60, 0x01, 0x6d, 0x7d, 0x12, 0x7c, 0x51, 0xec, 0x8d, 0x55,
    0xf5, 0x6a, 0xe5, 0xbc, 0x36, 0x3b, 0xb9, 0x1b, 0x23, 0xf3, 0x0c, 0x5b, 0xd4 };

static const char alpha42[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ +-./?";

static int is_d(char c) { return c >= '0' && c <= '9'; }
static int is_lsp(char c) { return (c >= 'A' && c <= 'Z') || c == ' '; }

static int code42(char c)
{
    int j;
    for (j = 0; j < 42; j++) if (alpha42[j] == c) return j;
    return -1;
}

static uint32_t parity32(uint32_t x)
{
    x ^= x >> 16; x ^= x >> 8; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1;
    return x & 1u;
}

/* WSJT-X packcall() succeeds on this 6-char field (i.e. it is a JT "callsign" token) */
static int packcall_ok(const char *w, int n)
{
    char c[6], t[6];
    int i;
    for (i = 0; i < 6; i++) c[i] = i < n ? w[i] : ' ';
    if (c[0] == '3' && c[1] == 'D' && c[2] == 'A' && c[3] == '0') return 1;  /* conservative */
    if (c[0] == '3' && c[1] == 'X' && c[2] >= 'A' && c[2] <= 'Z') return 1;   /* conservative */
    if (c[0] == 'C' && c[1] == 'Q' && c[2] == ' ') return 1;
    if (c[0] == 'Q' && c[1] == 'R' && c[2] == 'Z' && c[3] == ' ') return 1;
    if (c[0] == 'D' && c[1] == 'E' && c[2] == ' ') return 1;
    if (is_d(c[2])) for (i = 0; i < 6; i++) t[i] = c[i];
    else if (is_d(c[1])) {
        if (c[5] != ' ') return 0;
        t[0] = ' ';
        for (i = 0; i < 5; i++) t[i + 1] = c[i];
    } else return 0;
    return (is_lsp(t[0]) || is_d(t[0])) && (is_lsp(t[1]) || is_d(t[1])) && t[1] != ' ' &&
           is_d(t[2]) && is_lsp(t[3]) && is_lsp(t[4]) && is_lsp(t[5]);
}

int jt4_beacon_text(const char *call, const char *loc, char *out)
{
    int i = 0, j, n = 0;
    if (!call || !loc || !out) return TXE_ARG;
    while (loc[n]) n++;
    if (n != 4 && n != 6) return TXE_GRID;
    for (j = 0; call[j]; j++) { if (j >= 11) return TXE_LEN; out[i++] = call[j]; }
    out[i++] = ' ';
    for (j = 0; j < n; j++) {
        char c = loc[j];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (j < 2 ? (c < 'A' || c > 'R') : j < 4 ? !is_d(c) : (c < 'A' || c > 'X')) return TXE_GRID;
        if (j < 4) out[i++] = c;
    }
    out[i] = 0;
    for (j = 0; j < i; j++) if (out[j] >= 'a' && out[j] <= 'z') out[j] = (char)(out[j] - 32);
    return 0;
}

int jt4_encode(const char *msg, uint8_t tones[JT4_NSYM])
{
    char m[14];
    int n = 0, i, j, k, w1 = 0, w2s = -1, w2 = 0, w1slash = 0, w2slash = 0, inv = 0;
    uint32_t nc1 = 0, nc2 = 0, nc3 = 0, ng, st = 0;
    uint8_t d[13], sym[JT4_NSYM];
    if (!msg || !tones) return TXE_ARG;
    /* fmtmsg: upper case, collapse blanks (leading/trailing blanks dropped too) */
    for (i = 0; msg[i]; i++) {
        char c = msg[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c == ' ' && (n == 0 || m[n - 1] == ' ')) continue;
        if (code42(c) < 0) return TXE_CHAR;
        if (n >= 13) { if (c == ' ') continue; return TXE_LEN; }
        m[n++] = c;
    }
    while (n > 0 && m[n - 1] == ' ') n--;
    if (n == 0) return TXE_MSG;
    for (i = n; i < 13; i++) m[i] = ' ';
    m[13] = 0;
    /* refuse anything packmsg could treat as a standard / special message (always send text) */
    if (m[0] == 'C' && m[1] == 'Q' && m[2] == ' ') return TXE_MSG;
    if (n >= 4 && m[n - 4] == ' ' && m[n - 3] == 'O' && m[n - 2] == 'O' && m[n - 1] == 'O') return TXE_MSG;
    for (i = 0; i < n && m[i] != ' '; i++) if (m[i] == '/') w1slash = 1;
    w1 = i;
    if (i < n) {
        w2s = i + 1;
        for (j = w2s; j < n && m[j] != ' '; j++) if (m[j] == '/') w2slash = 1;
        w2 = j - w2s;
    }
    {
        int t2 = w2s < 0 || (!w2slash && !packcall_ok(m + w2s, w2 > 6 ? 6 : w2));
        int t1 = !w1slash && !packcall_ok(m, w1 > 6 ? 6 : w1);
        if (!t1 && !t2) return TXE_MSG;
    }
    /* gen4: '-' at position >= 9 (1-based) followed by 0..3 -> inverted sync */
    for (i = 0; i < n && m[i] != '-'; i++) ;
    if (i < n && i + 1 >= 9 && m[i + 1] >= '0' && m[i + 1] <= '3') inv = 1;
    /* packtext */
    for (i = 0; i < 5; i++) nc1 = nc1 * 42u + (uint32_t)code42(m[i]);
    for (i = 5; i < 10; i++) nc2 = nc2 * 42u + (uint32_t)code42(m[i]);
    for (i = 10; i < 13; i++) nc3 = nc3 * 42u + (uint32_t)code42(m[i]);
    nc1 = nc1 * 2u + ((nc3 >> 15) & 1u);
    nc2 = nc2 * 2u + ((nc3 >> 16) & 1u);
    ng = (nc3 & 32767u) + 32768u;
    /* 72 bits (nc1:28, nc2:28, ng:16) + zero tail, 8 bits per byte (entail) */
    for (i = 0; i < 13; i++) d[i] = 0;
    for (k = 0; k < 72; k++) {
        uint32_t bit = k < 28 ? (nc1 >> (27 - k)) & 1u : k < 56 ? (nc2 >> (55 - k)) & 1u : (ng >> (71 - k)) & 1u;
        if (bit) d[k >> 3] |= (uint8_t)(0x80u >> (k & 7));
    }
    /* encode232: 103 input bits -> 206 symbols */
    for (k = 0; k < 103; k++) {
        st = (st << 1) | ((d[k >> 3] >> (7 - (k & 7))) & 1u);
        sym[2 * k] = (uint8_t)parity32(st & POLY1);
        sym[2 * k + 1] = (uint8_t)parity32(st & POLY2);
    }
    /* interleave4 (ndir = 1): out[j0(i)] = in[i], j0 = bit-reversed 0..255 that are <= 205 */
    for (i = 0, k = 0; i < 256; i++) {
        int r = 0, b;
        for (b = 0; b < 8; b++) r |= ((i >> b) & 1) << (7 - b);
        if (r <= 205) {
            int s = (npr[r >> 3] >> (7 - (r & 7))) & 1;
            tones[r] = (uint8_t)(2 * sym[k] + (inv ? 1 - s : s));
            k++;
        }
    }
    return 0;
}

int jt4_tx_init(jt4_tx_t *t, const char *msg, double f0_hz, int submode, int level, uint32_t ramp)
{
    static const uint8_t nch[7] = { 1, 2, 4, 9, 18, 36, 72 };
    int rc;
    if (!t || submode < 0 || submode > 6) return TXE_ARG;
    if ((rc = jt4_encode(msg, t->tones)) != 0) return rc;
    return txs_mfsk_init(&t->s, t->tones, JT4_NSYM, JT4_SPS_NUM, JT4_SPS_DEN, f0_hz,
                         4.375 * (double)nch[submode], 4, 0, level, ramp);
}
