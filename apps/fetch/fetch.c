/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Dawid SQ6EMM
 *
 * FETCH - download files over HTTPS/HTTP to the radio's SD card, then exit.
 * IC-705: WLAN, HTTPS and HTTP.  IC-9700: wired LAN, plain HTTP only (no TLS in that firmware: an https:// line
 * fails with a clear message).  The radio's folder is \IC-705 or \IC-9700 (ICAP_O_RADIO).
 *
 * Reads FETCH.CFG from the app folder, one entry per line ('#' = comment):
 *     https://host/path/file.app              -> Apps folder, name from the URL
 *     https://host/path/something  NAME.EXT   -> Apps folder, as NAME.EXT
 *     https://host/fw/705E_20261116v2.dat     -> *.dat (firmware) goes to the radio's folder (\IC-705), where
 *                                                MENU > Firmware Update looks (SD root on older loaders)
 *     https://host/path/file  @  / @NAME      -> the radio's folder (\IC-705)
 *     https://host/path/file  /               -> SD card root, name from the URL
 *     https://host/path/file  /NAME.EXT       -> SD card root, as NAME.EXT
 *     https://host/folder/                    -> EVERY file listed in that folder (URL ends with '/')
 *     https://host/folder/    /               -> ... all of them into the SD card root
 * A folder URL must return a directory index: an HTML page (nginx/Apache/python autoindex - the
 * href links are used) or plain text with one file name per line.  Sub-folders and parent links
 * are skipped.  If the folder has an MD5SUMS file (md5sum format), files whose MD5 on the card already
 * matches are skipped and every download is verified (a mismatch empties the file).  Without
 * MD5SUMS, files of 64 KB or more already on the card with the same size are skipped.
 * Folder lines are synchronised: a file FETCH downloaded from a folder earlier and that is no longer on
 * the server is deleted (bookkeeping in FETCH.LST; files FETCH never downloaded are never touched).
 *     -NAME / -/NAME / -@NAME                 -> delete that file from Apps / SD root / radio folder  Follows redirects (GitHub releases); HTTP/1.0 so the body is never chunked.  If the
 * radio cannot connect, a popup window explains why.  Built only against the public SDK header. */
#include "icap.h"

#ifndef TIMEOUT_MS
#define TIMEOUT_MS   20000u   /* a single read must make progress within this; a stall past it triggers resume */
#endif
#define MAX_RESUME   12       /* consecutive stalls (no new bytes) before a large download is given up */
#define MAX_REDIR    5
#define MAX_URL      256
#define MAX_FILES    64
#define E_SHORT      (-100)   /* app-local: body shorter than Content-Length */
#define E_HTTP       (-101)   /* app-local: server answered, but not 200 */
#define E_BIG        (-102)   /* app-local: folder index larger than the buffer */
#define E_MD5        (-103)   /* app-local: downloaded file does not match MD5SUMS */
#define SKIP_MIN     65536    /* without MD5SUMS: files this big are skipped when the card has the same size */

void *memcpy(void *d, const void *s, unsigned n) { char *p = d; const char *q = s; while (n--) *p++ = *q++; return d; }
void *memset(void *d, int c, unsigned n) { char *p = d; while (n--) *p++ = (char)c; return d; }

/* ------------------------------------------------------------ small string helpers */
static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static char *cat(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }
static char *num(char *d, int v)
{
    char t[12]; int n = 0; unsigned u = v < 0 ? (unsigned)-v : (unsigned)v;
    if (v < 0) *d++ = '-';
    do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (n) *d++ = t[--n];
    *d = 0; return d;
}
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int prefix_ci(const char *s, const char *p) { while (*p) if (lower(*s++) != *p++) return 0; return 1; }
static int prefix(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }
static int hexv(int c) { c = lower(c); return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

/* ------------------------------------------------------------ URL */
struct url { int tls; unsigned port; char host[128]; char path[MAX_URL]; };

static int url_parse(const char *u, struct url *o)
{
    if (prefix_ci(u, "https://")) { o->tls = 1; o->port = 443; u += 8; }
    else if (prefix_ci(u, "http://")) { o->tls = 0; o->port = 80; u += 7; }
    else return 0;
    unsigned k = 0;
    while (*u && *u != '/' && *u != ':' && k < sizeof o->host - 1) o->host[k++] = *u++;
    o->host[k] = 0;
    if (!k) return 0;
    if (*u == ':') { u++; o->port = 0; while (*u >= '0' && *u <= '9') o->port = o->port * 10 + (unsigned)(*u++ - '0'); }
    if (*u && *u != '/') return 0;
    k = 0;
    if (!*u) o->path[k++] = '/';
    while (*u && k < sizeof o->path - 1) o->path[k++] = *u++;
    o->path[k] = 0;
    return o->port > 0 && o->port < 65536;
}

/* "scheme://host[:port]" of u into d; returns the end */
static char *origin(char *d, const struct url *u)
{
    d = cat(d, u->tls ? "https://" : "http://"); d = cat(d, u->host);
    if (u->port != (u->tls ? 443u : 80u)) { *d++ = ':'; d = num(d, (int)u->port); }
    return d;
}

/* end of the path proper (before ?query / #fragment) */
static const char *path_end(const char *s) { while (*s && *s != '?' && *s != '#') s++; return s; }

/* local file name from the last segment of a URL path: %XX decoded, characters the SD API does not
 * take become '_'.  Names that fit 8.3 are upper-cased (as before); long names keep their case. */
static int local_name(const char *path, char *out)
{
    const char *e = path_end(path), *seg = path;
    for (const char *s = path; s < e; s++) if (*s == '/') seg = s + 1;
    unsigned n = 0;
    for (const char *p = seg; p < e; p++) {
        int c = (unsigned char)*p;
        if (c == '%' && p + 2 < e && hexv(p[1]) >= 0 && hexv(p[2]) >= 0) { c = hexv(p[1]) * 16 + hexv(p[2]); p += 2; }
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) c = '_';
        if (n >= ICAP_NAME_MAX) return 0;
        out[n++] = (char)c;
    }
    out[n] = 0;
    if (!n || out[0] == '.' || out[n - 1] == '.') return 0;
    for (unsigned i = 0; i + 1 < n; i++) if (out[i] == '.' && out[i + 1] == '.') return 0;
    unsigned dots = 0, b = 0, ok83 = 1;                     /* 8.3 -> upper case */
    for (unsigned i = 0; i < n; i++) {
        if (out[i] == '.') { if (++dots > 1 || !b) ok83 = 0; b = 0; continue; }
        if (++b > (dots ? 3u : 8u)) ok83 = 0;
    }
    if (ok83) for (unsigned i = 0; i < n; i++) if (out[i] >= 'a' && out[i] <= 'z') out[i] = (char)(out[i] - 32);
    return 1;
}

