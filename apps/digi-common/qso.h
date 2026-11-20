/* SPDX-License-Identifier: GPL-3.0-or-later
 * qso.h - QSO mode shared by the ICAP decoder apps (digiapp.c: FT8, JT4; q65_705.c: Q65).  Phase 4b.
 *
 * The OPERATOR drives every step; nothing here advances a QSO by itself.
 *   - Tap a decoded line on the DECODE screen.  The loader hands the row's text to the app ("@QSO"); the app derives
 *     the NEXT message of the standard sequence from that line and prints it as a prompt:   TX? SP6ABC SQ6EMM JO81
 *   - Tap the "TX?" line.  The app asks the loader for ONE transmission in the period opposite to the tapped
 *     station's ("@QTX0" / "@QTX1"); the loader grants it only after a tap, arms its own TX state machine (the beacon
 *     gate: clock lock, band / mode / PTT / TOT checks, cap, owner abort) and shows the green frame.  When the period
 *     comes the app sends the message once and prints   HHMMSS TX SP6ABC SQ6EMM JO81.
 *     Tapping the same "TX?" line again later repeats that message once more (no answer heard).
 *   - Tapping any other row (not a decode, not the current prompt) clears the selection and cancels an armed answer.
 *   - CQ: armed on the radio (hold the mode tile twice as long as for the beacon, green frame).  While it is armed and
 *     no station is selected the app sends "CQ <call> <grid>" in every second period, always the same one.
 * Sequence (my = own call from @MYCALL, grid = the first four characters of @MYLOC; <MODE>.CFG call= / loc= override):
 *     tapped line                      next message
 *     CQ [xx] DX GRID / anything not   DX MY GRID
 *       addressed to me (DX = 2nd call)
 *     MY DX GRID   (or no 3rd word)    DX MY -NN        NN = the SNR this app measured for the tapped line
 *     MY DX -NN / +NN                  DX MY R-NN
 *     MY DX R-NN / R+NN                DX MY RR73       (JT4: RRR)
 *     MY DX RRR / RR73                 DX MY 73
 *     MY DX 73                         nothing; the QSO is logged if it was not yet
 * The QSO is logged once, after the RR73 / RRR / 73 transmission went out (or when the DX's 73 is tapped): one ADIF
 * record through the loader ("@QLOG <record>" -> <Apps folder>\ICAPQSO.ADI).  If the card does not take it the app says
 * so and tries again (every 5 s, 6 times).
 * Safety rules of the app side (the loader enforces its own, see "QSO mode" in the SDK README.md):
 *   - a tap on the "TX?" line confirms only when the line has been on the screen for about a second;
 *   - a tap made while the radio was transmitting is dropped (never answered after the transmission);
 *   - nothing is sent without a grant of the loader ("@QTX"), which needs a tap made while the radio was idle.
 *
 * The including file provides, before the #include:
 *   QSO_SEC            period length in s (15 or 60); the decode stamp is "HHMMSS" for 15, "HHMM" for 60
 *   QSO_FINAL          "RR73" or "RRR"
 *   QSO_CALL, QSO_LOC  the CFG override strings ("" = none), QSO_TXF the CFG tone (0 = none)
 *   int  qso_enc(const char *msg, int tone_hz)   prepare the encoder, 0 = ok
 *   int  qso_stream(void)                        send it (tx_begin was granted), 0 = sent
 *   void qso_capdrop(void)                       the capture state after a transmission (closes the capture file)
 *   char *qso_modetag(char *d)                   append the ADIF mode fields ("<MODE:3>FT8 ")
 * and uses beacon.h (bcn_cpy, bcn_u, bcn_field, bcn_vfline).  Freestanding, no libc. */
#ifndef QSO_H
#define QSO_H
#ifndef QSO_FN
#define QSO_FN static __attribute__((noinline))
#endif
/* ms before the period's first sample at which the app leaves the decoder.  The loader keys at T0 - 400 ms by itself and
 * gives up (gate 5, "late") when the first sample has not been written by T0 + 300 ms, so the app has QSO_EARLY + 300 ms for
 * what is in front of the stream: the encoder (CPU only) and one DECODE line (bounded by the loader: <= 100 ms).
 * P5: no SD card access in that path - the capture file is closed AFTER the transmission (qso_capdrop). */
#define QSO_EARLY 1000u
#define QSO_PROMPT_MS 1300u             /* P5: a tap confirms the "TX?" line only when the line is at least this old at the
                                         * poll that sees the tap (taps are polled every 300 ms: the line was on the screen
                                         * for >= 1 s) - a prompt that appears under the finger is not a confirmation */
