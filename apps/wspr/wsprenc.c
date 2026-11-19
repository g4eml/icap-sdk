/* SPDX-License-Identifier: GPL-3.0-or-later
 * wsprenc.c - WSPR-2 transmit encoder (channel symbols + streaming 8 kHz synth).
 *
 * The message-packing (pack_call / pack_grid4_power / pack_prefix / nhash /
 * convolutional encode / bit-reversal interleave / sync vector) reproduces
 * WSJT-X get_wspr_channel_symbols() (wsprsim_utils.c, Copyright 2014-2018
 * Steven Franke K9AN / Joe Taylor K1JT, GPL-3.0).  This module is therefore
 * GPL-3.0-or-later.  Rewritten freestanding: no libc, no malloc, no libm.
 */
#include "wsprenc.h"

/* ---- tiny freestanding helpers ---------------------------------------- */
static int w_is_space(char c) { return c == ' ' || c == '\t'; }
static int w_is_digit(char c) { return c >= '0' && c <= '9'; }
static char w_up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static int w_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* parse a (possibly signed) integer; stops at first non-digit. */
static int w_atoi(const char *s)
{
    int sign = 1, v = 0;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (w_is_digit(*s)) { v = v * 10 + (*s - '0'); s++; }
    return sign * v;
}

/* ---- Jenkins lookup3 hash (WSJT-X nhash), byte-safe port -------------- */
#define W_ROT(x, k) (((x) << (k)) | ((x) >> (32 - (k))))
static uint32_t w_le32(const unsigned char *p, int n) /* n bytes, little-endian */
{
    uint32_t v = 0; int i;
    for (i = 0; i < n; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}
static uint32_t w_nhash(const char *key, int length)
{
    uint32_t a, b, c;
    const unsigned char *k = (const unsigned char *)key;
    a = b = c = 0xdeadbeef + (uint32_t)length + 146u; /* initval = 146 */
    while (length > 12) {
        a += w_le32(k, 4); b += w_le32(k + 4, 4); c += w_le32(k + 8, 4);
        a -= c; a ^= W_ROT(c, 4);  c += b;
        b -= a; b ^= W_ROT(a, 6);  a += c;
        c -= b; c ^= W_ROT(b, 8);  b += a;
        a -= c; a ^= W_ROT(c, 16); c += b;
        b -= a; b ^= W_ROT(a, 19); a += c;
        c -= b; c ^= W_ROT(b, 4);  b += a;
        length -= 12; k += 12;
    }
    switch (length) {
    case 12: c += w_le32(k + 8, 4); b += w_le32(k + 4, 4); a += w_le32(k, 4); break;
    case 11: c += w_le32(k + 8, 3); b += w_le32(k + 4, 4); a += w_le32(k, 4); break;
    case 10: c += w_le32(k + 8, 2); b += w_le32(k + 4, 4); a += w_le32(k, 4); break;
    case 9:  c += w_le32(k + 8, 1); b += w_le32(k + 4, 4); a += w_le32(k, 4); break;
    case 8:  b += w_le32(k + 4, 4); a += w_le32(k, 4); break;
    case 7:  b += w_le32(k + 4, 3); a += w_le32(k, 4); break;
    case 6:  b += w_le32(k + 4, 2); a += w_le32(k, 4); break;
    case 5:  b += w_le32(k + 4, 1); a += w_le32(k, 4); break;
    case 4:  a += w_le32(k, 4); break;
    case 3:  a += w_le32(k, 3); break;
    case 2:  a += w_le32(k, 2); break;
    case 1:  a += w_le32(k, 1); break;
    case 0:  return c & 32767u;
    }
    c ^= b; c -= W_ROT(b, 14);
    a ^= c; a -= W_ROT(c, 11);
    b ^= a; b -= W_ROT(a, 25);
    c ^= b; c -= W_ROT(b, 16);
    a ^= c; a -= W_ROT(c, 4);
    b ^= a; b -= W_ROT(a, 14);
    c ^= b; c -= W_ROT(b, 24);
    return c & 32767u;
}

/* ---- WSPR character codes (per WSJT-X) --------------------------------- */
static int loc_code(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch == ' ') return 36;
    if (ch >= 'A' && ch <= 'R') return ch - 'A';
    return -1;
}
static int call_code(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch == ' ') return 36;
    if (ch >= 'A' && ch <= 'Z') return ch - 'A' + 10;
    return -1;
}