static int is_dat(const char *n) { unsigned l = slen(n); return l > 4 && prefix_ci(n + l - 4, ".dat"); }

/* ------------------------------------------------------------ UI */
static void say(const char *a, const char *b)
{
    char t[96], *p = t; p = cat(p, a); if (b) { unsigned k = 0; while (b[k] && k < 60) *p++ = b[k++]; *p = 0; }
    icap_print(t);
}

static int popup_shown;
static void fail_popup(const char *l1, const char *l2, const char *l3)
{
    char t[200], *p = t;
    p = cat(p, "FETCH: "); p = cat(p, l1);
    if (l2) { *p++ = '\n'; p = cat(p, l2); }
    if (l3) { *p++ = '\n'; p = cat(p, l3); }
    if (!popup_shown && icap_popup(t) == 0) popup_shown = 1;   /* one window per run is enough */
}

static const char *err_text(int e)
{
    switch (e) {
    case ICAP_E_NONET:   return "network not connected";
    case ICAP_E_NOSYS:   return "no HTTPS on this radio";
    case ICAP_E_DNS:     return "host name not found";
    case ICAP_E_CONN:    return "connection refused/TLS";
    case ICAP_E_TIMEOUT: return "timeout";
    case ICAP_E_NOFD:    return "too many connections";
    case ICAP_E_IO:      return "SD card write error";
    case ICAP_E_NAME:    return "bad file name / old firmware";
    case E_SHORT:        return "download incomplete";
    case E_HTTP:         return "server error";
    case E_BIG:          return "folder list too long";
    case E_MD5:          return "MD5 mismatch, file removed";
    default:             return "error";
    }
}

/* ------------------------------------------------------------ MD5 (RFC 1321) */
struct md5 { uint32_t h[4], n; unsigned char b[64]; };
static const uint32_t MD5_K[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,
    0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,
    0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,
    0x676f02d9,0x8d2a4c8a,0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,0xf4292244,0x432aff97,
    0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
    0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
static const unsigned char MD5_R[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };
static void md5_block(struct md5 *m, const unsigned char *p)
{
    uint32_t w[16], a = m->h[0], b = m->h[1], c = m->h[2], d = m->h[3];
    for (int i = 0; i < 16; i++) w[i] = p[4 * i] | p[4 * i + 1] << 8 | p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24;
    for (int i = 0; i < 64; i++) {
        uint32_t f; int g;
        if (i < 16)      { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) & 15; }
        else             { f = c ^ (b | ~d);       g = (7 * i) & 15; }
        uint32_t t = d, x = a + f + MD5_K[i] + w[g]; int r = MD5_R[(i >> 4) * 4 + (i & 3)];
        d = c; c = b; b = b + (x << r | x >> (32 - r)); a = t;
    }
    m->h[0] += a; m->h[1] += b; m->h[2] += c; m->h[3] += d;
}
static void md5_init(struct md5 *m) { m->h[0] = 0x67452301; m->h[1] = 0xefcdab89; m->h[2] = 0x98badcfe; m->h[3] = 0x10325476; m->n = 0; }
static void md5_put(struct md5 *m, const void *v, unsigned n)
{
    const unsigned char *p = v;
    while (n) {
        unsigned o = m->n & 63, c = 64 - o < n ? 64 - o : n;
        memcpy(m->b + o, p, c); m->n += c; p += c; n -= c;
        if ((m->n & 63) == 0) md5_block(m, m->b);
    }
}
static void md5_end(struct md5 *m, unsigned char out[16])
{
    uint32_t bits = m->n * 8, n = m->n;
    static const unsigned char pad[64] = { 0x80 };
    md5_put(m, pad, ((55 - n) & 63) + 1);
    unsigned char l[8] = { (unsigned char)bits, (unsigned char)(bits >> 8), (unsigned char)(bits >> 16), (unsigned char)(bits >> 24),
                           (unsigned char)(n >> 29), 0, 0, 0 };
    md5_put(m, l, 8);
    for (int i = 0; i < 16; i++) out[i] = (unsigned char)(m->h[i >> 2] >> (8 * (i & 3)));
}
static int same16(const unsigned char *a, const unsigned char *b) { for (int i = 0; i < 16; i++) if (a[i] != b[i]) return 0; return 1; }

