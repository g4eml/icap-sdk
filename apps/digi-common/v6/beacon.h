/* SPDX-License-Identifier: GPL-3.0-or-later
 * beacon.h - header-only beacon-TX driver shared by the ICAP decoder apps (digiapp.c and the Q65 app).
 *
 * When <MODE>.CFG has beacon=1 the decoder app becomes a BEACON: every period the loader allows it to key, it
 * builds the message from @MYCALL + @MYLOC (unless call=/loc= override in the CFG), encodes it with the mode's
 * encoder and streams the audio through icap_tx_begin/tx_write/tx_end.  Period phase-lock, keying, gating and
 * all TX safety live in the firmware's ICAP loader; this module only builds the message, runs the
 * encoder and keeps the window legible.
 *
 * Depends on icap.h being included first (uses icap_open/read/close, icap_tx_*, icap_set_line, icap_print,
 * icap_should_stop, icap_sleep_ms).  Freestanding: no libc.
 *
 * This is the TX (beacon) side.  The RX side's PSKReporter spotting of decoded stations is a separate,
 * mutually-exclusive path in pskr_app.h (an app is either a beacon OR a decoder+spotter in a given run);
 * both read the same @MYCALL/@MYLOC virtual files, so they never run at once.
 */
#ifndef BEACON_H
#define BEACON_H
#include <stdint.h>

/* mode-specific fill: fill up to n s16 samples into buf, return count written (0 = waveform complete). */
typedef int (*bcn_fill_t)(void *ctx, int16_t *buf, int n);

/* mode-specific prepare: build the message from call (>=1 char) + loc (>=4 char) and initialise the encoder.
 * on success set *fill + *ctx and return 0; on failure return <0 and set *err to a SHORT reason (window text). */
typedef int (*bcn_prepare_t)(const char *call, const char *loc, bcn_fill_t *fill, void **ctx, const char **err);

/* ---- small helpers -------------------------------------------------------- */

static char *bcn_cpy(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }

/* read the first line of a virtual read-only file (e.g. "@MYCALL") into out[cap]; trims trailing blanks;
 * returns the length (0 = empty / file unavailable). */
static int bcn_vfline(const char *name, char *out, int cap)
{
    out[0] = 0;
    int fd = icap_open(name, ICAP_O_READ);
    if (fd < 0) return 0;
    char t[48];
    int n = icap_read(fd, t, (int)sizeof t - 1);
    icap_close(fd);
    if (n <= 0) return 0;
    int k = 0;
    for (int i = 0; i < n && t[i] != '\n' && t[i] != '\r' && k < cap - 1; i++) out[k++] = t[i];
    while (k && out[k - 1] == ' ') k--;
    out[k] = 0;
    return k;
}

/* loader minute-clock lock, parsed from @CLOCK "... L=<0/1> ...": 1 locked, 0 not, -1 @CLOCK unavailable. */
static int bcn_clock_lock(void)
{
    char b[72];
    int n = bcn_vfline("@CLOCK", b, (int)sizeof b);
    if (!n) return -1;
    for (int i = 0; i + 1 < n; i++) if (b[i] == 'L' && b[i + 1] == '=') return b[i + 2] == '1';
    return -1;
}

/* parse a signed integer field " <key>=NN" (matched only at a token start) from a NUL-terminated buffer;
 * returns def when the key is absent / has no digits.  Used to read the loader @TX / @CLOCK k=v lines. */
static int bcn_field(const char *b, const char *key, int def)
{
    for (int i = 0; b[i]; i++) {
        if (i && b[i - 1] != ' ') continue;              /* key must start a token (i==0 or after a blank) */
        int j = 0; while (key[j] && b[i + j] == key[j]) j++;
        if (key[j] || b[i + j] != '=') continue;
        const char *p = b + i + j + 1; int neg = (*p == '-'); if (neg) p++;
        if (*p < '0' || *p > '9') return def;
        int v = 0; while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        return neg ? -v : v;
    }
    return def;
}

