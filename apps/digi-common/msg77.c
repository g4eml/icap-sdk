/* SPDX-License-Identifier: GPL-3.0-or-later
 * msg77.c - WSJT-X 77-bit standard message packing (subset), see msg77.h.
 * Follows WSJT-X lib/77bit/packjt77.f90 (pack77, pack77_1, pack28) and lib/chkcall.f90,
 * (C) Joe Taylor K1JT et al., GPL-3.0; re-implemented in C for the ICAP encoders.
 */
#include "msg77.h"
#include "txsynth.h"

#define NTOKENS  2063592u
#define MAX22    4194304u
#define MAXGRID4 32400u

static int is_d(char c) { return c >= '0' && c <= '9'; }
static int is_l(char c) { return c >= 'A' && c <= 'Z'; }
static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* index of c in the WSJT-X alphabets, -1 if absent */
static int a1(char c) { return c == ' ' ? 0 : is_d(c) ? c - '0' + 1 : is_l(c) ? c - 'A' + 11 : -1; }
static int a2(char c) { return is_d(c) ? c - '0' : is_l(c) ? c - 'A' + 10 : -1; }
static int a4(char c) { return c == ' ' ? 0 : is_l(c) ? c - 'A' + 1 : -1; }

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

/* Standard call (+ optional /P or /R) -> n28.  *sfx = 0 none, 'P', 'R'.  0 or TXE_CALL. */
static int pack_call(const char *w, uint32_t *n28, char *sfx)
{
    char bc[7], cs[6];
    int n = slen(w), nb, i, i1, iarea, npdig = 0, nplet = 0, nslet = 0, k[6];
    *sfx = 0;
    if (n > 11) return TXE_CALL;
    if (n >= 4 && w[n - 2] == '/' && (w[n - 1] == 'P' || w[n - 1] == 'R')) {
        *sfx = w[n - 1];
        n -= 2;
    }
    if (n > 6 || n < 3) return TXE_CALL;
    for (i = 0; i < n; i++) {
        if (!is_d(w[i]) && !is_l(w[i])) return TXE_CALL;   /* '/' left = prefix/suffix WSJT-X would drop */
        bc[i] = w[i];
    }
    nb = n;
    bc[nb] = 0;
    /* chkcall */
    if (!is_l(bc[0]) && !is_l(bc[1])) return TXE_CALL;
    if (bc[0] == 'Q' && !seq(bc, "QU1RK")) return TXE_CALL;
    i1 = 0;
    if (is_d(bc[1])) i1 = 2;
    if (is_d(bc[2])) i1 = 3;
    if (i1 == 0 || i1 == nb) return TXE_CALL;
    for (i = i1; i < nb; i++) if (!is_l(bc[i])) return TXE_CALL;
    if (nb - i1 < 1 || nb - i1 > 3) return TXE_CALL;
    /* pack28 standard-call test (else WSJT-X would send a 22-bit hash) */
    for (iarea = nb; iarea >= 2; iarea--) if (is_d(bc[iarea - 1])) break;
    if (iarea < 2) iarea = 1;
    for (i = 0; i < iarea - 1; i++) { if (is_d(bc[i])) npdig++; if (is_l(bc[i])) nplet++; }
    for (i = iarea; i < nb; i++) if (is_l(bc[i])) nslet++;
    if (iarea < 2 || iarea > 3 || nplet == 0 || npdig >= iarea - 1 || nslet > 3) return TXE_CALL;
    for (i = 0; i < 6; i++) cs[i] = ' ';
    if (iarea == 2) { if (nb > 5) return TXE_CALL; for (i = 0; i < nb; i++) cs[i + 1] = bc[i]; }
    else for (i = 0; i < nb; i++) cs[i] = bc[i];
    k[0] = a1(cs[0]); k[1] = a2(cs[1]); k[2] = is_d(cs[2]) ? cs[2] - '0' : -1;
    k[3] = a4(cs[3]); k[4] = a4(cs[4]); k[5] = a4(cs[5]);
    for (i = 0; i < 6; i++) if (k[i] < 0) return TXE_CALL;
    *n28 = NTOKENS + MAX22 + (uint32_t)(((((k[0] * 36 + k[1]) * 10 + k[2]) * 27 + k[3]) * 27 + k[4]) * 27 + k[5]);
    return 0;
}