/* ------------------------------------------------------------ HTTP */
static char hbuf[2048];          /* response head (+ first part of the body) */
static char io[1400];            /* body chunks */
static char req[512];

/* where a body goes: a file (name + root flag) or a memory buffer */
struct sink { const char *fname; int root; char *mem; unsigned max, len; const unsigned char *want; };
static struct md5 dl_md5;        /* hash of the body being written */

enum { R_OK = 0, R_REDIRECT = 1, R_SKIP = 2 };

/* size of a file already on the card (read through to the end, max `limit`+1), or -1 if absent */
static char chk[12288];          /* 12 KB: FETCH must fit the IC-9700 app window (image + bss <= 0x13000) */
/* MD5 of a file already on the card; 0 if absent */
static int local_md5(const char *fname, int root, unsigned char out[16])
{
    static struct md5 m;
    int fd = icap_open(fname, ICAP_O_READ | root);
    if (fd < 0) return 0;
    md5_init(&m);
    int r;
    while ((r = icap_read(fd, chk, sizeof chk)) > 0 && !icap_should_stop()) md5_put(&m, chk, (unsigned)r);
    icap_close(fd);
    md5_end(&m, out);
    return r >= 0;
}
static int local_size(const struct sink *k, int limit)
{
    int fd = icap_open(k->fname, ICAP_O_READ | k->root);
    if (fd < 0) return -1;
    int total = 0;
    while (total <= limit && !icap_should_stop()) {
        int r = icap_read(fd, chk, sizeof chk);
        if (r <= 0) break;
        total += r;
    }
    icap_close(fd);
    return total;
}

/* finds "name:" (lower-case) in the head and copies the value; 1 if found */
static int header(const char *head, const char *name, char *out, unsigned max)
{
    for (const char *s = head; *s; s++) {
        if ((s == head || s[-1] == '\n') && prefix_ci(s, name)) {
            s += slen(name);
            while (*s == ' ' || *s == '\t') s++;
            unsigned k = 0;
            while (*s && *s != '\r' && *s != '\n' && k < max - 1) out[k++] = *s++;
            out[k] = 0;
            return 1;
        }
    }
    return 0;
}

static int sink_open(struct sink *k)
{
    if (k->mem) { k->len = 0; return 0; }
    md5_init(&dl_md5);
    return icap_open(k->fname, ICAP_O_WRITE | k->root);
}
static int sink_put(struct sink *k, int fd, const char *b, unsigned n)
{
    if (!k->mem) { md5_put(&dl_md5, b, n); return icap_write(fd, b, n) == (int)n ? 0 : ICAP_E_IO; }
    if (k->len + n >= k->max) return E_BIG;
    memcpy(k->mem + k->len, b, n); k->len += n; k->mem[k->len] = 0;
    return 0;
}

/* open a socket to u and send one GET; if `from` >= 0 add "Range: bytes=<from>-".  On success returns the
 * socket handle (>= 0) and fills *status (HTTP code), *body_at (offset of the first body byte in hbuf) and
 * *have (bytes read into hbuf); hbuf holds the whole response head.  On a connect/send/recv failure it closes
 * the socket and returns the negative error. */
static int http_open_get(const struct url *u, int from, int *status, int *body_at, unsigned *have)
{
    int h = icap_net_open(u->host, u->port, u->tls ? ICAP_NET_TLS : 0, TIMEOUT_MS);
    if (h == ICAP_E_CONN && u->tls) {
        say("  retry without SNI", 0);
        h = icap_net_open(u->host, u->port, ICAP_NET_TLS | ICAP_NET_NOSNI, TIMEOUT_MS);
    }
    if (h < 0) return h;

    char *p = req;
    p = cat(p, "GET "); p = cat(p, u->path); p = cat(p, " HTTP/1.0\r\nHost: "); p = cat(p, u->host);
    p = cat(p, "\r\nUser-Agent: ICAP-FETCH/1\r\nAccept: */*\r\n");
    if (from >= 0) { p = cat(p, "Range: bytes="); p = num(p, from); p = cat(p, "-\r\n"); }
    p = cat(p, "Connection: close\r\n\r\n");
    int r = icap_net_send(h, req, (uint32_t)(p - req), TIMEOUT_MS);
    if (r < (int)(p - req)) { icap_net_close(h); return r < 0 ? r : ICAP_E_CONN; }

    unsigned hv = 0; int at = -1;
    while (at < 0) {
        if (hv >= sizeof hbuf - 1) { icap_net_close(h); return ICAP_E_CONN; }
        r = icap_net_recv(h, hbuf + hv, sizeof hbuf - 1 - hv, TIMEOUT_MS);
        if (r <= 0) { icap_net_close(h); return r < 0 ? r : ICAP_E_CONN; }
        hv += (unsigned)r; hbuf[hv] = 0;
        for (unsigned i = 3; i < hv; i++)
            if (hbuf[i - 3] == '\r' && hbuf[i - 2] == '\n' && hbuf[i - 1] == '\r' && hbuf[i] == '\n') { at = (int)i + 1; break; }
    }
    int st = 0;
    if (prefix_ci(hbuf, "http/")) { const char *s = hbuf; while (*s && *s != ' ') s++; while (*s == ' ') s++;
                                    while (*s >= '0' && *s <= '9') st = st * 10 + (*s++ - '0'); }
    *status = st; *body_at = at; *have = hv;
    return h;
}

/* parse "<digits>" at s into an int (decimal), stopping at the first non-digit */
static int dec(const char *s) { int v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v; }