static char *bcn_u(char *d, unsigned v)                   /* append an unsigned decimal */
{
    char t[12]; int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) *d++ = t[--k];
    *d = 0; return d;
}

/* map the loader's live TX gate (TXE_* from @TX gate=, 0 = ready) + clock lock to a short window phrase.
 * The gate numbering is the loader's: 0 NONE, 1 STOP, 2 OWNER, 3 APP, 4 STALL, 5 LATE, 6 CAP, 7 RADIO, 8 STOCK, 9 PWR,
 * 10 SD, 11 LAN, 12 CLOCK, 13 REFUSED.
 * tx_gate() never returns CLOCK itself (the minute-clock lock is the separate clk= field), so !clk wins first. */
static const char *bcn_gate_word(int gate, int clk)
{
    if (!clk) return "set clock";                        /* loader minute-clock not locked (TXE_CLOCK equiv) */
    switch (gate) {
        case 0:  return "ready";
        case 1:  return "stopping";                      /* TXE_STOP    */
        case 2:  return "radio TX in use";               /* TXE_OWNER   */
        case 3:  return "app not ready";                 /* TXE_APP     */
        case 4:  return "audio stall";                   /* TXE_STALL   */
        case 5:  return "missed slot";                   /* TXE_LATE    */
        case 6:  return "over time cap";                 /* TXE_CAP     */
        case 7:  return icap_is_ic9700() ? "need USB-D data mode"
                                         : "need USB-D + DATA MOD=WLAN"; /* TXE_RADIO (705: DATA MOD=WLAN) */
        case 8:  return "blocked (PTT/TOT)";             /* TXE_STOCK   */
        case 9:  return "powering off";                  /* TXE_PWR     */
        case 10: return "no SD card";                    /* TXE_SD      */
        case 11: return "remote session (RS-BA1)";       /* TXE_LAN     */
        case 12: return "set clock";                     /* TXE_CLOCK   */
        case 13: return "PTT refused";                   /* TXE_REFUSED */
        default: return "not ready";
    }
}

/* persistent one-line TX status for the app window: "TX: off" / "TX: ready (:NN)" / "TX: <reason>" / "TX: sending".
 * Reads the loader @TX virtual file (beacon=armed, gate=live tx_gate TXE_*, clk=lock, st=state, next=s to next slot).
 * Older loaders (no @TX) -> "TX: n/a"; a loader without gate= (pre-txstat) degrades to the clock-lock state. */
static void bcn_txstat(int line_no)
{
    char b[112];
    int fd = icap_open("@TX", ICAP_O_READ);
    if (fd < 0) { icap_set_line(line_no, "TX: n/a (old loader)"); return; }
    int n = icap_read(fd, b, (int)sizeof b - 1);
    icap_close(fd);
    if (n <= 0) { icap_set_line(line_no, "TX: n/a"); return; }
    b[n] = 0;
    int beacon = bcn_field(b, "beacon", 0), clk = bcn_field(b, "clk", 1);
    int gate = bcn_field(b, "gate", -1), st = bcn_field(b, "st", 0), nx = bcn_field(b, "next", -1);
    char out[40], *q = bcn_cpy(out, "TX: ");
    if (!beacon)                 bcn_cpy(q, "off");                           /* beacon not armed */
    else if (st >= 1 && st <= 3) bcn_cpy(q, st == 1 ? "armed" : "sending");   /* ARMED / KEYING / ON */
    else if (gate < 0)           bcn_cpy(q, clk ? "waiting" : "set clock");   /* pre-txstat loader: no live gate= */
    else if (gate == 0 && clk) { q = bcn_cpy(q, "ready");                     /* will key at the next period */
                                 if (nx >= 0) { q = bcn_cpy(q, " (:"); q = bcn_u(q, (unsigned)nx); *q++ = ')'; *q = 0; } }
    else                         bcn_cpy(q, bcn_gate_word(gate, clk));
    icap_set_line(line_no, out);
}