/* pack a standard call (<=6 chars) into a 28-bit number, or -1 on failure. */
static long w_pack_call(const char *callsign)
{
    char call6[6]; int i, cc[6]; long n;
    int len = w_len(callsign);
    if (len > 6 || len < 3) return -1;
    for (i = 0; i < 6; i++) call6[i] = ' ';
    if (w_is_digit(callsign[2])) {
        for (i = 0; i < len; i++) call6[i] = callsign[i];
    } else if (w_is_digit(callsign[1])) {
        for (i = 0; i < len; i++) call6[i + 1] = callsign[i];
    } else {
        return -1; /* no digit in position 2 or 3: not a standard call */
    }
    for (i = 0; i < 6; i++) { cc[i] = call_code(call6[i]); if (cc[i] < 0) return -1; }
    if (cc[2] > 9) return -1;            /* position 3 must be a digit */
    if (cc[0] > 36 || cc[1] > 36) return -1;
    if (cc[3] < 10 || cc[4] < 10 || cc[5] < 10) return -1; /* must be letter/space */
    n = cc[0];
    n = n * 36 + cc[1];
    n = n * 10 + cc[2];
    n = n * 27 + cc[3] - 10;
    n = n * 27 + cc[4] - 10;
    n = n * 27 + cc[5] - 10;
    return n;
}

static long w_pack_grid4_power(const char *grid4, int power)
{
    int g[4], i; long m;
    for (i = 0; i < 4; i++) { g[i] = loc_code(grid4[i]); if (g[i] < 0) return -1; }
    if (g[0] > 17 || g[1] > 17) return -1;   /* field letters A..R (chars 0,1) */
    if (g[2] > 9 || g[3] > 9) return -1;      /* squares 0..9 (chars 2,3)       */
    m = (long)(179 - 10 * g[0] - g[2]) * 180 + 10 * g[1] + g[3];
    m = m * 128 + power + 64;
    return m;
}

/* power dBm rounded to the WSPR allowed set offset (type 2/3). */
static int w_pow_adjust(int power)
{
    static const int nu[10] = {0, -1, 1, 0, -1, 2, 1, 0, -1, 1};
    if (power < 0) power = 0;
    if (power > 60) power = 60;
    return power + nu[power % 10];
}

/* pack_prefix: fills *n (28-bit call), *m (prefix/suffix code), *nadd.
 * callsign is the compound token "PFX/CALL" or "CALL/SFX" (NUL terminated).
 * Returns 0 ok, -1 on failure. */
