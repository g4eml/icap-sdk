/* SPDX-License-Identifier: GPL-3.0-or-later
 * spot.h - thin PSKReporter *client* glue for the ICAP decoder apps (v4 re-architecture).
 *
 * v3 linked the whole PSKReporter IPFIX engine (pskr.c + pskr_app.h: a 64-entry queue, the 5-min flush, the
 * UDP send, the @MYCALL/@MYLOC/net gate and a 1400-byte datagram buffer) into EVERY decoder app.  On the
 * IC-9700 that ~17 KiB (text+bss, and a ctx+datagram carved from the decoder region) dropped the Q65 decoder
 * heap below its high-water and Q65 stopped decoding.
 *
 * v4 moves the engine, the queue, the datagram buffer, the flush timer, the UDP transport and the receiver
 * identity (@MYCALL/@MYLOC) + the true on-air dial (@FREQ) INTO THE LOADER.  An app now reports one decode by
 * opening the loader virtual file "@SPOT" for write and writing ONE line
 *
 *     "<sender_call> <sender_grid|-> <audio_tone_hz> <mode> <snr>\n"
 *
 * The loader parses the line, stamps UTC from its own clock, adds its own @FREQ true dial (IF + transverter)
 * to the audio tone, applies the call+grid+net gate and owns the single queue + single datagram.  The app
 * keeps only the small WSJT-X message parser (to pull the SENDER call/grid out of the decode text) and a few
 * bytes of line formatting.  No per-app pskr.c, no per-app ctx, no datagram buffer, nothing carved from the
 * decoder heap.  Freestanding: no libc.  Depends on icap.h being included first.
 */
#ifndef SPOT_H
#define SPOT_H
#include <stdint.h>

/* ---- tiny freestanding helpers (local; no libc, no shared state) --------- */
static int sp_slen(const char *s, int max) { int n = 0; if (s) while (n < max && s[n]) n++; return n; }

/* a plausible callsign: 3..11 of [A-Z0-9/], at least one letter and one digit */
static int sp_iscall(const char *s, int n)
{
    int dig = 0, let = 0;
    if (n < 3 || n > 11) return 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') dig++;
        else if (c >= 'A' && c <= 'Z') let++;
        else if (c == '/') ;
        else return 0;
    }
    return dig && let;
}

/* a 4- or 6-char Maidenhead locator (AA00 / AA00aa); rejects the RR73 ack that looks like a grid */
static int sp_isgrid(const char *s, int n)
{
    if (n != 4 && n != 6) return 0;
    if (n == 4 && s[0] == 'R' && s[1] == 'R' && s[2] == '7' && s[3] == '3') return 0;   /* RR73 is not a grid */
    if (s[0] < 'A' || s[0] > 'R' || s[1] < 'A' || s[1] > 'R') return 0;
    if (s[2] < '0' || s[2] > '9' || s[3] < '0' || s[3] > '9') return 0;
    if (n == 6) {
        char c = s[4] >= 'a' ? (char)(s[4] - 32) : s[4], d = s[5] >= 'a' ? (char)(s[5] - 32) : s[5];
        if (c < 'A' || c > 'X' || d < 'A' || d > 'X') return 0;
    }
    return 1;
}

/* Parse a WSJT-X decoded message into the SENDER callsign (the transmitting station) and its grid.
 *   "CQ [DIR] CALL [GRID]"   -> CALL, GRID
 *   "TOCALL DECALL [GRID]"   -> DECALL, GRID   (signal reports / RR73 leave grid empty)
 * Returns 1 and fills call/grid (grid[0]=0 if none) when a reportable sender is found, else 0. */
