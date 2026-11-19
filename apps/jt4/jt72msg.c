/* SPDX-License-Identifier: GPL-3.0-or-later
 * jt72msg.c - unpack the 72-bit JT4/JT9/JT65 message (port of WSJT-X lib/packjt.f90:
 * unpackmsg, unpackcall, unpackgrid, unpacktext, getpfx2, grid2k, deg2grid).
 * Derived from WSJT-X (C) Joe Taylor K1JT et al., GPL-3.0.
 */
#include "jt72msg.h"

#define NBASE 262177560   /* 37*36*10*27*27*27 */
#define NGBASE 32400      /* 180*180 */

static const char pfx_tab[] =
  "1A\0""1S\0""3A\0""3B6\0""3B8\0""3B9\0""3C\0""3C0\0""3D2\0""3D2C\0""3D2R\0""3DA\0""3V\0""3W\0"
  "3X\0""3Y\0""3YB\0""3YP\0""4J\0""4L\0""4S\0""4U1I\0""4U1U\0""4W\0""4X\0""5A\0""5B\0""5H\0"
  "5N\0""5R\0""5T\0""5U\0""5V\0""5W\0""5X\0""5Z\0""6W\0""6Y\0""7O\0""7P\0""7Q\0""7X\0""8P\0"
  "8Q\0""8R\0""9A\0""9G\0""9H\0""9J\0""9K\0""9L\0""9M2\0""9M6\0""9N\0""9Q\0""9U\0""9V\0""9X\0"
  "9Y\0""A2\0""A3\0""A4\0""A5\0""A6\0""A7\0""A9\0""AP\0""BS7\0""BV\0""BV9\0""BY\0""C2\0""C3\0"
  "C5\0""C6\0""C9\0""CE\0""CE0X\0""CE0Y\0""CE0Z\0""CE9\0""CM\0""CN\0""CP\0""CT\0""CT3\0""CU\0"
  "CX\0""CY0\0""CY9\0""D2\0""D4\0""D6\0""DL\0""DU\0""E3\0""E4\0""EA\0""EA6\0""EA8\0""EA9\0"
  "EI\0""EK\0""EL\0""EP\0""ER\0""ES\0""ET\0""EU\0""EX\0""EY\0""EZ\0""F\0""FG\0""FH\0""FJ\0"
  "FK\0""FKC\0""FM\0""FO\0""FOA\0""FOC\0""FOM\0""FP\0""FR\0""FRG\0""FRJ\0""FRT\0""FT5W\0"
  "FT5X\0""FT5Z\0""FW\0""FY\0""M\0""MD\0""MI\0""MJ\0""MM\0""MU\0""MW\0""H4\0""H40\0""HA\0""HB\0"
  "HB0\0""HC\0""HC8\0""HH\0""HI\0""HK\0""HK0\0""HK0M\0""HL\0""HM\0""HP\0""HR\0""HS\0""HV\0"
  "HZ\0""I\0""IS\0""IS0\0""J2\0""J3\0""J5\0""J6\0""J7\0""J8\0""JA\0""JDM\0""JDO\0""JT\0""JW\0"
  "JX\0""JY\0""K\0""KG4\0""KH0\0""KH1\0""KH2\0""KH3\0""KH4\0""KH5\0""KH5K\0""KH6\0""KH7\0"
  "KH8\0""KH9\0""KL\0""KP1\0""KP2\0""KP4\0""KP5\0""LA\0""LU\0""LX\0""LY\0""LZ\0""OA\0""OD\0"
  "OE\0""OH\0""OH0\0""OJ0\0""OK\0""OM\0""ON\0""OX\0""OY\0""OZ\0""P2\0""P4\0""PA\0""PJ2\0""PJ7\0"
  "PY\0""PY0F\0""PT0S\0""PY0T\0""PZ\0""R1F\0""R1M\0""S0\0""S2\0""S5\0""S7\0""S9\0""SM\0""SP\0"
  "ST\0""SU\0""SV\0""SVA\0""SV5\0""SV9\0""T2\0""T30\0""T31\0""T32\0""T33\0""T5\0""T7\0""T8\0"
  "T9\0""TA\0""TF\0""TG\0""TI\0""TI9\0""TJ\0""TK\0""TL\0""TN\0""TR\0""TT\0""TU\0""TY\0""TZ\0"
  "UA\0""UA2\0""UA9\0""UK\0""UN\0""UR\0""V2\0""V3\0""V4\0""V5\0""V6\0""V7\0""V8\0""VE\0""VK\0"
  "VK0H\0""VK0M\0""VK9C\0""VK9L\0""VK9M\0""VK9N\0""VK9W\0""VK9X\0""VP2E\0""VP2M\0""VP2V\0"
  "VP5\0""VP6\0""VP6D\0""VP8\0""VP8G\0""VP8H\0""VP8O\0""VP8S\0""VP9\0""VQ9\0""VR\0""VU\0""VU4\0"
  "VU7\0""XE\0""XF4\0""XT\0""XU\0""XW\0""XX9\0""XZ\0""YA\0""YB\0""YI\0""YJ\0""YK\0""YL\0""YN\0"
  "YO\0""YS\0""YU\0""YV\0""YV0\0""Z2\0""Z3\0""ZA\0""ZB\0""ZC4\0""ZD7\0""ZD8\0""ZD9\0""ZF\0"
  "ZK1N\0""ZK1S\0""ZK2\0""ZK3\0""ZL\0""ZL7\0""ZL8\0""ZL9\0""ZP\0""ZS\0""ZS8\0""KC4\0""E5\0"
  ;