static int w_pack_prefix(const char *callsign, long *n, int *m, int *nadd)
{
    char buf[16]; int i, i1, clen = w_len(callsign);
    if (clen >= (int)sizeof(buf)) return -1;
    for (i = 0; i < clen; i++) buf[i] = callsign[i];
    buf[clen] = 0;
    i1 = 0; while (buf[i1] && buf[i1] != '/') i1++;
    if (!buf[i1]) return -1;

    if (buf[i1 + 1] && buf[i1 + 2] == 0) {
        /* single-char suffix:  CALL/x */
        char call6[8]; long nn; int nc;
        for (i = 0; i < i1; i++) call6[i] = buf[i];
        call6[i1] = 0;
        nn = w_pack_call(call6); if (nn < 0) return -1;
        *n = nn; *nadd = 1;
        nc = buf[i1 + 1];
        if (nc >= '0' && nc <= '9') *m = nc - '0';
        else if (nc >= 'A' && nc <= 'Z') *m = nc - 'A' + 10;
        else *m = 38;
        *m = 60000 - 32768 + *m;
    } else if (buf[i1 + 1] && buf[i1 + 2] && buf[i1 + 3] == 0) {
        /* two-char suffix:  CALL/xy  (digits) */
        char call6[8]; long nn;
        for (i = 0; i < i1; i++) call6[i] = buf[i];
        call6[i1] = 0;
        nn = w_pack_call(call6); if (nn < 0) return -1;
        *n = nn; *nadd = 1;
        *m = 10 * (buf[i1 + 1] - '0') + (buf[i1 + 2] - '0');
        *m = 60000 + 26 + *m;
    } else {
        /* prefix form:  PFX/CALL */
        char pfx[8], call[12]; long nn; int plen = i1, mm;
        if (plen >= (int)sizeof(pfx)) return -1;
        for (i = 0; i < plen; i++) pfx[i] = buf[i];
        pfx[plen] = 0;
        { int j = 0; for (i = i1 + 1; buf[i]; i++) call[j++] = buf[i]; call[j] = 0; }
        nn = w_pack_call(call); if (nn < 0) return -1;
        *n = nn;
        if (plen == 1) { mm = 36; mm = 37 * mm + 36; }
        else if (plen == 2) mm = 36;
        else mm = 0;
        for (i = 0; i < plen; i++) {
            int nc = pfx[i];
            if (nc >= '0' && nc <= '9') nc = nc - '0';
            else if (nc >= 'A' && nc <= 'Z') nc = nc - 'A' + 10;
            else nc = 36;
            mm = 37 * mm + nc;
        }
        *nadd = 0;
        if (mm > 32768) { mm = mm - 32768; *nadd = 1; }
        *m = mm;
    }
    return 0;
}

/* ---- convolutional code + interleave + sync vector -------------------- */
#define WSPR_POLY1 0xF2D05351u
#define WSPR_POLY2 0xE4613C47u

static const uint8_t wspr_pr3[WSPR_NSYM] = {
    1,1,0,0,0,0,0,0,1,0,0,0,1,1,1,0,0,0,1,0,
    0,1,0,1,1,1,1,0,0,0,0,0,0,0,1,0,0,1,0,1,
    0,0,0,0,0,0,1,0,1,1,0,0,1,1,0,1,0,0,0,1,
    1,0,1,0,0,0,0,1,1,0,1,0,1,0,1,0,1,0,0,1,
    0,0,1,0,1,1,0,0,0,1,1,0,1,0,1,0,0,0,1,0,
    0,0,0,0,1,0,0,1,0,0,1,1,1,0,1,1,0,0,1,1,
    0,1,0,0,0,1,1,1,0,0,0,0,0,1,0,1,0,0,1,1,
    0,0,0,0,0,0,0,1,1,0,1,0,1,1,0,0,0,1,1,0,
    0,0};

static uint32_t w_parity(uint32_t v)
{
    v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return v & 1u;
}

/* 11 data bytes -> 176 code bits (we keep first 162). */
static void w_conv_encode(const uint8_t *data, uint8_t *bits)
{
    uint32_t enc = 0; int b, i, o = 0;
    for (b = 0; b < 11; b++) {
        for (i = 7; i >= 0; i--) {
            enc = (enc << 1) | ((data[b] >> i) & 1u);
            bits[o++] = (uint8_t)w_parity(enc & WSPR_POLY1);
            bits[o++] = (uint8_t)w_parity(enc & WSPR_POLY2);
        }
    }
}

/* bit-reversal interleave of the 162 code bits, in place. */
static void w_interleave(uint8_t *sym)
{
    uint8_t tmp[162]; unsigned p = 0, i = 0, j;
    while (p < 162) {
        j = (uint8_t)(((i * 0x80200802ULL) & 0x0884422110ULL) * 0x0101010101ULL >> 32);
        if (j < 162) { tmp[j] = sym[p]; p++; }
        i++;
    }
    for (i = 0; i < 162; i++) sym[i] = tmp[i];
}