#define QSO_CALLN 16                    /* own call / locator buffers: the size of the CFG strings (call= 15, loc= 11) */
#define QSO_LOCN  12

static struct {
    uint32_t t_poll, t_due, t_cq, t_prompt, t_log;
    uint16_t seq, tone;
    uint8_t init, have, due, cq, par, kind;             /* due: 1 answer, 2 CQ; kind: 0 grid 1 report 2 R-report 3 final 4 73 */
    uint8_t logged;                     /* this QSO: 0 not logged, 1 on the card, 2 its record waits in qso_rec */
    uint8_t lp;                         /* the record in qso_rec: write attempts left (0 = none waiting) */
    int8_t snr, sent, rcvd;             /* reports, 127 = none */
    char dx[14], grid[6], msg[40];
} Q;
static char qso_rec[256], qso_rdx[14];  /* "@QLOG <record>" + its DX call: kept until the loader has written it */

QSO_FN int qso_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
QSO_FN int qso_tok(const char **pp, char *out, int cap)         /* next blank-separated token, 0 = none */
{
    const char *p = *pp; int n = 0;
    while (*p == ' ') p++;
    while (*p && *p != ' ') { if (n < cap - 1) out[n++] = *p; p++; }
    out[n] = 0; *pp = p;
    return n;
}
QSO_FN int qso_num(const char *s, int *v)                       /* "[+-]digits" -> 1 */
{
    int neg = *s == '-', x = 0, nd = 0;
    if (*s == '-' || *s == '+') s++;
    for (; *s >= '0' && *s <= '9'; s++, nd++) x = x * 10 + (*s - '0');
    *v = neg ? -x : x;
    return nd && !*s;
}
QSO_FN int qso_isgrid(const char *g)
{
    return g[0] >= 'A' && g[0] <= 'R' && g[1] >= 'A' && g[1] <= 'R' && g[2] >= '0' && g[2] <= '9' && g[3] >= '0' && g[3] <= '9' &&
           !g[4] && !qso_eq(g, "RR73");
}
QSO_FN int qso_iscall(const char *c)                            /* looks like a callsign: 3..11 of A-Z 0-9 /, a letter and a digit */
{
    int n = 0, l = 0, d = 0;
    for (; c[n]; n++) {
        if (c[n] >= 'A' && c[n] <= 'Z') l = 1; else if (c[n] >= '0' && c[n] <= '9') d = 1; else if (c[n] != '/') return 0;
    }
    return n >= 3 && n <= 11 && l && d;
}
QSO_FN char *qso_rpt(char *d, int v)                            /* "+NN" / "-NN", -50..+49 */
{
    if (v < -50) v = -50;
    if (v > 49) v = 49;
    *d++ = v < 0 ? '-' : '+';
    if (v < 0) v = -v;
    *d++ = (char)('0' + v / 10); *d++ = (char)('0' + v % 10); *d = 0;
    return d;
}
QSO_FN void qso_say(const char *a, const char *b)               /* one line on the DECODE screen */
{
    char l[64], *q = bcn_cpy(l, a);
    if (b) { int n = 0; while (b[n] && n < 44) n++; for (int i = 0; i < n; i++) *q++ = b[i]; *q = 0; }
    icap_decode_out(l);
}
QSO_FN int qso_file(const char *name, char *b, int cap)         /* a loader virtual file -> b, length (0 = none) */
{
    int fd = icap_open(name, ICAP_O_READ), n = 0;
    if (fd >= 0) { n = icap_read(fd, b, cap - 1); icap_close(fd); }
    if (n < 0) n = 0;
    while (n && (b[n - 1] == '\n' || b[n - 1] == '\r')) n--;
    b[n] = 0;
    return n;
}
QSO_FN void qso_cpyn(char *d, const char *s, int cap)           /* bounded copy, always terminated */
{
    while (--cap > 0 && *s) *d++ = *s++;
    *d = 0;
}
/* own call[QSO_CALLN] + locator[QSO_LOCN] (as long as it is set: 4 .. 10 characters), 1 = both known.  The messages
 * carry the first four characters of the locator (qso_grid4), the log the whole of it. */