/* a 206 response's "Content-Range: bytes <start>-<end>/<len>" must start exactly at `from` (no gap/overlap) */
static int cr_starts_at(const char *head, int from)
{
    char v[48];
    if (!header(head, "content-range:", v, sizeof v)) return 0;
    const char *s = v; while (*s == ' ') s++;
    if (prefix_ci(s, "bytes")) s += 5;
    while (*s == ' ') s++;
    if (*s < '0' || *s > '9') return 0;
    return dec(s) == from;
}

/* one GET; on a redirect, `next` receives the new absolute URL.  Returns R_OK, R_REDIRECT or <0.
 * A large body that stalls mid-transfer (recv timeout/error, or an early close with total < Content-Length)
 * is not aborted: the output fd and the running MD5 are kept open and the socket is reconnected with a
 * Range request to resume from where it stopped.  A 206 continues the same file; a 200 (no Range support)
 * restarts it cleanly.  STOP is honoured between retries. */
static int get_once(const struct url *u, struct sink *k, char *next, int *bytes, int *conn_err)
{
    int status, body_at; unsigned have;
    int h = http_open_get(u, -1, &status, &body_at, &have);
    if (h < 0) { *conn_err = 1; return h; }

    int r;
    char save = hbuf[body_at]; hbuf[body_at] = 0;
    char val[MAX_URL];
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
        int ok = header(hbuf, "location:", val, sizeof val);
        icap_net_close(h);
        if (!ok) return ICAP_E_CONN;
        if (val[0] == '/') {                               /* relative: same scheme/host */
            char *q = origin(next, u);
            if (slen(val) + (unsigned)(q - next) >= MAX_URL) return ICAP_E_ARG;
            cat(q, val);
        } else cat(next, val);
        return R_REDIRECT;
    }
    if (status != 200) {
        icap_net_close(h);
        char t[40], *q = t; q = cat(q, "  HTTP "); num(q, status); icap_print(t);
        return E_HTTP;
    }
    int clen = -1;
    if (header(hbuf, "content-length:", val, 16)) clen = dec(val);
    hbuf[body_at] = save;

    /* already on the card?  Big files only (firmware): same name + same size = same file.  Small
     * files (.APP, .CFG) are always refreshed - cheap, and a rebuilt app may keep its size. */
    if (!k->mem && !k->want && clen >= SKIP_MIN) {
        if (local_size(k, clen) == clen) { icap_net_close(h); *bytes = clen; return R_SKIP; }
    }

    int fd = sink_open(k);
    if (fd < 0) { icap_net_close(h); return fd; }
    int total = 0, err = 0;
    unsigned first = have - (unsigned)body_at;
    if (first) { err = sink_put(k, fd, hbuf + body_at, first); total += (int)first; }
    uint32_t shown = 0;
    int last_mark = total, stalls = 0;     /* total at the last give-up decision / consecutive stalls */

    for (;;) {                             /* body, across socket reconnects (resume) */
        while (h >= 0 && !err && (clen < 0 || total < clen)) {
            if (icap_should_stop()) { err = ICAP_E_TIMEOUT; break; }
            r = icap_net_recv(h, io, sizeof io, TIMEOUT_MS);
            if (r == 0) break;                              /* server closed */
            if (r < 0) break;                               /* timeout/socket error: try to resume */
            if ((err = sink_put(k, fd, io, (unsigned)r)) != 0) break;   /* SD write error: fatal */
            total += r;
            if (icap_ticks() - shown > 500) {
                shown = icap_ticks();
                char t[40], *q = t; q = cat(q, "  "); q = num(q, total);
                if (clen > 0) { q = cat(q, " / "); num(q, clen); }
                icap_set_line(7, t);
            }
        }
        if (h >= 0) { icap_net_close(h); h = -1; }
        if (err) break;                                     /* fatal (SD write / STOP) */
        if (clen < 0 || total >= clen) break;               /* no length: a clean close ends it; or complete */
        if (icap_should_stop()) { err = ICAP_E_TIMEOUT; break; }

        if (total > last_mark) stalls = 0; else stalls++;   /* progress resets the stall counter */
        last_mark = total;
        if (stalls >= MAX_RESUME) { err = E_SHORT; break; } /* too many stalls with no new bytes */

        { char t[40], *q = t; q = cat(q, "  resume "); q = num(q, total);
          if (clen > 0) { q = cat(q, " / "); num(q, clen); } icap_set_line(7, t); }

        int st2, at2; unsigned hv2;
        h = http_open_get(u, total, &st2, &at2, &hv2);      /* reconnect, Range: bytes=<total>- */
        if (h < 0) { h = -1; continue; }                    /* reconnect failed: counts as a stall, retry */
        if (st2 == 206) {
            char s2 = hbuf[at2]; hbuf[at2] = 0;
            int ok = cr_starts_at(hbuf, total);
            hbuf[at2] = s2;
            if (!ok) { icap_net_close(h); h = -1; continue; }   /* wrong offset: don't corrupt, retry */
        } else if (st2 == 200) {                            /* server ignores Range: start the file over */
            icap_close(fd);
            fd = sink_open(k);                              /* truncate + reset the running MD5 */
            if (fd < 0) { icap_net_close(h); h = -1; err = ICAP_E_IO; break; }
            total = 0;     /* restart from byte 0; keep last_mark so repeated no-Range truncation still gives up */
            char s2 = hbuf[at2]; hbuf[at2] = 0;
            if (header(hbuf, "content-length:", val, 16)) clen = dec(val);
            hbuf[at2] = s2;
        } else { icap_net_close(h); h = -1; continue; }     /* unexpected status: retry */
        unsigned f2 = hv2 - (unsigned)at2;                  /* this response's first body bytes */
        if (f2) { if ((err = sink_put(k, fd, hbuf + at2, f2)) != 0) break; total += (int)f2; }
    }

    if (h >= 0) icap_net_close(h);
    if (!k->mem) icap_close(fd);
    if (!err && clen >= 0 && total != clen) err = E_SHORT;        /* truncated */
    if (!err && !k->mem && k->want) {                       /* integrity against MD5SUMS */
        unsigned char got[16]; md5_end(&dl_md5, got);
        if (!same16(got, k->want)) err = E_MD5;
    }
    if (err) {
        if (!k->mem) {                                      /* never leave a partial file behind */
            fd = icap_open(k->fname, ICAP_O_WRITE | k->root);
            if (fd >= 0) icap_close(fd);
        }
        return err;
    }
    *bytes = total;
    return R_OK;
}