/* stream one transmission the loader has already armed (icap_tx_begin returned >=0).  Pumps fill()->tx_write
 * until the waveform is done / STOP / a write error, then icap_tx_end().  Returns 0, or the tx_write error. */
static int bcn_stream(bcn_fill_t fill, void *ctx, int16_t *buf, int bufn)
{
    int rc = 0;
    for (;;) {
        int got = fill(ctx, buf, bufn);
        if (got <= 0) break;
        if (icap_should_stop()) { rc = 0; break; }
        int w = icap_tx_write(buf, got);
        if (w != got) { rc = w < 0 ? w : -1; break; }
    }
    icap_tx_end();
    return rc;
}

/* short reason text for an icap_tx_begin() error (clk: the loader clock-lock state from bcn_clock_lock()). */
static const char *bcn_why(int r, int clk)
{
    if (r == ICAP_E_BUSY && clk == 0) return "waiting for clock";       /* fix b: not a hard error */
    if (r == ICAP_E_BUSY)  return "radio busy / not ready";
    if (r == ICAP_E_PERM)  return "not permitted (beacon? limit? stop)";
    if (r == ICAP_E_NOSYS) return "no TX in this runtime";
    return "tx error";
}

/* ---- runtime activation (v6) ---------------------------------------------- *
 * The owner arms / disarms the beacon on the radio (long press of the MODE tile); the loader keeps it in TXB.beacon
 * and publishes it in @TX as "beacon=<0|1> owner=<0|1>" (owner=1: an owner abort latched, no TX until re-armed).
 * The decoder apps poll it and switch decode <-> beacon at run time (the choice is no longer made once at start).
 * bcn_want(dflt): 1 = beacon, 0 = decode.  dflt (the <MODE>.CFG beacon= dev gate) is used only when the loader has
 * no @TX beacon state at all (an older loader / its TX code not loaded); a transient open failure (all 4 app file
 * slots in use) keeps the previous answer.  The loader itself sets beacon=1 at app start when the CFG says so, so
 * the dev override still starts the app straight in beacon mode. */
static int bcn_last = -1;
/* " key=D" (single digit, the key after a blank as in every @TX field but the first) -> D, else -1.  Smaller than
 * bcn_field: the 9700 Q65 window has ~100 B to spare. */
static int bcn_flag(const char *b, const char *key)
{
    for (; *b; b++) {
        if (*b != ' ') continue;
        int j = 0; while (key[j] && b[1 + j] == key[j]) j++;
        if (!key[j] && b[1 + j] == '=') { unsigned d = (unsigned)(b[2 + j] - '0'); return d <= 9u ? (int)d : -1; }
    }
    return -1;
}
__attribute__((noinline)) static int bcn_want(int dflt)
{
    char b[112];
    int a = dflt, fd = icap_open("@TX", ICAP_O_READ);
    if (fd == ICAP_E_NOFD) a = -1;                       /* no free file slot right now: keep the last answer */
    else if (fd >= 0) {
        int n = icap_read(fd, b, (int)sizeof b - 1);
        icap_close(fd);
        if (n <= 0) a = -1;
        else { b[n] = 0; int be = bcn_flag(b, "beacon"); if (be >= 0) a = be && bcn_flag(b, "owner") <= 0; }
    }
    if (a >= 0) bcn_last = a;
    return bcn_last < 0 ? dflt : bcn_last;
}
/* the same, re-read at most every ms milliseconds (cached in between): for the decoders' loops */
static uint32_t bcn_poll_t;
__attribute__((noinline)) static int bcn_want_poll(int dflt, uint32_t ms)
{
    uint32_t now = icap_ticks();
    if (bcn_last >= 0 && now - bcn_poll_t < ms) return bcn_last;
    bcn_poll_t = now;
    return bcn_want(dflt);
}
/* bcn_run: -1 = transmit until STOP (legacy); >= 0 = also return as soon as bcn_want(bcn_follow) says decode */
static int bcn_follow = -1;