/* pack n (28-bit) and m (22-bit) into the 11-byte data vector (31 tail = 0). */
static void w_pack_data(long n, long m, uint8_t data[11])
{
    int i;
    for (i = 0; i < 11; i++) data[i] = 0;
    data[0] = (uint8_t)(0xFF & (n >> 20));
    data[1] = (uint8_t)(0xFF & (n >> 12));
    data[2] = (uint8_t)(0xFF & (n >> 4));
    data[3] = (uint8_t)(((n & 0x0F) << 4) + ((m >> 18) & 0x0F));
    data[4] = (uint8_t)(0xFF & (m >> 10));
    data[5] = (uint8_t)(0xFF & (m >> 2));
    data[6] = (uint8_t)((m & 0x03) << 6);
}

static void w_finish(const uint8_t data[11], uint8_t tones[WSPR_NSYM])
{
    uint8_t bits[176]; int i;
    w_conv_encode(data, bits);
    w_interleave(bits);
    for (i = 0; i < WSPR_NSYM; i++) tones[i] = (uint8_t)(2 * bits[i] + wspr_pr3[i]);
}

/* ---- message parsing / type dispatch --------------------------------- */
int wspr_pack_message(const char *msg, uint8_t tones[WSPR_NSYM])
{
    char message[32]; int mlen = 0, i;
    int i1, i2, i3, i4;
    if (!msg || !tones) return TXE_ARG;
    /* copy + upper-case, collapse nothing (match ref which keeps as given but
     * callers pass canonical upper-case). */
    while (msg[mlen] && mlen < 31) { message[mlen] = w_up(msg[mlen]); mlen++; }
    message[mlen] = 0;
    if (mlen < 3) return TXE_MSG;

    i1 = 0; while (message[i1] && !w_is_space(message[i1])) i1++;       /* first space */
    i2 = 0; while (message[i2] && message[i2] != '/') i2++;              /* first '/'   */
    i3 = 0; while (message[i3] && message[i3] != '<') i3++;              /* first '<'   */
    i4 = 0; while (message[i4] && message[i4] != '>') i4++;              /* first '>'   */

    /* split into up to 3 space-separated tokens */
    char tok[3][24]; int nt = 0, p = 0;
    for (;;) {
        while (message[p] && w_is_space(message[p])) p++;
        if (!message[p] || nt >= 3) break;
        int q = 0;
        while (message[p] && !w_is_space(message[p]) && q < 23) tok[nt][q++] = message[p++];
        tok[nt][q] = 0; nt++;
    }

    if (i1 >= 3 && i1 < 7 && i2 == mlen && i3 == mlen) {
        /* Type 1: CALL GRID4 PWR */
        long n, m;
        if (nt < 3) return TXE_MSG;
        if (w_len(tok[1]) != 4) return TXE_GRID;
        n = w_pack_call(tok[0]); if (n < 0) return TXE_CALL;
        m = w_pack_grid4_power(tok[1], w_atoi(tok[2])); if (m < 0) return TXE_GRID;
        { uint8_t data[11]; w_pack_data(n, m, data); w_finish(data, tones); }
        return 0;
    } else if (i3 == 0 && i4 < mlen) {
        /* Type 3: <CALL> GRID6 PWR */
        long n, m; int power, ntype, ihash, j, glen;
        char hc[16], grid6[8];
        if (nt < 3) return TXE_MSG;
        /* token 0 is "<CALL>" -> strip < > */
        { int s = 0, d = 0; const char *t = tok[0];
          if (t[0] == '<') s = 1;
          while (t[s] && t[s] != '>' && d < 15) hc[d++] = t[s++];
          hc[d] = 0; }
        if (w_len(hc) < 1) return TXE_CALL;
        power = w_pow_adjust(w_atoi(tok[2]));
        ntype = -(power + 1);
        ihash = (int)w_nhash(hc, w_len(hc));
        m = 128L * ihash + ntype + 64;
        glen = w_len(tok[1]);
        if (glen < 1) return TXE_GRID;
        for (i = 0; i < 8; i++) grid6[i] = 0;
        for (j = 0; j < glen - 1 && j < 6; j++) grid6[j] = tok[1][j + 1];
        grid6[5] = tok[1][0];
        n = w_pack_call(grid6); if (n < 0) return TXE_GRID;
        { uint8_t data[11]; w_pack_data(n, m, data); w_finish(data, tones); }
        return 0;
    } else if (i2 < mlen) {
        /* Type 2: compound/portable call + power */
        long n, m; int power, ntype, ng, nadd;
        if (nt < 2) return TXE_MSG;
        if (i2 == 0 || i2 > w_len(tok[0])) return TXE_CALL;
        power = w_pow_adjust(w_atoi(tok[1]));
        if (w_pack_prefix(tok[0], &n, &ng, &nadd) != 0) return TXE_CALL;
        ntype = power + 1 + nadd;
        m = 128L * ng + ntype + 64;
        { uint8_t data[11]; w_pack_data(n, m, data); w_finish(data, tones); }
        return 0;
    }
    return TXE_MSG;
}

