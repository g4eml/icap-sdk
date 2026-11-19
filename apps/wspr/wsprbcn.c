/* SPDX-License-Identifier: GPL-3.0-or-later
 * wsprbcn.c - WSPR-2 beacon app for the ICAP loader (IC-705 / IC-9700).  Registered as mode WSPR so it appears
 * in the RTTY-button cycle; the WSPR *decoder* is a separate later step, so this app only transmits.
 *
 * beacon=1 in WSPR.CFG: every even UTC minute (+1 s, aligned by the loader) it builds "<CALL> <GRID4> <DBM>" from
 * @MYCALL + @MYLOC and the CFG, encodes it (wsprenc, bit-exact vs WSJT-X) and streams it via icap_tx_*.  Keys only
 * when @MYCALL + @MYLOC are set.  CFG: beacon=, call=/loc= overrides, dbm= (default 23), txfreq= (audio centre Hz,
 * default 1500), plus the loader's txlevel=/txmax=/txlimit=.  Built only against the public SDK + wsprenc.
 */
#include "icap.h"
#include "beacon.h"
#include "wsprenc.h"

#ifndef DIGI_HOST
void *memcpy(void *d, const void *s, unsigned n) { char *p = d; const char *q = s; while (n--) *p++ = *q++; return d; }
void *memset(void *d, int c, unsigned n) { char *p = d; while (n--) *p++ = (char)c; return d; }
#endif

static int   cfg_beacon = 0, cfg_dbm = 23, cfg_txfreq = 0;
static char  cfg_call[16], cfg_loc[12];

static int kmatch(const char *s, const char *k) { int i = 0; while (k[i] && s[i] == k[i]) i++; return k[i] == 0 && s[i] == '='; }
static int kint(const char *s) { int v = 0, nd = 0, neg = 0; while (*s && *s != '=') s++; if (*s) s++; if (*s == '-') { neg = 1; s++; }
    for (; *s >= '0' && *s <= '9' && nd < 4; s++, nd++) v = v * 10 + (*s - '0'); return nd ? (neg ? -v : v) : -1000; }
static void ktok(const char *s, char *out, int cap) { int k = 0; while (*s && *s != '=') s++; if (*s) s++; while (*s == ' ') s++;
    for (; *s > ' ' && k < cap - 1; s++) out[k++] = (*s >= 'a' && *s <= 'z') ? (char)(*s - 32) : *s; out[k] = 0; }
static void read_cfg(void)
{
    char b[160]; int fd = icap_open("WSPR.CFG", ICAP_O_READ);
    if (fd < 0) return;
    int n = icap_read(fd, b, (int)sizeof b - 1); icap_close(fd);
    if (n <= 0) return;
    b[n] = 0;
    for (char *s = b; *s; s++) {
        if (s != b && s[-1] != '\n' && s[-1] != '\r' && s[-1] != ' ' && s[-1] != '\t') continue;
        if (kmatch(s, "beacon")) { int v = kint(s); if (v != -1000) cfg_beacon = v != 0; }
        else if (kmatch(s, "call")) ktok(s, cfg_call, sizeof cfg_call);
        else if (kmatch(s, "loc"))  ktok(s, cfg_loc, sizeof cfg_loc);
        else if (kmatch(s, "dbm"))  { int v = kint(s); if (v >= 0 && v <= 60) cfg_dbm = v; }
        else if (kmatch(s, "txfreq")) { int v = kint(s); if (v >= 100 && v <= 3800) cfg_txfreq = v; }
    }
}

/* v6: beacon activation is a RUNTIME state: the owner arms / disarms it on the radio (MODE-tile long press, loader
 * @TX beacon=/owner=, followed via bcn_want); beacon=1 in WSPR.CFG stays the dev override (the loader arms at start). */
static wspr_tx_t g_tx;
static int bcn_fill(void *c, int16_t *b, int n) { (void)c; return wspr_tx_fill(&g_tx, b, n); }
static int bcn_prepare(const char *call, const char *loc, bcn_fill_t *f, void **c, const char **err)
{
    char g4[5]; int i; for (i = 0; i < 4 && loc[i]; i++) g4[i] = loc[i]; g4[i] = 0;    /* WSPR type-1 uses GRID4 */
    int e = wspr_tx_init(&g_tx, call, g4, cfg_dbm, cfg_txfreq ? (double)cfg_txfreq : WSPR_F0_DEFAULT_HZ, 0, 0);
    if (e) { *err = e == TXE_CALL ? "compound call: set call=" : e == TXE_GRID ? "bad grid" : "encode error"; return -1; }
    *f = bcn_fill; *c = 0; return 0;
}

int app_main(const struct icap_svc *svc)
{
    icap_use(svc);
    read_cfg();
    static int16_t txbuf[160];
    for (;;) {
        if (bcn_want(cfg_beacon)) {                  /* armed: transmit until the owner disarms */
            bcn_follow = cfg_beacon;
            bcn_run("WSPR", bcn_prepare, cfg_call, cfg_loc, txbuf, (int)(sizeof txbuf / sizeof txbuf[0]));
            if (icap_should_stop()) return 0;
        }
        icap_clear();                                /* decoder is a later step: beacon-only for now */
        icap_set_line(0, "WSPR beacon");
        icap_set_line(2, "long-press MODE to transmit");
        icap_set_line(3, "(WSPR decode: a later firmware step)");
        while (!icap_should_stop() && !bcn_want_poll(cfg_beacon, 1000)) icap_sleep_ms(200);
        if (icap_should_stop()) return 0;
    }
}
