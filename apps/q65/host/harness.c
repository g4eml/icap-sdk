/* SPDX-License-Identifier: GPL-3.0-or-later
 * Host harness for Q65.APP (q65_705.c): the real app and decoder on a PC, against a simulated radio with a
 * virtual clock.  Build and examples: build.sh, test.sh, README.md in this directory.
 *
 * usage: harness <dir> <secs> <tx_period> <tx_sub 0..4> <snr> <menu_sub A..E> "<Q65.CFG text or ->" [start_s] [cost_ms]
 *   - start_s: UTC seconds of the day at launch (default 45230.4 = 12:33:50.4); secs: how long the app runs;
 *   - env HEAP=<bytes>: size of the app data region left for the app (default the IC-705's: 240 KiB - segment 2);
 *     env TRACE=1: print the app's debug log (Q65DBG.TXT lines) as it is written;
 *   - audio: one Q65 transmission (tx_period / tx_sub) in every T/R period, WSJT-X TX start + dt, in white noise;
 *     message, tone and dt vary per period so every decode can be matched to its period;
 *   - files live in <dir>; at most 4 open at once (the loader's limit) - a 5th open fails and is reported;
 *   - "@MODECFG" reads "mode=Q65 sub=<menu_sub> pskr=0 gen=1", "@SPOT" accepts writes, other '@' names fail;
 *   - virtual time: sleep_ms adds its ms, yield 1 ms, every f_read / f_write cost_ms (default 3: the decode takes
 *     simulated time, so the capture of the next period runs alongside it).
 * Prints the DECODE screen lines, the app window lines 0 and 4, and the transmitted schedule. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/mman.h>
#include "icap.h"
#include "q65enc.h"

int app_main(const struct icap_svc *svc);

static char g_dir[512];
static uint32_t g_ms, g_end_ms, g_cost = 3;
static double g_t0;                                   /* UTC seconds of the day at g_ms = 0 */
static int16_t *g_audio; static uint32_t g_audio_n, g_consumed;
static char g_menu_sub = 'D';
static int g_maxopen, g_nopen, g_open_fail;

static void h_print(const char *s) { printf("[print] %s\n", s); }
static char g_lines[8][96];
static void h_set_line(int l, const char *s)
{
    if (l < 0 || l > 7) return;
    if (strcmp(g_lines[l], s)) { snprintf(g_lines[l], sizeof g_lines[l], "%s", s); if (l == 0 || l == 4) printf("%9.3f [line%d] %s\n", g_t0 + g_ms / 1000.0, l, s); }
}
static void h_clear(void) { memset(g_lines, 0, sizeof g_lines); }

/* files */
static struct { FILE *f; int kind; } fds[4];         /* kind 0 file, 1 @MODECFG (read pos in f), 2 @SPOT */
static int h_open(const char *name, int mode)
{
    int i; for (i = 0; i < 4 && (fds[i].f || fds[i].kind); i++) ;
    if (i == 4) { g_open_fail++; printf("!! 5th open file refused: %s\n", name); return ICAP_E_NOFD; }
    if (name[0] == '@') {
        if (!strcmp(name, "@MODECFG") && !(mode & 1)) {
            fds[i].f = tmpfile(); fprintf(fds[i].f, "mode=Q65 sub=%c pskr=0 gen=1\n", g_menu_sub); rewind(fds[i].f); fds[i].kind = 1;
        } else if (!strcmp(name, "@SPOT") && (mode & 1)) { fds[i].kind = 2; }
        else return ICAP_E_NAME;
    } else {
        char p[700]; snprintf(p, sizeof p, "%s/%s", g_dir, name);
        fds[i].f = fopen(p, (mode & 1) ? "wb" : "rb");
        if (!fds[i].f) return ICAP_E_IO;
        fds[i].kind = 0;
    }
    if (++g_nopen > g_maxopen) g_maxopen = g_nopen;
    return i;
}
static int h_read(int fd, void *b, uint32_t n)
{
    if (fd < 0 || fd > 3 || !fds[fd].f) return ICAP_E_ARG;
    g_ms += g_cost;
    return (int)fread(b, 1, n, fds[fd].f);
}
static int h_write(int fd, const void *b, uint32_t n)
{
    if (fd < 0 || fd > 3) return ICAP_E_ARG;
    if (fds[fd].kind == 2) { printf("%9.3f [spot] %.*s", g_t0 + g_ms / 1000.0, (int)n, (const char *)b); return (int)n; }
    if (!fds[fd].f) return ICAP_E_ARG;
    g_ms += g_cost;
    return (int)fwrite(b, 1, n, fds[fd].f);
}
static int h_close(int fd)
{
    if (fd < 0 || fd > 3 || (!fds[fd].f && !fds[fd].kind)) return ICAP_E_ARG;
    if (fds[fd].f) fclose(fds[fd].f);
    fds[fd].f = 0; fds[fd].kind = 0; g_nopen--;
    return 0;
}

/* time */
static void h_sleep(uint32_t ms) { g_ms += ms; }
static void h_yield(void) { g_ms += 1; }
static uint32_t h_tick(void) { return g_ms; }
static int h_stop(void) { return g_ms >= g_end_ms; }
static int h_clock(struct icap_time *t)
{
    double s = fmod(g_t0 + g_ms / 1000.0, 86400.0);
    uint32_t si = (uint32_t)s;
    t->year = 2026; t->mon = 11; t->day = 23; t->wday = 1;
    t->hour = (uint8_t)(si / 3600); t->min = (uint8_t)(si / 60 % 60); t->sec = (uint8_t)(si % 60);
    return 0;
}

