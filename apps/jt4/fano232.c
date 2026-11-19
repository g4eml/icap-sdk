/* SPDX-License-Identifier: GPL-3.0-or-later
 * fano232.c - K=32 r=1/2 convolutional code (Layland-Lushbaugh polynomials):
 * encoder and Fano sequential decoder. Port of WSJT-X lib/fano232.f90 and
 * lib/encode232.f90 (after Phil Karn KA9Q), metric table from lib/getmet4.f90
 * (bias 0.5, scale 50, ndelta 170). Derived from WSJT-X, GPL-3.0.
 */
#include "fano232.h"

#define POLY1 0xf2d05351u
#define POLY2 0xe4613c47u

/* getmet4 mettab(x,0) for x=-128..127; mettab(x,1)=mettab(-x,0) (x=-128 -> -127) */
static const int16_t met0[256] = {
  25,25,25,25,25,25,25,25,25,25,25,25,25,25,25,25,
  25,25,25,25,25,25,25,25,25,25,25,25,25,25,25,25,
  25,25,25,25,25,25,25,25,25,25,25,25,25,25,25,25,
  24,25,25,25,25,25,25,25,25,25,25,25,25,25,25,25,
  25,25,25,25,25,24,25,24,24,24,24,24,24,24,24,24,
  24,24,24,24,23,23,23,23,23,23,22,22,22,22,21,21,
  21,21,20,20,19,19,18,18,17,17,16,15,15,14,13,12,
  11,10,8,7,6,4,2,1,-1,-4,-6,-9,-11,-14,-18,-21,
  -25,-29,-33,-37,-42,-46,-51,-55,-60,-65,-70,-74,-79,-84,-89,-94,
  -99,-104,-109,-115,-119,-125,-130,-135,-140,-146,-152,-155,-160,-166,-172,-178,
  -183,-188,-194,-199,-204,-209,-215,-220,-225,-230,-235,-241,-246,-251,-256,-262,
  -267,-272,-277,-282,-288,-293,-298,-303,-309,-314,-319,-324,-329,-335,-340,-345,
  -350,-356,-361,-366,-371,-376,-382,-387,-392,-397,-403,-408,-413,-418,-423,-429,
  -434,-439,-444,-450,-455,-460,-465,-470,-476,-481,-486,-491,-497,-502,-507,-512,
  -518,-523,-528,-533,-538,-544,-549,-554,-559,-565,-570,-575,-580,-585,-591,-596,
  -601,-606,-612,-617,-622,-627,-632,-638,-643,-648,-653,-659,-664,-669,-674,-679,
};

/* 8-bit parity, 1 bit per entry */
static const uint32_t partab32[8] = {
    0x96696996u, 0x69969669u, 0x69969669u, 0x96696996u,
    0x69969669u, 0x96696996u, 0x96696996u, 0x69969669u
};

static inline int parity32(uint32_t n)
{
    n ^= n >> 16; n ^= n >> 8; n &= 255;
    return (int)((partab32[n >> 5] >> (n & 31)) & 1u);
}

static inline int encsym(uint32_t st)
{
    return (parity32(st & POLY1) << 1) | parity32(st & POLY2);
}

void conv232_encode(const uint8_t *data, int nbits, uint8_t *sym)
{
    uint32_t st = 0;
    int i;
    for (i = 0; i < nbits; i++) {
        int s;
        st = (st << 1) | ((data[i >> 3] >> (7 - (i & 7))) & 1u);
        s = encsym(st);
        sym[2 * i] = (uint8_t)(s >> 1);
        sym[2 * i + 1] = (uint8_t)(s & 1);
    }
}

void fano232_init(fano232_work_t *w)
{
    int i;
    w->e0[0] = w->e1[0] = w->e2[0] = 0;          /* the encoder is linear: table[i] = table[i - lowest bit] ^ table[lowest bit] */
    for (i = 1; i < 2048; i++) {
        int low = i & -i;
        if (low == i) { w->e0[i] = (uint8_t)encsym((uint32_t)i); w->e1[i] = (uint8_t)encsym((uint32_t)i << 11); }
        else { w->e0[i] = w->e0[i - low] ^ w->e0[low]; w->e1[i] = w->e1[i - low] ^ w->e1[low]; }
        if (i < 1024) w->e2[i] = low == i ? (uint8_t)encsym((uint32_t)i << 22) : (uint8_t)(w->e2[i - low] ^ w->e2[low]);
    }
}