/* GET url (following redirects) into the sink; `final` receives the URL that answered 200 */
static int get(const char *url, struct sink *k, char *final)
{
    static char next[MAX_URL];
    struct url u;
    cat(final, url);
    if (!url_parse(final, &u)) { say("bad URL: ", url); return ICAP_E_ARG; }
    for (int hop = 0; hop <= MAX_REDIR; hop++) {
        int bytes = 0, conn_err = 0;
        say("  ", u.host);
        int r = get_once(&u, k, next, &bytes, &conn_err);
        if (r == R_SKIP) { icap_print("  same size, skipped"); return 0; }
        if (r == R_OK) {
            char t[40], *q = t; q = cat(q, k->mem ? "  list " : "  saved "); q = num(q, bytes); cat(q, " B"); icap_print(t);
            return 0;
        }
        if (r == R_REDIRECT) {
            cat(final, next);
            if (!url_parse(final, &u)) { say("bad redirect", 0); return ICAP_E_ARG; }
            continue;
        }
        if (r == ICAP_E_NOSYS && u.tls) {                  /* IC-9700: no TLS in the firmware */
            say("  no HTTPS on this radio, use http://", 0);
            fail_popup("no HTTPS on this radio.", "Use http:// in FETCH.CFG", "(no TLS in this firmware).");
            return r;
        }
        char t[64], *q = t; q = cat(q, "  "); q = cat(q, err_text(r));
        if (conn_err) { q = cat(q, " "); num(q, icap_net_error()); }
        icap_print(t);
        if (conn_err) {
            char e[40], *w = e; w = cat(w, err_text(r)); w = cat(w, " "); num(w, icap_net_error());
            fail_popup("cannot connect to", u.host, e);
        }
        return r;
    }
    say("  too many redirects", 0);
    return ICAP_E_CONN;
}

/* same name, ignoring case */
static int same_ci(const char *a, const char *b) { while (*a && lower(*a) == lower(*b)) a++, b++; return !*a && !*b; }

/* IC-9700: the ICAP runtime ICAPRT.BIN is downloaded to ICAPRT.TMP first and only a complete, verified file is
 * copied over it - a failed download never leaves the radio without its runtime (loaded at the next power-up). */
#define RT_FILE "ICAPRT.BIN"
#define RT_TMP  "ICAPRT.TMP"
static int replace_from(const char *from, const char *to)
{
    unsigned char a[16], b[16];
    int err = 0, r;
    if (!local_md5(from, 0, a)) err = ICAP_E_IO;
    int in = err ? -1 : icap_open(from, ICAP_O_READ);
    if (!err && in < 0) err = in;
    int out = err ? -1 : icap_open(to, ICAP_O_WRITE);
    if (!err && out < 0) { err = out; icap_close(in); in = -1; }
    if (!err) {
        while ((r = icap_read(in, chk, sizeof chk)) > 0)
            if (icap_write(out, chk, (uint32_t)r) != r) { err = ICAP_E_IO; break; }
        if (r < 0 && !err) err = r;
        icap_close(in); icap_close(out);
        if (!err && (!local_md5(to, 0, b) || !same16(a, b))) err = E_MD5;
    }
    icap_remove(from, 0);
    if (err) say("  runtime NOT replaced: ", err_text(err));
    return err;
}

/* does the loader know ICAP_O_RADIO?  Older ones reject the flag with ICAP_E_NAME. */
static int radio_ok(void)
{
    static int ok = -1;
    if (ok < 0) { int fd = icap_open("FETCH.LST", ICAP_O_READ | ICAP_O_RADIO); if (fd >= 0) icap_close(fd); ok = fd != ICAP_E_NAME; }
    return ok;
}
static int dir_flag(char c) { return c == '/' || c == '\\' ? ICAP_O_ROOT : c == '@' ? ICAP_O_RADIO : 0; }

/* one file.  `root` = folder flags (0 Apps, ICAP_O_ROOT, ICAP_O_RADIO).  dest: 0/"" = name from URL;
 * "/" or "@" = SD root / radio folder, name from URL; "/NAME", "@NAME"; "NAME" = Apps */