static int sp_parse(const char *msg, char *call, int ccap, char *grid, int gcap)
{
    const char *tk[6]; int tl[6], nt = 0;
    const char *p = msg;
    while (*p && nt < 6) {
        while (*p == ' ') p++;
        if (!*p) break;
        tk[nt] = p; int l = 0; while (p[l] && p[l] != ' ') l++;
        tl[nt] = l; p += l; nt++;
    }
    grid[0] = 0;
    if (nt < 2) return 0;
    int ci, gi;
    if (tl[0] == 2 && tk[0][0] == 'C' && tk[0][1] == 'Q') {
        if (sp_iscall(tk[1], tl[1])) { ci = 1; gi = 2; }       /* CQ CALL [GRID] */
        else if (nt >= 3) { ci = 2; gi = 3; }                  /* CQ DIR CALL [GRID] */
        else return 0;
    } else { ci = 1; gi = 2; }                                 /* TO DE [GRID] */
    if (ci >= nt || !sp_iscall(tk[ci], tl[ci])) return 0;
    int n = tl[ci]; if (n > ccap - 1) n = ccap - 1;
    for (int i = 0; i < n; i++) call[i] = tk[ci][i];
    call[n] = 0;
    if (gi < nt && sp_isgrid(tk[gi], tl[gi])) {
        int g = tl[gi]; if (g > gcap - 1) g = gcap - 1;
        for (int i = 0; i < g; i++) grid[i] = tk[gi][i];
        grid[g] = 0;
    }
    return 1;
}

static char *sp_cpy(char *d, const char *s) { while (*s) *d++ = *s++; return d; }
static char *sp_u(char *d, uint32_t v) { char t[12]; int k = 0; do { t[k++] = (char)('0' + v % 10u); v /= 10u; } while (v); while (k) *d++ = t[--k]; return d; }
static char *sp_i(char *d, int v) { if (v < 0) { *d++ = '-'; v = -v; } return sp_u(d, (uint32_t)v); }

/* Report one decode to the loader.  mode = "FT8"/"Q65"/...; msg = the WSJT-X decode text; tone_hz = the decode
 * audio tone (f0, < 4 kHz); snr = dB.  The loader supplies @MYCALL/@MYLOC/@FREQ/UTC and does the gate + queue +
 * flush, so this is a single short virtual-file write and nothing is reserved in the app.  Does nothing (returns
 * 0) when the message has no reportable sender or the runtime has no @SPOT service (old loader). */
static int spot_emit(const char *mode, const char *msg, double tone_hz, int snr)
{
    char call[16], grid[12], line[64], *q;
    int fd;
    if (!sp_parse(msg, call, (int)sizeof call, grid, (int)sizeof grid)) return 0;
    fd = icap_open("@SPOT", ICAP_O_WRITE);
    if (fd < 0) return 0;                                   /* no @SPOT service in this loader: silently skip */
    q = sp_cpy(line, call);
    *q++ = ' '; q = grid[0] ? sp_cpy(q, grid) : (*q = '-', q + 1);
    *q++ = ' '; q = sp_u(q, tone_hz > 0 ? (uint32_t)(tone_hz + 0.5) : 0u);
    *q++ = ' '; q = sp_cpy(q, mode);
    *q++ = ' '; q = sp_i(q, snr);
    *q++ = '\n';
    icap_write(fd, line, (uint32_t)(q - line));
    icap_close(fd);
    return 1;
}

/* WSPR variant: the decoder already gives call+grid directly ("CALL GRID DBM" word order is not a QSO, so the
 * generic parser would mis-read it).  grid may be ""/NULL. */
static int spot_emit_cg(const char *mode, const char *call, const char *grid, double tone_hz, int snr)
{
    char line[64], *q;
    int fd, cl = sp_slen(call, 15), gl = sp_slen(grid, 11);
    if (cl < 3) return 0;
    fd = icap_open("@SPOT", ICAP_O_WRITE);
    if (fd < 0) return 0;
    q = sp_cpy(line, call);
    *q++ = ' '; q = (gl && sp_isgrid(grid, gl)) ? sp_cpy(q, grid) : (*q = '-', q + 1);
    *q++ = ' '; q = sp_u(q, tone_hz > 0 ? (uint32_t)(tone_hz + 0.5) : 0u);
    *q++ = ' '; q = sp_cpy(q, mode);
    *q++ = ' '; q = sp_i(q, snr);
    *q++ = '\n';
    icap_write(fd, line, (uint32_t)(q - line));
    icap_close(fd);
    return 1;
}

#endif /* SPOT_H */