/* ---- the beacon loop ------------------------------------------------------ *
 * modename: 3-4 char tag for the window.  prepare: mode glue.  ovr_call/ovr_loc: CFG overrides ("" = none).
 * scratch/scratchn: s16 write buffer (>= 160 recommended).  Runs until STOP, or (bcn_follow >= 0) until the owner
 * disarms the beacon.  Short status -> app window; never touches the DECODE screen (beacon sends nothing to
 * decode_out). */
static void bcn_run(const char *modename, bcn_prepare_t prepare,
                    const char *ovr_call, const char *ovr_loc, int16_t *scratch, int scratchn)
{
    char line[80], *q;
    unsigned sent = 0;
    int last_warn = -2;                                  /* debounce the "set call+grid" / "waiting" notes */

    q = bcn_cpy(line, modename); bcn_cpy(q, " beacon");
    icap_set_line(0, line);
    if (!icap_tx_available()) {
        icap_set_line(2, "beacon: no TX in this runtime (old loader)");
        while (!icap_should_stop() && (bcn_follow < 0 || bcn_want_poll(bcn_follow, 1000))) icap_sleep_ms(200);
        return;
    }

    while (!icap_should_stop()) {
        if (bcn_follow >= 0 && !bcn_want(bcn_follow)) return;   /* disarmed (long press OFF / owner abort) */
        char call[16], loc[12];
        if (!(ovr_call && ovr_call[0] ? (bcn_cpy(call, ovr_call), 1) : bcn_vfline("@MYCALL", call, (int)sizeof call))) {
            if (last_warn != 0) { icap_set_line(2, "beacon idle: set MY call (and grid)"); last_warn = 0; }
            icap_sleep_ms(500); continue;
        }
        if (!(ovr_loc && ovr_loc[0] ? (bcn_cpy(loc, ovr_loc), 1) : bcn_vfline("@MYLOC", loc, (int)sizeof loc))) {
            if (last_warn != 1) { icap_set_line(2, "beacon idle: set grid (GPS/Manual)"); last_warn = 1; }
            icap_sleep_ms(500); continue;
        }
        bcn_fill_t fill; void *ctx; const char *err = "encode error";
        if (prepare(call, loc, &fill, &ctx, &err) < 0) {
            if (last_warn != 2) { q = bcn_cpy(line, "beacon: "); bcn_cpy(q, err); icap_set_line(2, line); icap_print(line); last_warn = 2; }
            icap_sleep_ms(1000); continue;
        }
        last_warn = -2;
        int r = icap_tx_begin();
        if (r < 0) {
            int clk = bcn_clock_lock();
            q = bcn_cpy(line, modename); q = bcn_cpy(q, ": "); bcn_cpy(q, bcn_why(r, clk));
            icap_set_line(2, line);
            icap_sleep_ms(r == ICAP_E_BUSY && clk == 0 ? 500 : 2000);
            continue;
        }
        /* armed: short live status (full detail stays in @TX + the window log) */
        q = bcn_cpy(line, modename); q = bcn_cpy(q, " TX "); q = bcn_cpy(q, call); *q++ = ' '; q = bcn_cpy(q, loc);
        icap_set_line(2, line);
        int rc = bcn_stream(fill, ctx, scratch, scratchn);
        sent++;
        q = bcn_cpy(line, modename); q = bcn_cpy(q, rc ? " TX aborted" : " TX sent");
        icap_set_line(3, line);
        icap_print(line);                                /* window log: one line per transmission */
        { char t[80]; if (bcn_vfline("@TX", t, (int)sizeof t)) icap_set_line(4, t); }   /* loader diagnostics */
    }
}

#endif
