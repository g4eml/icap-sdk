/* SPDX-License-Identifier: GPL-3.0-or-later
 * Host test-signal generator: one Q65 T/R period at 8 kHz, s16 mono WAV, from the app's own encoder.
 * usage: q65gen out.wav period_s submode(0..4) f0_hz snr_db(2500 Hz) dt_s "MESSAGE" [seed]
 * Decode with ../decoder/q65beacon/target/release/q65beacon_cli out.wav --period P --sub X --fmin 600 --fmax 1800.
 * The signal starts at the WSJT-X TX start (0.5 s for 15/30, 1.0 s for 60) + dt_s. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "q65enc.h"

static double gauss(void)
{
    double u1 = (rand() + 1.0) / (RAND_MAX + 2.0), u2 = (rand() + 1.0) / (RAND_MAX + 2.0);
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}
static void w32(FILE *f, unsigned v) { fputc(v, f); fputc(v >> 8, f); fputc(v >> 16, f); fputc(v >> 24, f); }
static void w16(FILE *f, unsigned v) { fputc(v, f); fputc(v >> 8, f); }

int main(int argc, char **argv)
{
    if (argc < 8) { fprintf(stderr, "usage: q65gen out.wav period sub f0 snr dt MSG [seed]\n"); return 2; }
    int period = atoi(argv[2]), sub = atoi(argv[3]);
    double f0 = atof(argv[4]), snr = atof(argv[5]), dt = atof(argv[6]);
    srand(argc > 8 ? atoi(argv[8]) : 1);
    static q65_tx_t t;
    int rc = q65_tx_init(&t, argv[7], f0, period, sub, 1000, 0);
    if (rc) { fprintf(stderr, "q65_tx_init rc=%d\n", rc); return 1; }
    unsigned n = (unsigned)period * 8000u;
    float *x = calloc(n, sizeof *x);
    long st = lround((q65_tx_start_ms(period) / 1000.0 + dt) * 8000.0);
    int16_t b[512]; int k; long pos = st;
    while ((k = q65_tx_fill(&t, b, 512)) > 0) {
        for (int i = 0; i < k; i++, pos++) if (pos >= 0 && pos < (long)n) x[pos] = b[i];
    }
    /* signal power A^2/2 (A = 1000); noise sigma^2 over 4 kHz, SNR referenced to 2500 Hz */
    double ps = 1000.0 * 1000.0 / 2.0;
    double sigma = sqrt(ps / pow(10.0, snr / 10.0) * 4000.0 / 2500.0);
    FILE *f = fopen(argv[1], "wb");
    fwrite("RIFF", 1, 4, f); w32(f, 36 + n * 2); fwrite("WAVEfmt ", 1, 8, f);
    w32(f, 16); w16(f, 1); w16(f, 1); w32(f, 8000); w32(f, 16000); w16(f, 2); w16(f, 16);
    fwrite("data", 1, 4, f); w32(f, n * 2);
    for (unsigned i = 0; i < n; i++) {
        double v = x[i] + sigma * gauss();
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        w16(f, (unsigned)(int16_t)lrint(v));
    }
    fclose(f);
    return 0;
}