QSO_FN int qso_my(char *call, char *loc)
{
    const char *oc = QSO_CALL, *ol = QSO_LOC;
    if (oc[0]) qso_cpyn(call, oc, QSO_CALLN); else if (!bcn_vfline("@MYCALL", call, QSO_CALLN)) return 0;
    if (ol[0]) qso_cpyn(loc, ol, QSO_LOCN); else if (!bcn_vfline("@MYLOC", loc, QSO_LOCN)) return 0;
    return 1;
}
/* "<a> <my> " + the 4-character grid (g != 0) -> Q.msg[40]: 13 + 1 + 15 + 1 + 4 + NUL at most */
QSO_FN char *qso_head(const char *a, const char *my, const char *g)
{
    char *q = bcn_cpy(Q.msg, a);
    *q++ = ' '; q = bcn_cpy(q, my); *q++ = ' '; *q = 0;
    if (g) qso_cpyn(q, g, 5);
    return q;
}
QSO_FN void qso_cancel(void) { if (Q.due) { icap_tx_end(); Q.due = 0; } }
/* ask the loader for one transmission: par 0 / 1, 2 = the CQ period */
QSO_FN void qso_req(int par, int quiet)
{
    char nm[8], b[24]; int r = -99;
    bcn_cpy(nm, "@QTX0"); nm[4] = par == 2 ? 'C' : (char)('0' + par);
    if (qso_file(nm, b, (int)sizeof b)) r = bcn_field(b, "r", -99);
    if (r >= 0) { Q.due = par == 2 ? 2 : 1; Q.t_due = icap_ticks() + (uint32_t)r - QSO_EARLY; if (!quiet) qso_say("QSO: TX armed", 0); }
    else if (!quiet) qso_say(r == ICAP_E_BUSY ? "QSO: radio not ready (clock / USB-D / TX)" : r == -99 ? "QSO: needs the QSO runtime" : "QSO: TX not permitted", 0);
}
QSO_FN void qso_log(const char *my, const char *loc);
QSO_FN void qso_logw(void);
/* a tapped row */
QSO_FN void qso_tap(const char *p)
{
    char t[14], w[4][14], my[QSO_CALLN], loc[QSO_LOCN], *q;
    int nw = 0, snr, f, par, x, mine;
    const char *third = "", *dx;
    qso_tok(&p, t, 14);
    if (qso_eq(t, "TX?")) {                             /* the prompt: send it (again) */
        while (*p == ' ') p++;
        if (Q.have && !Q.due && qso_eq(p, Q.msg) && icap_ticks() - Q.t_prompt >= QSO_PROMPT_MS) qso_req(Q.par, 0);
        return;
    }
    for (x = 0; t[x] >= '0' && t[x] <= '9'; x++) ;
    if (t[x] || x != (QSO_SEC == 60 ? 4 : 6)) goto clear;       /* not a decode row */
    par = QSO_SEC == 60 ? t[3] & 1 : ((t[4] - '0') * 10 + (t[5] - '0')) / QSO_SEC & 1;
    if (!qso_tok(&p, t, 14) || !qso_num(t, &snr)) goto clear;
    qso_tok(&p, t, 14);                                 /* DT */
    if (!qso_tok(&p, t, 14) || !qso_num(t, &f)) goto clear;
    if (qso_tok(&p, t, 14) != 1) goto clear;            /* the mode marker */
    while (nw < 4 && qso_tok(&p, w[nw], 14)) nw++;
    if (nw < 2) goto clear;
    if (!qso_my(my, loc)) { qso_say("QSO: set MY call and position", 0); return; }
    mine = qso_eq(w[0], my);
    dx = w[1];
    if (nw > 2) third = w[nw - 1];
    if (!mine && nw == 4) dx = w[2];                    /* CQ DX <call> <grid> */
    if (!qso_iscall(dx) || !(mine || qso_iscall(w[0]) || qso_eq(w[0], "CQ") || qso_eq(w[0], "QRZ") || qso_eq(w[0], "DE"))) goto clear;   /* free text */
    if (!qso_eq(dx, Q.dx)) { bcn_cpy(Q.dx, dx); Q.grid[0] = 0; Q.sent = Q.rcvd = 127; Q.logged = 0; }   /* a new station (a
                                                         * waiting record of the one before stays in qso_rec) */
    if (qso_isgrid(third)) bcn_cpy(Q.grid, third);
    qso_cancel();
    Q.par = (uint8_t)(par ^ 1); Q.tone = (uint16_t)f; Q.snr = (int8_t)(snr < -50 ? -50 : snr > 49 ? 49 : snr);
    q = qso_head(dx, my, 0);
    if (!mine) { qso_cpyn(q, loc, 5); Q.kind = 0; }
    else if (nw == 2 || qso_isgrid(third)) { qso_rpt(q, Q.snr); Q.kind = 1; }
    else if ((third[0] == '+' || third[0] == '-') && qso_num(third, &x)) { Q.rcvd = (int8_t)x; *q++ = 'R'; qso_rpt(q, Q.snr); Q.kind = 2; }
    else if (third[0] == 'R' && (third[1] == '+' || third[1] == '-') && qso_num(third + 1, &x)) { Q.rcvd = (int8_t)x; bcn_cpy(q, QSO_FINAL); Q.kind = 3; }
    else if (qso_eq(third, "RRR") || qso_eq(third, "RR73")) { bcn_cpy(q, "73"); Q.kind = 4; }
    else if (qso_eq(third, "73")) {                     /* the DX's 73: nothing more to send */
        if (!Q.logged) qso_log(my, loc);
        Q.have = 0;
        qso_say("QSO complete: ", Q.dx);
        return;
    } else goto clear;
    Q.have = 1;
    Q.t_prompt = icap_ticks();
    qso_say("TX? ", Q.msg);
    return;
clear:
    qso_cancel();
    if (Q.have) qso_say("QSO: cleared", 0);
    Q.have = 0;
}
/* poll the loader (taps, CQ arm); 1 = a transmission is due now -> the main loop calls qso_run().  Cheap: reads "@QSO"
 * at most every 300 ms.  May be called from inside a decode (abort callback). */