#define NZ 339
static const char sfx_tab[12] = { 'P','0','1','2','3','4','5','6','7','8','9','A' };
static const char c37[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ ";
static const char c42[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ +-./?";

/* fixed-width Fortran-style strings (space padded, NUL at [w]) */
static void sfill(char *s, int w) { int i; for (i = 0; i < w; i++) s[i] = ' '; s[w] = 0; }
static int slen_trim(const char *s, int w) { while (w > 0 && s[w - 1] == ' ') w--; return w; }
/* d(w) = a(1:na) // b ... helper: append n chars of src to dst at *pos (bounded by w) */
static void sapp(char *d, int w, int *pos, const char *src, int n)
{
    int i;
    for (i = 0; i < n && *pos < w; i++) d[(*pos)++] = src[i];
}

static void unpackcall(int32_t n, char word[13], int *iv2, char psfx[5])
{
    int i;
    sfill(word, 12); sfill(psfx, 4);
    for (i = 0; i < 6; i++) word[i] = '.';
    *iv2 = 0;
    if (n < 262177560) {
        word[5] = c37[n % 27 + 10]; n /= 27;
        word[4] = c37[n % 27 + 10]; n /= 27;
        word[3] = c37[n % 27 + 10]; n /= 27;
        word[2] = c37[n % 10]; n /= 10;
        word[1] = c37[n % 36]; n /= 36;
        word[0] = c37[n];
        for (i = 0; i < 4; i++) if (word[i] != ' ') break;
        if (i < 4 && i > 0) {
            int j;
            for (j = 0; j < 12 - i; j++) word[j] = word[j + i];
            for (; j < 12; j++) word[j] = ' ';
        }
    } else if (n < 267796946) {
        int ps = 0, base = 0;
        if (n >= 262178563 && n <= 264002071) { *iv2 = 1; base = 262178563; ps = 4; }
        else if (n >= 264002072 && n <= 265825580) { *iv2 = 2; base = 264002072; ps = 4; }
        else if (n >= 265825581 && n <= 267649089) { *iv2 = 3; base = 265825581; ps = 4; }
        else if (n >= 267649090 && n <= 267698374) { *iv2 = 4; base = 267649090; ps = 3; }
        else if (n >= 267698375 && n <= 267747659) { *iv2 = 5; base = 267698375; ps = 3; }
        else if (n >= 267747660 && n <= 267796944) { *iv2 = 6; base = 267747660; ps = 3; }
        else if (n == 267796945) { *iv2 = 7; }
        if (ps) {
            n -= base;
            for (i = ps - 1; i > 0; i--) { psfx[i] = c37[n % 37]; n /= 37; }
            psfx[0] = c37[n];
        }
    }
    if (word[0] == '3' && word[1] == 'D' && word[2] == '0') {      /* '3DA0'//word(4:) */
        int j;
        for (j = 11; j > 3; j--) word[j] = word[j - 1];
        word[2] = 'A'; word[3] = '0';
    }
    if (word[0] == 'Q' && word[1] >= 'A' && word[1] <= 'Z') {      /* '3X'//word(2:) */
        int j;
        for (j = 11; j > 1; j--) word[j] = word[j - 1];
        word[0] = '3'; word[1] = 'X';
    }
}

/* Fortran write(...,'(i3.2)') into 3 chars */
static void fmt_i32(char *d, int n)
{
    int a = n < 0 ? -n : n;
    d[0] = n < 0 ? '-' : ' ';
    if (a >= 100) { d[0] = (char)('0' + a / 100 % 10); }
    d[1] = (char)('0' + (a / 10) % 10);
    d[2] = (char)('0' + a % 10);
}

static void unpackgrid(int32_t ng, char grid[5])
{
    sfill(grid, 4);
    if (ng < NGBASE) {
        int dlat = ng % 180 - 90;
        int dlong = (ng / 180) * 2 - 180 + 2;
        int nlong, nlat, n1, n2;
        if (dlong < -180) dlong += 360;
        if (dlong > 180) dlong -= 360;
        nlong = 12 * (180 - dlong);
        n1 = nlong / 240; n2 = (nlong - 240 * n1) / 24;
        grid[0] = (char)('A' + n1); grid[2] = (char)('0' + n2);
        nlat = 24 * (dlat + 90);
        n1 = nlat / 240; n2 = (nlat - 240 * n1) / 24;
        grid[1] = (char)('A' + n1); grid[3] = (char)('0' + n2);
        if (grid[0] == 'K' && grid[1] == 'A') {
            int n = (grid[2] - '0') * 10 + (grid[3] - '0') - 50;
            sfill(grid, 4); fmt_i32(grid, n);
            if (grid[0] == ' ') grid[0] = '+';
        } else if (grid[0] == 'L' && grid[1] == 'A') {
            int n = (grid[2] - '0') * 10 + (grid[3] - '0') - 50;
            sfill(grid, 4); grid[0] = 'R'; fmt_i32(grid + 1, n);
            if (grid[1] == ' ') grid[1] = '+';
        }
        return;
    }
    {
        int n = ng - NGBASE - 1;
        if (n >= 1 && n <= 30) fmt_i32(grid, -n);
        else if (n >= 31 && n <= 60) { grid[0] = 'R'; fmt_i32(grid + 1, -(n - 30)); }
        else if (n == 61) { grid[0] = 'R'; grid[1] = 'O'; }
        else if (n == 62) { grid[0] = 'R'; grid[1] = 'R'; grid[2] = 'R'; }
        else if (n == 63) { grid[0] = '7'; grid[1] = '3'; }
    }
}

static int nint_f(float x) { return x >= 0 ? (int)(x + 0.5f) : -(int)(-x + 0.5f); }

/* grid2k(grid//'ma') */
static int grid2k(const char grid[4])
{
    int g1 = grid[0], g2 = grid[1], g3 = grid[2], g4 = grid[3];
    if (g1 >= 'a' && g1 <= 'z') g1 += 'A' - 'a';
    if (g2 >= 'a' && g2 <= 'z') g2 += 'A' - 'a';
    {
        int nlong = 180 - 20 * (g1 - 'A');
        int n20d = 2 * (g3 - '0');
        float dlong = (float)nlong - (float)n20d - 62.5f / 60.0f;
        int nlat = -90 + 10 * (g2 - 'A') + g4 - '0';
        float dlat = (float)nlat + 1.25f / 60.0f;
        int il = nint_f(dlong), ia = nint_f(dlat);
        if (ia >= 85) return 5 * (il + 179) / 2 + ia - 84;
    }
    return 0;
}

static void getpfx2(int k0, char cs[13])
{
    int k = k0 > 450 ? k0 - 450 : k0;
    if (k >= 1 && k <= NZ) {
        const char *p = pfx_tab;
        char tmp[13];
        int i, pos = 0;
        for (i = 1; i < k; i++) { while (*p) p++; p++; }
        sfill(tmp, 12);
        while (*p) sapp(tmp, 12, &pos, p++, 1);
        sapp(tmp, 12, &pos, "/", 1);
        sapp(tmp, 12, &pos, cs, 12);
        for (i = 0; i < 13; i++) cs[i] = tmp[i];
    } else if (k >= 401 && k <= 400 + 12) {
        int iz = 0;
        while (iz < 12 && cs[iz] != ' ') iz++;
        {
            char tmp[13];
            int pos = 0, i;
            sfill(tmp, 12);
            sapp(tmp, 12, &pos, cs, iz);
            sapp(tmp, 12, &pos, "/", 1);
            sapp(tmp, 12, &pos, &sfx_tab[k - 401], 1);
            for (i = 0; i < 13; i++) cs[i] = tmp[i];
        }
    }
    /* k==449 (operator's own add-on prefix) is not available on the radio: leave as is */
}

static void unpacktext(int32_t nc1, int32_t nc2, int32_t nc3, char msg[23])
{
    int i;
    nc3 &= 32767;
    if (nc1 & 1) nc3 += 32768;
    nc1 /= 2;
    if (nc2 & 1) nc3 += 65536;
    nc2 /= 2;
    for (i = 4; i >= 0; i--) { msg[i] = c42[nc1 % 42]; nc1 /= 42; }
    for (i = 9; i >= 5; i--) { msg[i] = c42[nc2 % 42]; nc2 /= 42; }
    for (i = 12; i >= 10; i--) { msg[i] = c42[nc3 % 42]; nc3 /= 42; }
    for (i = 13; i < 22; i++) msg[i] = ' ';
}

int jt72_unpack(const uint8_t b[9], char out[23])
{
    int32_t dat[12], nc1, nc2, ng;
    char msg[23], c1[13], c2[13], grid[5], psfx[5], junk[5];
    int i, iv2, junk1, k = 0, j;
    for (i = 0; i < 12; i++) {           /* 9 bytes -> 12 six-bit words */
        int bit = i * 6, v = 0, q;
        for (q = 0; q < 6; q++, bit++) v = (v << 1) | ((b[bit >> 3] >> (7 - (bit & 7))) & 1);
        dat[i] = v;
    }
    nc1 = (dat[0] << 22) + (dat[1] << 16) + (dat[2] << 10) + (dat[3] << 4) + ((dat[4] >> 2) & 15);
    nc2 = ((dat[4] & 3) << 26) + (dat[5] << 20) + (dat[6] << 14) + (dat[7] << 8) + (dat[8] << 2) + ((dat[9] >> 4) & 3);
    ng = ((dat[9] & 15) << 12) + (dat[10] << 6) + dat[11];
    sfill(msg, 22);

    if (ng >= 32768) {
        unpacktext(nc1, nc2, ng, msg);
        goto post;
    }
    {
        int cqnnn = 0;
        unpackcall(nc1, c1, &iv2, psfx);
        if (iv2 == 0) {
            int nfreq;
            if (nc1 == NBASE + 1) { sfill(c1, 12); c1[0] = 'C'; c1[1] = 'Q'; }
            if (nc1 == NBASE + 2) { sfill(c1, 12); c1[0] = 'Q'; c1[1] = 'R'; c1[2] = 'Z'; }
            nfreq = nc1 - NBASE - 3;
            if (nfreq >= 0 && nfreq <= 999) {
                sfill(c1, 12);
                c1[0] = 'C'; c1[1] = 'Q';
                c1[3] = (char)('0' + nfreq / 100); c1[4] = (char)('0' + nfreq / 10 % 10); c1[5] = (char)('0' + nfreq % 10);
                cqnnn = 1;
            }
        }
        unpackcall(nc2, c2, &junk1, junk);
        unpackgrid(ng, grid);

        if (iv2 > 0) {
            int n1 = slen_trim(psfx, 4), n2 = slen_trim(c2, 12), pos = 0;
            if (iv2 >= 1 && iv2 <= 6) {
                int hv = (iv2 - 1) % 3;
                if (hv == 0) sapp(msg, 22, &pos, "CQ ", 3);
                else if (hv == 1) sapp(msg, 22, &pos, "QRZ ", 4);
                else sapp(msg, 22, &pos, "DE ", 3);
                if (iv2 <= 3) { sapp(msg, 22, &pos, psfx, n1); sapp(msg, 22, &pos, "/", 1); sapp(msg, 22, &pos, c2, n2); }
                else { sapp(msg, 22, &pos, c2, n2); sapp(msg, 22, &pos, "/", 1); sapp(msg, 22, &pos, psfx, n1); }
                sapp(msg, 22, &pos, " ", 1);
                sapp(msg, 22, &pos, grid, 4);
            } else if (iv2 == 7) {
                int kk = grid2k(grid);
                sapp(msg, 22, &pos, "DE ", 3);
                if (kk >= 451 && kk <= 900) {
                    getpfx2(kk, c2);
                    n2 = slen_trim(c2, 12);
                    sapp(msg, 22, &pos, c2, n2);
                } else {
                    sapp(msg, 22, &pos, c2, n2);
                    sapp(msg, 22, &pos, " ", 1);
                    sapp(msg, 22, &pos, grid, 4);
                }
            }
            goto post;
        }

        k = grid2k(grid);
        if (k >= 1 && k <= 450) getpfx2(k, c1);
        if (k >= 451 && k <= 900) getpfx2(k, c2);

        sfill(msg, 22);
        j = 0;                       /* Fortran 1-based index of last char written */
        if (cqnnn) {
            for (i = 0; i < 12 && i < 22; i++) msg[i] = c1[i];
            j = 7;
            goto l10;
        }
        for (i = 0; i < 12; i++) {
            j++; msg[j - 1] = c1[i];
            if (c1[i] == ' ') goto l10;
        }
        j++; msg[j - 1] = ' ';
l10:
        for (i = 0; i < 12; i++) {
            if (j <= 21) j++;
            msg[j - 1] = c2[i];
            if (c2[i] == ' ') goto l20;
        }
        if (j <= 21) j++;
        msg[j - 1] = ' ';
l20:
        if (k == 0) {
            for (i = 0; i < 4; i++) {
                if (j <= 21) j++;
                msg[j - 1] = grid[i];
            }
            if (j <= 21) j++;
            msg[j - 1] = ' ';
        }
    }
post:
    if (msg[0] == 'C' && msg[1] == 'Q' && msg[2] == '9' && msg[3] == 'D' && msg[4] == 'X' && msg[5] == ' ')
        msg[2] = ' ';
    if (msg[0] == 'E' && msg[1] == '9' && msg[2] >= 'A' && msg[2] <= 'Z' && msg[3] >= 'A' && msg[3] <= 'Z' && msg[4] == ' ') {
        char t[23]; int pos = 0;                 /* 'CQ '//msg(3:) */
        sfill(t, 22);
        sapp(t, 22, &pos, "CQ ", 3);
        sapp(t, 22, &pos, msg + 2, 20);
        for (i = 0; i < 23; i++) msg[i] = t[i];
    }
    if (msg[0] == 'C' && msg[1] == 'Q' && msg[2] == ' ' && msg[3] == '0' && msg[4] == '0' && msg[5] >= '0' && msg[5] <= '9') {
        char t[23]; int pos = 0;                 /* 'CQ '//msg(6:) */
        sfill(t, 22);
        sapp(t, 22, &pos, "CQ ", 3);
        sapp(t, 22, &pos, msg + 5, 17);
        for (i = 0; i < 23; i++) msg[i] = t[i];
    }
    msg[22] = 0;
    j = slen_trim(msg, 22);
    for (i = 0; i < 23; i++) out[i] = i < j ? msg[i] : 0;
    if (msg[0] == '0' && msg[1] == '0' && msg[2] == '0' && msg[3] == 'A' && msg[4] == 'A' && msg[5] == 'A')
        return -1;  /* WSJT-X extract4: '000AAA' => wrong mode, reject */
    return 0;
}