static int grid4(const char *g, uint32_t *ig)
{
    if (slen(g) != 4) return TXE_GRID;
    if (g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R' || !is_d(g[2]) || !is_d(g[3]))
        return TXE_GRID;
    *ig = (uint32_t)((((g[0] - 'A') * 18 + (g[1] - 'A')) * 10 + (g[2] - '0')) * 10 + (g[3] - '0'));
    return 0;
}

static void put(uint8_t *o, int *pos, uint32_t v, int nb)
{
    while (nb-- > 0) {
        if ((v >> nb) & 1u) o[*pos >> 3] |= (uint8_t)(0x80u >> (*pos & 7));
        (*pos)++;
    }
}

int msg77_pack(const char *msg, uint8_t out[10])
{
    char w[3][14];
    int nw = 0, len = 0, i, rc, pos = 0;
    uint32_t n28a, n28b, ig = MAXGRID4 + 1, i3 = 1, ipa = 0, ipb = 0;
    char sa = 0, sb = 0;
    if (!msg || !out) return TXE_ARG;
    for (i = 0; ; i++) {                       /* upper case, split on blanks */
        char c = up(msg[i]);
        if (c == ' ' || c == 0) {
            if (len) { w[nw][len] = 0; nw++; len = 0; }
            if (!c) break;
            continue;
        }
        if (nw >= 3 || len >= 13) return TXE_MSG;
        if (!is_d(c) && !is_l(c) && c != '/') return TXE_CHAR;
        w[nw][len++] = c;
    }
    if (nw < 2) return TXE_MSG;
    if (seq(w[0], "DE")) n28a = 0;
    else if (seq(w[0], "QRZ")) n28a = 1;
    else if (seq(w[0], "CQ")) n28a = 2;
    else if ((rc = pack_call(w[0], &n28a, &sa)) != 0) return rc;
    if ((rc = pack_call(w[1], &n28b, &sb)) != 0) return rc;
    if (nw == 2 && sb) return TXE_MSG;         /* WSJT-X: 2 words with "/" in call 2 is not type 1 */
    if (nw == 3 && (rc = grid4(w[2], &ig)) != 0) return rc;
    if ((sa == 'P' && sb == 'R') || (sa == 'R' && sb == 'P')) return TXE_CALL;
    if (sa == 'P' || sb == 'P') i3 = 2;
    ipa = sa != 0;
    ipb = sb != 0;
    for (i = 0; i < 10; i++) out[i] = 0;
    put(out, &pos, n28a, 28); put(out, &pos, ipa, 1);
    put(out, &pos, n28b, 28); put(out, &pos, ipb, 1);
    put(out, &pos, 0, 1);      /* ir */
    put(out, &pos, ig, 15);
    put(out, &pos, i3, 3);
    return 0;
}

int msg77_beacon_text(const char *prefix, const char *call, const char *loc, char *out)
{
    int i = 0, j, n;
    if (!prefix || !call || !loc || !out) return TXE_ARG;
    n = slen(loc);
    if (n != 4 && n != 6) return TXE_GRID;
    if (slen(call) > 11 || slen(prefix) > 3) return TXE_LEN;
    for (j = 0; prefix[j]; j++) out[i++] = up(prefix[j]);
    if (i) out[i++] = ' ';
    for (j = 0; call[j]; j++) out[i++] = up(call[j]);
    out[i++] = ' ';
    for (j = 0; j < 4; j++) out[i++] = up(loc[j]);
    out[i] = 0;
    if (n == 6) {
        char a = up(loc[4]), b = up(loc[5]);
        if (a < 'A' || a > 'X' || b < 'A' || b > 'X') return TXE_GRID;
    }
    {
        uint32_t ig;
        return grid4(out + i - 4, &ig);
    }
}
