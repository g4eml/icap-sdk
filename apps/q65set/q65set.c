/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Q65SET.APP - switch the Q65 decoder's T/R period between 60 s and 30 s without taking the SD card out.
 *
 * Every start toggles the "period=" setting in Q65.CFG (the Q65 app's config, same folder): 30 -> 60, anything
 * else (60, 15, no setting, no file) -> 30.  The rest of the file is kept as it is; "period=" becomes its first
 * line, because Q65.APP reads only the start of the file.  A file in the old positional form ("fmin fmax
 * [capture_s]") is rewritten as flo= / fhi= / cap= lines first - Q65.APP reads no keys from that form.
 * The previous file is saved as Q65CFG.BAK; the new one is read back and compared before success is reported.
 * Q65.APP reads Q65.CFG when it starts: select Q65 (RTTY button) for the new period to apply.  The loader runs one
 * app at a time - with Q65 (or any app) running, starting Q65SET shows the running app instead, so that app has to
 * be stopped first (STOP in MENU > SET > Apps).
 * Only the public ICAP API (icap.h) is used.
 */
#include "icap.h"

#define F_CFG "Q65.CFG"
#define F_BAK "Q65CFG.BAK"
#define CAP   2048u                     /* largest Q65.CFG handled (Q65.APP itself reads its first 199 bytes) */

static char g_in[CAP + 1], g_out[CAP + 64], g_chk[CAP + 64];

static unsigned z_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static char *z_cpy(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }
static char *z_u(char *d, unsigned v) { char t[10]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) *d++ = t[--n]; *d = 0; return d; }
static int is_blank(char c) { return c == ' ' || c == '\t'; }
static int is_eol(char c) { return c == '\r' || c == '\n'; }

/* whole file -> b (NUL-terminated); length, -1 = no file, -2 = too large or read error */
static int read_file(const char *name, char *b, unsigned cap)
{
    int fd = icap_open(name, ICAP_O_READ);
    if (fd < 0) return -1;
    unsigned n = 0;
    for (;;) {
        if (n == cap) { char x; int r = icap_read(fd, &x, 1); icap_close(fd); if (r > 0) return -2; b[n] = 0; return (int)n; }
        int r = icap_read(fd, b + n, cap - n);
        if (r < 0) { icap_close(fd); return -2; }
        if (r == 0) break;
        n += (unsigned)r;
    }
    icap_close(fd);
    b[n] = 0;
    return (int)n;
}
static int write_file(const char *name, const char *b, unsigned n)
{
    int fd = icap_open(name, ICAP_O_WRITE);
    if (fd < 0) return -1;
    unsigned off = 0;
    while (off < n) { int w = icap_write(fd, b + off, n - off); if (w <= 0) break; off += (unsigned)w; }
    icap_close(fd);
    return off == n ? 0 : -1;
}

/* the key at the start of line l (after blanks): 1 if it is "<k>=" */
static int key_at(const char *l, const char *k)
{
    while (is_blank(*l)) l++;
    while (*k && *l == *k) { l++; k++; }
    return !*k && *l == '=';
}
static unsigned num_after_eq(const char *l)
{
    while (*l && *l != '=' && !is_eol(*l)) l++;
    if (*l == '=') l++;
    while (is_blank(*l)) l++;
    unsigned v = 0, nd = 0;
    for (; *l >= '0' && *l <= '9' && nd < 6; l++, nd++) v = v * 10u + (unsigned)(*l - '0');
    return nd ? v : 0;
}

static void show(int line, const char *a, const char *b)
{
    static char t[80];
    char *p = z_cpy(t, a);
    if (b) z_cpy(p, b);
    icap_set_line(line, t);
}

int app_main(const struct icap_svc *svc)
{
    icap_use(svc);
    icap_clear();
    /* the app window shows at most 32 characters per line */
    icap_set_line(0, "Q65SET: period 60 s <-> 30 s");

    int n = read_file(F_CFG, g_in, CAP);
    if (n == -2) {
        show(2, "Q65.CFG too big or unreadable:", 0);
        show(3, "nothing was changed", 0);
        goto out;
    }
    if (n < 0) { n = 0; g_in[0] = 0; }                 /* no Q65.CFG yet */

    /* the old positional form "fmin fmax [capture_s]" (the file starts with a digit) -> key=value lines */
    const char *src = g_in;
    static char conv[96];
    unsigned i = 0;
    while (i < (unsigned)n && (is_blank(g_in[i]) || is_eol(g_in[i]))) i++;
    if (i < (unsigned)n && g_in[i] >= '0' && g_in[i] <= '9') {
        unsigned v[3] = { 0, 0, 0 }, vi = 0;
        while (i < (unsigned)n && vi < 3) {
            while (i < (unsigned)n && (g_in[i] < '0' || g_in[i] > '9')) i++;
            if (i >= (unsigned)n) break;
            unsigned a = 0; while (i < (unsigned)n && g_in[i] >= '0' && g_in[i] <= '9') { if (a < 100000u) a = a * 10u + (unsigned)(g_in[i] - '0'); i++; }
            v[vi++] = a;
        }
        char *q = conv;
        if (vi >= 2) { q = z_cpy(q, "flo="); q = z_u(q, v[0]); q = z_cpy(q, "\r\nfhi="); q = z_u(q, v[1]); q = z_cpy(q, "\r\n"); }
        if (vi >= 3) { q = z_cpy(q, "cap="); q = z_u(q, v[2]); q = z_cpy(q, "\r\n"); }
        src = conv;
    }

    /* the current period (the last period= line, as Q65.APP reads it), and the file without its period= lines */
    unsigned cur = 60, outn;
    char *o = g_out + 16;                               /* room in front for the new first line */
    const char *s = src;
    while (*s) {
        const char *e = s; while (*e && !is_eol(*e)) e++;
        while (is_eol(*e)) e++;                         /* the line with its line end */
        if (key_at(s, "period")) { unsigned v = num_after_eq(s); if (v == 15 || v == 30 || v == 60) cur = v; }
        else while (s < e) *o++ = *s++;
        s = e;
    }
    *o = 0;
    unsigned next = cur == 30 ? 60 : 30;
    {   char first[16], *q = z_cpy(first, "period="); q = z_u(q, next); z_cpy(q, "\r\n");
        unsigned fl = z_len(first);
        char *start = g_out + 16 - fl;
        for (unsigned k = 0; k < fl; k++) start[k] = first[k];
        outn = (unsigned)(o - start);
        /* keep the previous file, then write and verify the new one */
        if (n > 0 && write_file(F_BAK, g_in, (unsigned)n)) {
            show(2, "Could not save Q65CFG.BAK:", 0); show(3, "Q65.CFG was not changed", 0); goto out;
        }
        if (write_file(F_CFG, start, outn) || read_file(F_CFG, g_chk, CAP + 63) != (int)outn) goto bad;
        for (unsigned k = 0; k < outn; k++) if (g_chk[k] != start[k]) goto bad;
    }
    {   char t[48], *q = z_cpy(t, "Q65 period: "); q = z_u(q, next); q = z_cpy(q, " s (was "); q = z_u(q, cur); z_cpy(q, " s)");
        icap_set_line(2, t); }
    icap_set_line(4, "Select Q65 with the RTTY button");
    /* the loader runs one app at a time: starting Q65SET while another app runs shows that app instead */
    icap_set_line(5, "To switch again: STOP the");
    icap_set_line(6, "running app in the SET menu,");
    icap_set_line(7, "then start Q65SET.");
    goto out;
bad:
    show(2, "WRITE FAILED: Q65.CFG may", 0);
    show(3, "be damaged. The previous file", 0);
    show(4, "is in Q65CFG.BAK.", 0);
out:
    for (int t = 0; t < 100 && !icap_should_stop(); t++) icap_sleep_ms(100);   /* 10 s on screen, EXIT closes */
    return 0;
}