static int fetch_file(const char *url, const char *dest, int root, const unsigned char *want, int *root_out, char *name_out)
{
    static char fin[MAX_URL];
    char fname[ICAP_NAME_MAX + 1];
    if (dest && dir_flag(*dest)) { root = dir_flag(*dest); dest++; }
    if (dest && *dest) {
        unsigned k = 0;
        while (dest[k] && k < ICAP_NAME_MAX) { fname[k] = dest[k]; k++; }
        fname[k] = 0;
    } else {
        struct url u;
        if (!url_parse(url, &u)) { say("bad URL: ", url); return ICAP_E_ARG; }
        if (!local_name(u.path, fname)) { say("no file name, add one: ", url); return ICAP_E_NAME; }
    }
    if (is_dat(fname) && !root) root = radio_ok() ? ICAP_O_RADIO : ICAP_O_ROOT;   /* firmware: where MENU update looks */
    if (root == ICAP_O_RADIO && !radio_ok()) root = ICAP_O_ROOT;
    if (root_out) { *root_out = root; cat(name_out, fname); }
    if (want) {                                             /* same MD5 on the card: nothing to do */
        unsigned char have[16];
        if (local_md5(fname, root, have) && same16(have, want)) { say("= ", fname); return 0; }   /* "= NAME": unchanged */
    }
    say(root == ICAP_O_ROOT ? "->/" : root == ICAP_O_RADIO ? "->@" : "-> ", fname);   /* "/" SD root, "@" radio folder */
    int safe = !root && same_ci(fname, RT_FILE);
    struct sink k = { safe ? RT_TMP : fname, root, 0, 0, 0, want };
    int r = get(url, &k, fin);
    if (!safe) return r;
    if (r) { icap_remove(RT_TMP, 0); return r; }
    return replace_from(RT_TMP, fname);
}

/* ------------------------------------------------------------ folder */
static char listing[16384];

static void html_unescape(char *s)                          /* &amp; -> & (query strings in hrefs) */
{
    char *d = s;
    while (*s) { if (prefix(s, "&amp;")) { *d++ = '&'; s += 5; } else *d++ = *s++; }
    *d = 0;
}

static int n_ok, n_bad, n_del;                             /* files downloaded / failed / deleted */

/* ------------------------------------------------------------ sync (FETCH.LST)
 * FETCH.LST remembers which files each folder line has put on the card: "R|D|A NAME FOLDER-URL" (R = SD root, D = radio folder,
 * A = Apps folder).  After a COMPLETE pass over a folder, files that came from it before but are no longer
 * listed on the server are deleted.  Files FETCH never downloaded are never touched. */
#define LST_MAX 6144
static char lst_old[LST_MAX], lst_new[LST_MAX];
static unsigned lst_new_len;
static int lst_loaded, lst_over;
static const char *done_urls[16]; static int n_done;
static char kind_of(int f) { return f == ICAP_O_ROOT ? 'R' : f == ICAP_O_RADIO ? 'D' : 'A'; }
static int flags_of(char k) { return k == 'R' ? ICAP_O_ROOT : k == 'D' ? ICAP_O_RADIO : 0; }
static int lst_add(int root, const char *name, const char *folder)
{
    unsigned need = 2 + slen(name) + 1 + slen(folder) + 1;
    if (lst_new_len + need >= LST_MAX) { lst_over = 1; return 0; }
    char *p = lst_new + lst_new_len;
    *p++ = kind_of(root); *p++ = ' '; p = cat(p, name); *p++ = ' '; p = cat(p, folder); *p++ = '\n'; *p = 0;
    lst_new_len = (unsigned)(p - lst_new);
    return 1;
}
/* field k (0 kind, 1 name, 2 url) of a line [l, e) equals s */
static int lst_field_is(const char *l, const char *e, int k, const char *s)
{
    while (k--) { while (l < e && *l != ' ') l++; if (l < e) l++; }
    while (l < e && *s && *l == *s) l++, s++;
    return !*s && (l == e || *l == ' ' || *l == '\r');
}
static int in_list(const char *buf, unsigned len, const char *kind_name_line, unsigned kl)
{
    for (const char *l = buf; l < buf + len; ) {
        const char *e = l; while (e < buf + len && *e != '\n') e++;
        if ((unsigned)(e - l) == kl) { unsigned i = 0; while (i < kl && l[i] == kind_name_line[i]) i++; if (i == kl) return 1; }
        l = e + 1;
    }
    return 0;
}
/* delete what came from `folder` before and is not in this pass's entries [from, lst_new_len) */
static void lst_sync(const char *folder, unsigned from)
{
    if (!lst_loaded || lst_over) return;
    for (const char *l = lst_old; *l; ) {
        const char *e = l; while (*e && *e != '\n') e++;
        const char *nx = *e ? e + 1 : e;
        if ((l[0] == 'R' || l[0] == 'A' || l[0] == 'D') && l[1] == ' ' && lst_field_is(l, e, 2, folder)) {
            unsigned kl = (unsigned)(e - l); while (kl && (l[kl - 1] == '\r')) kl--;
            if (!in_list(lst_new + from, lst_new_len - from, l, kl)) {
                char name[ICAP_NAME_MAX + 1]; unsigned k = 0; const char *q = l + 2;
                while (q < e && *q != ' ' && k < ICAP_NAME_MAX) name[k++] = *q++;
                name[k] = 0;
                if (k && !(l[0] == 'A' && (lst_field_is(l, e, 1, "FETCH.CFG") || lst_field_is(l, e, 1, "FETCH.APP") ||
                                           lst_field_is(l, e, 1, RT_FILE)))) {
                    int r = icap_remove(name, flags_of(l[0]));
                    say(r == 0 ? "x " : "x? ", name);         /* "x NAME": deleted (no longer on the server) */
                    if (r == 0) n_del++;
                }
            }
        }
        l = nx;
    }
}
static void lst_load(void)
{
    if (!ICAP_HAS(remove)) return;                                      /* firmware cannot delete: no sync at all */
    int fd = icap_open("FETCH.LST", ICAP_O_READ);
    if (fd < 0) { lst_loaded = 1; lst_old[0] = 0; return; }           /* first run: nothing to delete yet */
    int n = icap_read(fd, lst_old, LST_MAX - 1);
    icap_close(fd);
    if (n < 0 || n >= LST_MAX - 1) return;                              /* unreadable/too big: never delete */
    lst_old[n] = 0; lst_loaded = 1;
}
static void lst_save(void)
{
    if (!n_done || !lst_loaded || lst_over) return;
    for (const char *l = lst_old; *l; ) {                               /* keep entries of folders not synced now */
        const char *e = l; while (*e && *e != '\n') e++;
        int keep = 1;
        for (int i = 0; i < n_done && keep; i++) if (lst_field_is(l, e, 2, done_urls[i])) keep = 0;
        if (keep && e > l) {
            unsigned kl = (unsigned)(e - l);
            if (lst_new_len + kl + 1 >= LST_MAX) return;
            memcpy(lst_new + lst_new_len, l, kl); lst_new_len += kl; lst_new[lst_new_len++] = '\n'; lst_new[lst_new_len] = 0;
        }
        l = *e ? e + 1 : e;
    }
    int fd = icap_open("FETCH.LST", ICAP_O_WRITE);
    if (fd < 0) return;
    icap_write(fd, lst_new, lst_new_len);
    icap_close(fd);
}