int fano232(const int8_t *symbol, int nbits, int maxcycles, uint8_t *dat,
            fano232_work_t *w, int (*should_abort)(void), uint32_t *ncycles_out)
{
    /* Same search as WSJT-X fano232.f90 (identical decisions and cycle count). What changed is the cost of
     * a cycle: the state lives in one record per node, the branch metrics are sorted per symbol pair before
     * the search (a forward move is one load), the encoder output comes from three byte tables, node [0] is
     * a sentinel instead of a "first node" test, and the abort poll is outside the inner loop. */
    const int ndelta = 170;
    fano232_node_t *const base = w->n + 1, *const end = base + nbits, *const tail = base + (nbits - 31);
    fano232_node_t *np;
    const uint8_t *const e0 = w->e0, *const e1 = w->e1, *const e2 = w->e2;
    uint32_t lim = (uint32_t)nbits * (uint32_t)maxcycles, it;
    int nt = 0, i, g, cur;
    for (i = 0; i < nbits; i++) {
        int a = symbol[2 * i], b = symbol[2 * i + 1], l;
        int a0 = met0[a + 128], b0 = met0[b + 128];
        int a1 = met0[(a > -128 ? -a : 127) + 128], b1 = met0[(b > -128 ? -b : 127) + 128];
        int met[4];
        met[0] = a0 + b0; met[1] = a0 + b1; met[2] = a1 + b0; met[3] = a1 + b1;
        for (l = 0; l < 4; l++) {
            int m0 = met[l], m1 = met[3 ^ l];              /* 0-branch sends pair l, 1-branch the complement */
            if (base + i >= tail || m0 > m1) { base[i].bf[l] = m0 * 2; base[i].sec[l] = m1; }
            else { base[i].bf[l] = m1 * 2 + 1; base[i].sec[l] = m0; }
        }
    }
    base[-1].gamma = INT32_MIN;
    np = base;
    np->sel = (uint32_t)encsym(0);
    np->st = (uint32_t)(np->bf[np->sel] & 1);
    np->gamma = 0;
    g = 0; cur = np->bf[np->sel] >> 1;         /* g = np->gamma, cur = metric of the branch in use */
    it = 1;
    while (it <= lim) {
        uint32_t last = it | 32767u, cnt;    /* run up to the next abort poll */
        if (last > lim) last = lim;
        for (cnt = last - it + 1; cnt; cnt--) {
            const int ngamma = g + cur;
            if (ngamma >= nt) {
                uint32_t st, lsym;
                int b;
                if (ngamma >= nt + ndelta && g < nt + ndelta) nt = nt + ndelta * ((ngamma - nt) / ndelta);
                st = np->st << 1;
                np++;
                np->gamma = ngamma;
                g = ngamma;
                if (np == end) { np->st = st; it = last - cnt + 1; goto done; }
                lsym = (uint32_t)(e0[st & 2047u] ^ e1[(st >> 11) & 2047u] ^ e2[st >> 22]);
                b = np->bf[lsym];
                cur = b >> 1;
                np->st = st | (uint32_t)(b & 1);
                np->sel = np >= tail ? lsym | 0x200u : lsym;
            } else {
                for (;;) {
                    const int gp = np[-1].gamma;
                    uint32_t sel;
                    if (gp < nt) {
                        nt -= ndelta;
                        sel = np->sel;
                        if (sel & 0x100u) { sel &= 3u; np->sel = sel; np->st ^= 1; }
                        cur = np->bf[sel & 3u] >> 1;       /* np may be a tail node reached by backing up */
                        break;
                    }
                    np--;
                    g = gp;
                    sel = np->sel;
                    if (sel < 0x100u) {
                        np->sel = sel | 0x100u;
                        np->st ^= 1;
                        cur = np->sec[sel];
                        break;
                    }
                }
            }
        }
        it = last + 1;
        if (it <= lim && should_abort && should_abort()) { if (ncycles_out) *ncycles_out = it; return -2; }
    }
    if (ncycles_out) *ncycles_out = it;
    return -1;
done:
    {
        int nbytes = (nbits + 7) / 8, j;
        for (j = 0, i = 7; j < nbytes - 1; j++, i += 8) dat[j] = (uint8_t)(base[i].st & 0xff);
        dat[nbytes - 1] = 0;
    }
    if (ncycles_out) *ncycles_out = it;
    return 0;
}