/* audio: everything up to "now" is available; more than 1 s behind -> overrun (the loader ring) */
static int h_audio_read(int16_t *b, int n)
{
    uint32_t avail_to = g_ms * 8u;
    if (avail_to > g_audio_n) avail_to = g_audio_n;
    if (avail_to > g_consumed + 8000u) { g_consumed = avail_to - 4000u; return ICAP_E_OVERRUN; }
    uint32_t k = avail_to - g_consumed; if (k > (uint32_t)n) k = (uint32_t)n;
    memcpy(b, g_audio + g_consumed, k * 2u); g_consumed += k;
    return (int)k;
}
static int h_rate(void) { return 8000; }
static void h_decode_out(const char *s) { printf("%9.3f [DECODE] %s\n", g_t0 + g_ms / 1000.0, s); }
static int h_mode(char *b, int max) { snprintf(b, max, "Q65"); return 3; }

void host_trace(const char *s, uint32_t n)
{
    if (getenv("TRACE")) printf("%9.3f   dbg: %.*s\n", g_t0 + g_ms / 1000.0, (int)n, s);
}

static double gauss(void)
{
    double u1 = (rand() + 1.0) / (RAND_MAX + 2.0), u2 = (rand() + 1.0) / (RAND_MAX + 2.0);
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

int main(int argc, char **argv)
{
    if (argc < 8) { fprintf(stderr, "usage: harness dir secs tx_period tx_sub snr menu_sub cfg [start] [cost]\n"); return 2; }
    snprintf(g_dir, sizeof g_dir, "%s", argv[1]);
    uint32_t secs = (uint32_t)atoi(argv[2]);
    int txp = atoi(argv[3]), txs = atoi(argv[4]);
    double snr = atof(argv[5]);
    g_menu_sub = argv[6][0];
    g_t0 = argc > 8 ? atof(argv[8]) : 12 * 3600 + 33 * 60 + 50.4;
    if (argc > 9) g_cost = (uint32_t)atoi(argv[9]);
    g_end_ms = secs * 1000u;
    { char p[700]; snprintf(p, sizeof p, "%s/Q65.CFG", g_dir); remove(p);
      if (strcmp(argv[7], "-")) { FILE *f = fopen(p, "wb"); fputs(argv[7], f); fclose(f); } }

    /* the audio stream: noise + one transmission per period */
    g_audio_n = (secs + 2) * 8000u;
    float *x = calloc(g_audio_n, sizeof *x);
    static const char *msgs[] = { "CQ G4ABC IO91", "G4ABC SP6XYZ JO81", "SP6XYZ G4ABC -12", "G4ABC SP6XYZ R-15", "SP6XYZ G4ABC RR73" };
    double first = ceil(g_t0 / txp) * txp - txp;      /* the period that contains t0 */
    for (int k = 0; ; k++) {
        double ps = first + k * txp;
        if (ps - g_t0 > secs) break;
        static q65_tx_t t;
        double f0 = 800.0 + 100.0 * (k % 6), dt = 0.1 * (k % 4) - 0.1;
        if (q65_tx_init(&t, msgs[k % 5], f0, txp, txs, 1000, 0)) { fprintf(stderr, "encode failed\n"); return 1; }
        long pos = lround((ps - g_t0 + q65_tx_start_ms(txp) / 1000.0 + dt) * 8000.0);
        int16_t b[512]; int m;
        while ((m = q65_tx_fill(&t, b, 512)) > 0)
            for (int i = 0; i < m; i++, pos++) if (pos >= 0 && pos < (long)g_audio_n) x[pos] = b[i];
        int hh = (int)fmod(ps, 86400.0) / 3600, mm = (int)fmod(ps, 3600.0) / 60, ss = (int)fmod(ps, 60.0);
        printf("   sent %02d%02d%02d  Q65-%d%c  f0=%4.0f dt=%+.1f  %s\n", hh, mm, ss, txp, 'A' + txs, f0, dt, msgs[k % 5]);
    }
    double sigma = sqrt(1000.0 * 1000.0 / 2.0 / pow(10.0, snr / 10.0) * 4000.0 / 2500.0);
    g_audio = malloc(g_audio_n * 2u);
    for (uint32_t i = 0; i < g_audio_n; i++) {
        double v = x[i] + sigma * gauss();
        g_audio[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrint(v));
    }

    /* the app data region below 4 GB (the app keeps region addresses in 32-bit integers) */
    uint32_t hsize = getenv("HEAP") ? (uint32_t)atoi(getenv("HEAP")) : 240u * 1024u - 21856u;   /* IC-705: region - segment 2 */
    void *heap = mmap(0, hsize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (heap == MAP_FAILED) { perror("mmap"); return 1; }

    static struct icap_svc svc;
    svc.abi_version = 3; svc.radio_id = 0; svc.size = sizeof svc;
    svc.print = h_print; svc.set_line = h_set_line; svc.clear = h_clear;
    svc.f_open = h_open; svc.f_read = h_read; svc.f_write = h_write; svc.f_close = h_close;
    svc.sleep_ms = h_sleep; svc.yield = h_yield; svc.tick_ms = h_tick; svc.should_stop = h_stop;
    svc.heap_base = (uint32_t)(uintptr_t)heap; svc.heap_size = hsize;
    svc.clock_now = h_clock; svc.audio_read = h_audio_read; svc.audio_rate = h_rate; svc.decode_out = h_decode_out;
    svc.mode = h_mode;
    int rc = app_main(&svc);
    printf("== app_main rc=%d  max open files=%d  refused opens=%d\n", rc, g_maxopen, g_open_fail);
    return 0;
}
/* the no_std decoder staticlib references it; panic=abort never unwinds (as stubs.c on the radio) */
int rust_eh_personality(void) { return 0; }