/* MD5SUMS (md5sum format "<32 hex> [ *]name"): expected hash for a file name, 0 if not listed */
static char sums[4096];
static int have_sums;
static const unsigned char *sum_for(const char *name)
{
    static unsigned char h[16];
    if (!have_sums) return 0;
    for (const char *s = sums; *s; ) {
        const char *l = s; while (*s && *s != '\n') s++;
        const char *e = s; if (*s) s++;
        int ok = 1;
        for (int i = 0; i < 32 && ok; i++) ok = l + i < e && hexv(l[i]) >= 0;
        if (!ok) continue;
        const char *p = l + 32; while (p < e && (*p == ' ' || *p == '*')) p++;
        const char *q = name; while (p < e && *q && *p == *q) p++, q++;
        while (p < e && (*p == '\r' || *p == ' ')) p++;
        if (*q || p != e) continue;
        for (int i = 0; i < 16; i++) h[i] = (unsigned char)(hexv(l[2 * i]) << 4 | hexv(l[2 * i + 1]));
        return h;
    }
    return 0;
}
static void pct_decode(const char *s, char *d, unsigned max)
{
    const char *e = path_end(s); unsigned n = 0;
    while (s < e && n < max - 1) {
        if (*s == '%' && s + 2 < e + 1 && hexv(s[1]) >= 0 && hexv(s[2]) >= 0) { d[n++] = (char)(hexv(s[1]) * 16 + hexv(s[2])); s += 3; }
        else d[n++] = *s++;
    }
    d[n] = 0;
}