QSO_FN int qso_poll(void)
{
    uint32_t now = icap_ticks();
    if (now - Q.t_poll >= 300u) {
        char b[104];
        Q.t_poll = now;
        if (qso_file("@QSO", b, (int)sizeof b)) {
            int n = bcn_field(b, "n", 0);
            Q.cq = (uint8_t)(bcn_field(b, "cq", 0) > 0);
            if (!Q.init) { Q.init = 1; Q.seq = (uint16_t)n; }
            else if ((uint16_t)n != Q.seq) {
                const char *t = b;
                Q.seq = (uint16_t)n;
                while (*t && !(t[0] == ' ' && t[1] == 't' && t[2] == '=')) t++;
                if (*t) qso_tap(t + 3);
            }
            if (Q.due == 2 && !Q.cq) Q.due = 0;         /* the owner disarmed CQ (the loader has ended the TX) */
            if (Q.cq && !Q.have && !Q.due && now - Q.t_cq >= 2000u) {
                char my[QSO_CALLN], loc[QSO_LOCN];
                Q.t_cq = now;
                if (qso_my(my, loc)) {
                    qso_head("CQ", my, loc);
                    Q.tone = (uint16_t)(QSO_TXF ? QSO_TXF : 1500);
                    qso_req(2, 1);
                }
            }
        }
        if (Q.lp && !Q.due && now - Q.t_log >= 5000u) qso_logw();     /* P5: a record the card did not take yet */
    }
    return Q.due && (int32_t)(now - Q.t_due) >= 0;
}
QSO_FN char *qso_adif(char *d, const char *tag, const char *v)  /* "<TAG:len>value " */
{
    unsigned n = 0;
    while (v[n]) n++;
    *d++ = '<'; d = bcn_cpy(d, tag); *d++ = ':'; d = bcn_u(d, n); *d++ = '>'; d = bcn_cpy(d, v); *d++ = ' '; *d = 0;
    return d;
}
QSO_FN char *qso_2d(char *d, unsigned v) { *d++ = (char)('0' + v / 10u % 10u); *d++ = (char)('0' + v % 10u); *d = 0; return d; }
/* hand the waiting record to the loader.  P5: the QSO counts as logged only when the loader has written it; a refusal
 * (card busy, I/O error, a transmission armed) is shown and tried again from qso_poll, every 5 s, QSO_LOGTRY times in
 * all.  After the last failure the QSO is "not logged" again: tapping the DX's 73 row makes a new record. */