int wspr_encode(const char *call, const char *grid4, int dbm, uint8_t tones[WSPR_NSYM])
{
    char msg[32]; int p = 0, i;
    if (!call || !grid4) return TXE_ARG;
    for (i = 0; call[i] && p < 20; i++) msg[p++] = w_up(call[i]);
    msg[p++] = ' ';
    for (i = 0; grid4[i] && p < 27; i++) msg[p++] = w_up(grid4[i]);
    msg[p++] = ' ';
    /* append power as decimal */
    { int v = dbm, d, q = p; char tmp[8]; int tn = 0;
      if (v < 0) { msg[p++] = '-'; v = -v; }
      if (v == 0) tmp[tn++] = '0';
      while (v > 0 && tn < 7) { tmp[tn++] = (char)('0' + v % 10); v /= 10; }
      for (d = tn - 1; d >= 0; d--) msg[p++] = tmp[d];
      (void)q;
    }
    msg[p] = 0;
    return wspr_pack_message(msg, tones);
}

/* ---- streaming synth -------------------------------------------------- */
int wspr_tx_init_msg(wspr_tx_t *t, const char *msg, double f0_hz, int level, uint32_t ramp)
{
    int rc;
    if (!t) return TXE_ARG;
    rc = wspr_pack_message(msg, t->tones);
    if (rc != 0) return rc;
    if (f0_hz <= 0.0) f0_hz = WSPR_F0_DEFAULT_HZ;
    /* f0_hz is the CENTER of the 4 tones (WSPR convention: reported freq = center);
     * tone 0 (lowest) sits 1.5 tone-spacings below it. */
    return txs_mfsk_init(&t->s, t->tones, WSPR_NSYM, WSPR_SPS_NUM, WSPR_SPS_DEN,
                         f0_hz - 1.5 * WSPR_TONE_SPACING_HZ, WSPR_TONE_SPACING_HZ, 4, 0, level, ramp);
}

int wspr_tx_init(wspr_tx_t *t, const char *call, const char *grid4, int dbm,
                 double f0_hz, int level, uint32_t ramp)
{
    int rc;
    if (!t) return TXE_ARG;
    rc = wspr_encode(call, grid4, dbm, t->tones);
    if (rc != 0) return rc;
    if (f0_hz <= 0.0) f0_hz = WSPR_F0_DEFAULT_HZ;
    /* f0_hz is the CENTER of the 4 tones (WSPR convention: reported freq = center);
     * tone 0 (lowest) sits 1.5 tone-spacings below it. */
    return txs_mfsk_init(&t->s, t->tones, WSPR_NSYM, WSPR_SPS_NUM, WSPR_SPS_DEN,
                         f0_hz - 1.5 * WSPR_TONE_SPACING_HZ, WSPR_TONE_SPACING_HZ, 4, 0, level, ramp);
}

int wspr_tx_fill(wspr_tx_t *t, int16_t *buf, int n) { return txs_mfsk_fill(&t->s, buf, n); }

uint32_t wspr_ms_to_next_start(uint32_t utc_s, uint32_t ms)
{
    return txs_ms_to_next_start(utc_s, ms, WSPR_PERIOD_MS, WSPR_TX_START_MS, 2000u);
}