static int fetch_folder(const char *url, const char *dest)
{
    static char base[MAX_URL], file[MAX_URL];
    say("folder ", url);
    struct sink k = { 0, 0, listing, sizeof listing, 0, 0 };
    int r = get(url, &k, base);
    if (r) { n_bad++; return r; }
    struct url bu;
    if (!url_parse(base, &bu)) return ICAP_E_ARG;
    unsigned bl = slen(base);
    if (bl && base[bl - 1] != '/') { while (bl && base[bl - 1] != '/') bl--; base[bl] = 0; }   /* redirected to a file? */
    const char *bpath = bu.path; unsigned bpl = 0;
    { const char *e = path_end(bpath); while (e > bpath && e[-1] != '/') e--; bpl = (unsigned)(e - bpath); }

    /* collect candidate entries, NUL-terminated in place */
    char *items[MAX_FILES]; int n = 0, html = 0;
    for (char *s = listing; *s; s++) if (prefix_ci(s, "href=")) { html = 1; break; }
    for (char *s = listing; *s && n < MAX_FILES; ) {
        char *it = 0;
        if (html) {
            if (!prefix_ci(s, "href=")) { s++; continue; }
            s += 5;
            char q = (*s == '"' || *s == '\'') ? *s++ : 0;
            it = s;
            while (*s && (q ? *s != q : (*s != ' ' && *s != '>'))) s++;
            if (*s) *s++ = 0;
        } else {
            while (*s == '\r' || *s == '\n' || *s == ' ' || *s == '\t') s++;
            it = s;
            while (*s && *s != '\r' && *s != '\n') s++;
            if (*s) *s++ = 0;
            char *e = it + slen(it); while (e > it && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
            if (*it == '#') continue;
        }
        html_unescape(it);
        /* make it relative to the folder */
        if (prefix_ci(it, "http://") || prefix_ci(it, "https://")) {
            if (!prefix(it, base)) continue;                /* another site / folder */
            it += bl;
        } else if (*it == '/') {
            if (bpl < 1 || !prefix(it, bpath) || slen(it) < bpl) continue;
            it += bpl;
        }
        if (!*it || *it == '?' || *it == '#' || *it == '.') continue;   /* sort links, ../, hidden */
        const char *e = path_end(it), *c = it;
        while (c < e && *c != '/') c++;
        if (c < e) continue;                                /* sub-folder */
        if (slen(base) + slen(it) >= MAX_URL) continue;
        int dup = 0;
        for (int i = 0; i < n && !dup; i++) { const char *a = items[i], *b = it; while (*a && *a == *b) a++, b++; dup = !*a && !*b; }
        if (!dup) items[n++] = it;
    }
    {
        char t[40], *q = t; q = cat(q, "  "); q = num(q, n); cat(q, n == 1 ? " file" : " files"); icap_print(t);
    }
    int root = dest ? dir_flag(*dest) : 0;
    have_sums = 0;
    for (int i = 0; i < n; i++) {                           /* checksums first (not saved to the card) */
        if (!prefix(items[i], "MD5SUMS") || items[i][7]) continue;
        static char fin[MAX_URL];
        cat(cat(file, base), "MD5SUMS");
        struct sink sk = { 0, 0, sums, sizeof sums, 0, 0 };
        have_sums = get(file, &sk, fin) == 0;
        items[i] = items[--n];
        break;
    }
    unsigned from = lst_new_len;
    int i = 0;
    for (; i < n && !icap_should_stop(); i++) {
        char *q = cat(file, base);
        const char *s = items[i];
        while (*s && *s != '#') *q++ = *s++;
        *q = 0;
        char nm[MAX_URL]; pct_decode(items[i], nm, sizeof nm);
        int r_root = 0; char r_name[ICAP_NAME_MAX + 1]; r_name[0] = 0;
        if (fetch_file(file, root == ICAP_O_ROOT ? "/" : root == ICAP_O_RADIO ? "@" : 0, root, sum_for(nm), &r_root, r_name)) n_bad++; else n_ok++;
        if (r_name[0]) lst_add(r_root, r_name, url);       /* on the server = keep, even if this download failed */
        if (popup_shown) break;
    }
    if (!n) { say("  no files listed in ", url); n_bad++; }
    if (i == n && n && !popup_shown && !icap_should_stop() && n_done < 16) {   /* complete pass: sync */
        lst_sync(url, from);
        done_urls[n_done++] = url;
    } else lst_new_len = from, lst_new[from] = 0;           /* incomplete: keep the old entries untouched */
    return 0;
}

/* ------------------------------------------------------------ main */
static char cfg[2048];

int app_main(const struct icap_svc *svc)
{
    icap_use(svc);
    icap_clear();
    icap_print("FETCH - SD downloader");

    if (!ICAP_HAS(net_open)) {
        icap_print("needs firmware with ICAP network");
        fail_popup("this firmware has no", "network support for apps.", 0);
        icap_sleep_ms(3000);
        return 1;
    }
    int fd = icap_open("FETCH.CFG", ICAP_O_READ);
    if (fd < 0) {
        icap_print("FETCH.CFG not found");
        fail_popup("FETCH.CFG not found in", "the Apps folder (one URL", "per line).");
        icap_sleep_ms(3000);
        return 2;
    }
    int n = icap_read(fd, cfg, sizeof cfg - 1);
    icap_close(fd);
    if (n <= 0) { icap_print("FETCH.CFG is empty"); return 2; }
    cfg[n] = 0;

    int lan = icap_is_ic9700();                            /* IC-9700: wired LAN; IC-705: WLAN */
    int st = icap_net_status();
    for (int i = 0; st == ICAP_E_NONET && i < 20 && !icap_should_stop(); i++) {   /* WLAN joining / DHCP lease */
        if (i == 0) icap_print(lan ? "waiting for LAN..." : "waiting for WLAN...");
        icap_sleep_ms(500);
        st = icap_net_status();
    }
    if (st != 0) {
        icap_print(lan ? "LAN not connected" : "WLAN not connected");
        if (lan) fail_popup("no LAN connection.", "Check the LAN cable and the", "radio's IP (DHCP / static).");
        else fail_popup("no WLAN connection.", "Turn WLAN on and connect", "to an access point.");
        icap_sleep_ms(3000);
        return 3;
    }

    lst_load();
    for (char *line = cfg; *line && !icap_should_stop(); ) {
        char *end = line; while (*end && *end != '\n') end++;
        char *next = *end ? end + 1 : end;
        *end = 0;
        while (*line == ' ' || *line == '\t') line++;
        char *e = line + slen(line); while (e > line && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        if (*line == '-' && line[1]) {                      /* "-NAME" / "-/NAME": delete (cleanup, owner's config) */
            int root = dir_flag(line[1]);
            const char *nm = line + 1 + (root != 0);
            int r = icap_remove(nm, root);
            if (r == 0) { say(root == ICAP_O_ROOT ? "x /" : root == ICAP_O_RADIO ? "x @" : "x ", nm); n_del++; }   /* already gone: silent */
            else if (r == ICAP_E_NAME || r == ICAP_E_NOSYS) say("x? firmware too old: ", nm);
        } else if (*line && *line != '#') {
            char *name = line; while (*name && *name != ' ' && *name != '\t') name++;
            if (*name) { *name++ = 0; while (*name == ' ' || *name == '\t') name++; }
            static struct url lu;
            const char *pe = url_parse(line, &lu) ? path_end(lu.path) : lu.path;
            if (pe > lu.path && pe[-1] == '/') fetch_folder(line, name);              /* URL ends with '/' */
            else if (fetch_file(line, name, 0, 0, 0, 0) == 0) n_ok++; else n_bad++;
            if (popup_shown) break;                         /* cannot connect: stop, the window says why */
        }
        line = next;
    }
    lst_save();
    char t[40], *q = t; q = cat(q, "done: "); q = num(q, n_ok); q = cat(q, " ok, "); q = num(q, n_bad); q = cat(q, " bad");
    if (n_del) { q = cat(q, ", "); q = num(q, n_del); cat(q, " del"); }
    icap_print(t);
    icap_sleep_ms(2000);
    return n_bad ? 4 : 0;
}