#define QSO_LOGTRY 6
QSO_FN void qso_logw(void)
{
    char v[24];
    int r = qso_file(qso_rec, v, (int)sizeof v) ? bcn_field(v, "r", -1) : -1;
    Q.t_log = icap_ticks();
    if (r == 0) Q.lp = 0; else Q.lp--;
    if (Q.logged == 2 && !Q.lp) Q.logged = (uint8_t)(r == 0);
    qso_say(r == 0 ? "QSO logged: " : Q.lp ? "QSO LOG FAILED (will retry): " : "QSO NOT LOGGED: ", qso_rdx);
}
QSO_FN void qso_log(const char *my, const char *loc)
{
    char v[24], fq[40], *d = bcn_cpy(qso_rec, "@QLOG "), *q;        /* qso_rec is static: this can run inside a decode */
    struct icap_time t;
    int n, i;
    if (Q.lp) { Q.lp = 1; qso_logw(); }                 /* an earlier QSO's record still waits: its last attempt */
    Q.logged = 2;
    bcn_cpy(qso_rdx, Q.dx);
    d = qso_adif(d, "CALL", Q.dx);
    if (Q.grid[0]) d = qso_adif(d, "GRIDSQUARE", Q.grid);
    d = qso_modetag(d);
    if (Q.sent != 127) { qso_rpt(v, Q.sent); d = qso_adif(d, "RST_SENT", v); }
    if (Q.rcvd != 127) { qso_rpt(v, Q.rcvd); d = qso_adif(d, "RST_RCVD", v); }
    if (icap_clock_now(&t) == 0) {
        q = qso_2d(v, t.year / 100u); q = qso_2d(q, t.year); q = qso_2d(q, t.mon); qso_2d(q, t.day);
        d = qso_adif(d, "QSO_DATE", v);
        q = qso_2d(v, t.hour); q = qso_2d(q, t.min); qso_2d(q, t.sec);
        d = qso_adif(d, "TIME_ON", v);
    }
    n = qso_file("@FREQ", fq, (int)sizeof fq);          /* "f=<Hz> ...": the true dial (IF + transverter), any size */
    if (n > 2 && fq[0] == 'f') {
        const char *h = fq + 2;
        for (n = 0; h[n] >= '0' && h[n] <= '9'; n++) ;
        q = v;
        if (n <= 6) { *q++ = '0'; *q++ = '.'; for (i = n; i < 6; i++) *q++ = '0'; for (i = 0; i < n; i++) *q++ = h[i]; }
        else if (n <= 15) { for (i = 0; i < n; i++) { if (i == n - 6) *q++ = '.'; *q++ = h[i]; } }
        *q = 0;
        if (q != v) d = qso_adif(d, "FREQ", v);
    }
    d = qso_adif(d, "STATION_CALLSIGN", my);
    d = qso_adif(d, "MY_GRIDSQUARE", loc);
    bcn_cpy(d, "<EOR>");                                /* worst case 246 of 256: CALL 13, call 15, locator 11, FREQ 16 */
    Q.lp = QSO_LOGTRY;
    qso_logw();
}
/* the main loop, when qso_poll() returned 1: one transmission */
QSO_FN void qso_run(void)
{
    char l[64], my[QSO_CALLN], loc[QSO_LOCN], *d = l;
    struct icap_time t;
    int kind = Q.due == 2 ? -1 : Q.kind, r;
    Q.due = 0;
    if (qso_enc(Q.msg, Q.tone)) { icap_tx_end(); qso_say("QSO: cannot encode ", Q.msg); return; }
    if (icap_clock_now(&t) == 0) {                      /* the period this goes out in: now + 2 s, floored to the period */
        unsigned s = t.sec + 2u, m = t.min, h = t.hour;
        if (s >= 60u) { s -= 60u; if (++m == 60u) { m = 0; if (++h == 24u) h = 0; } }
        d = qso_2d(d, h); d = qso_2d(d, m);
        if (QSO_SEC != 60) d = qso_2d(d, s - s % (unsigned)QSO_SEC);
        *d++ = ' ';
    }
    d = bcn_cpy(d, "TX "); bcn_cpy(d, Q.msg);
    icap_decode_out(l);
    r = qso_stream();
    qso_capdrop();                                      /* P5: the SD card work comes after the transmission */
    if (r) qso_say("QSO: TX not sent (aborted)", 0);
    else {
        if (kind == 1 || kind == 2) Q.sent = Q.snr;
        if (kind >= 3) {
            if (!Q.logged && qso_my(my, loc)) qso_log(my, loc);
            if (kind == 4 || QSO_FINAL[2] == '7') Q.have = 0;   /* 73 / RR73 sent: the QSO is over (CQ may go on) */
        }
    }
    /* P5: a tap made while we were sending is not a decision - take the loader's tap counter as it is now (a tap made
     * from here on counts again) */
    Q.init = 0; Q.t_poll = icap_ticks() - 300u;
    qso_poll();
}
#endif
