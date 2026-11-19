/* SPDX-License-Identifier: MIT
 * txsynth.h - streaming MFSK / GFSK / keyed-carrier audio synthesizer for the ICAP TX encoders
 * (q65enc, ft8enc, jt4enc, pi4enc), plus the UTC period-alignment helper.
 *
 * Output: 8000 Hz mono s16 (what icap_tx_write() takes).  Freestanding: no libc, no libm,
 * no malloc.  Deterministic: integer 32-bit phase accumulator, own sine polynomial, IEEE
 * float/double + - * only (build with -ffp-contract=off so x86 and ARM give identical samples).
 *
 * Phase-continuous FSK: the phase accumulator is never reset between symbols.
 * Fractional symbol timing: samples/symbol = sps_num / sps_den exactly (e.g. JT4 12800/7,
 * PI4 4000/3); symbol k covers samples [ceil(k*num/den), ceil((k+1)*num/den)).
 * GFSK (FT8): WSJT-X gen_ft8wave pulse (BT = 2), 64-point table + linear interpolation.
 * Ramps: raised-cosine (sin^2) ramp up at the first and down at the last sample of a keyed
 * segment, never inside it (so no amplitude modulation between symbols).
 */
#ifndef TXSYNTH_H
#define TXSYNTH_H
#include <stdint.h>

#define TXS_FS             8000
#define TXS_LEVEL_DEFAULT  26029   /* peak sample amplitude, -2 dBFS (29204 = -1, 23198 = -3 dBFS) */
#define TXS_RAMP_DEFAULT   40      /* samples (5 ms) */

/* Error codes shared by all encoders (returned as negative ints) */
#define TXE_OK        0
#define TXE_ARG     (-1)   /* NULL pointer / out-of-range parameter (frequency, submode, level) */
#define TXE_CHAR    (-2)   /* character not in the mode's alphabet */
#define TXE_LEN     (-3)   /* message / field too long */
#define TXE_CALL    (-4)   /* callsign cannot be encoded exactly by this protocol */
#define TXE_GRID    (-5)   /* locator invalid */
#define TXE_MSG     (-6)   /* message is not of a supported form */

/* One MFSK waveform (a sequence of tones), streamed. */
typedef struct {
    const uint8_t *tones;      /* nsym tone indices (caller-owned, must stay valid) */
    uint32_t nsym;
    uint32_t sps_num, sps_den; /* samples per symbol = num/den */
    uint32_t nsamp;            /* total samples = ceil(nsym*num/den) */
    uint32_t n;                /* next sample index */
    uint32_t phase;            /* phase accumulator, 2^32 = one cycle */
    double   inc0;             /* phase increment of tone 0 (base frequency) */
    double   incstep;          /* phase increment per tone step */
    float    level;            /* peak amplitude */
    uint32_t ramp;             /* ramp length, samples */
    uint8_t  gfsk;             /* 1 = FT8 GFSK (BT=2) frequency shaping */
} txs_mfsk_t;

/* f0 = frequency of tone 0, df = tone spacing (Hz).  level 0 -> default, ramp ~0 -> default
 * (ramp = 0xffffffff gives no ramp).  Returns TXE_OK or TXE_ARG (tone frequencies must lie
 * in 0 < f < 4000 Hz). */
int txs_mfsk_init(txs_mfsk_t *s, const uint8_t *tones, uint32_t nsym, uint32_t sps_num,
                  uint32_t sps_den, double f0, double df, uint32_t ntones, int gfsk,
                  int level, uint32_t ramp);
/* Write up to n samples; returns the number written (0 when the waveform is complete). */
int txs_mfsk_fill(txs_mfsk_t *s, int16_t *buf, int n);

/* ---- building blocks (used by pi4enc for the CW ID / carrier) ---- */
float    txs_sin(uint32_t phase);                 /* sin(2*pi*phase/2^32), |err| < 4e-6 */
uint32_t txs_hz_to_inc(double hz);                /* phase increment for hz at TXS_FS */
float    txs_ramp_gain(uint32_t k, uint32_t len, uint32_t ramp);  /* envelope of sample k of a len-sample keyed segment */
int16_t  txs_q16(float v);                        /* round to nearest + saturate to s16 */

/* ---- period alignment ----
 * Transmissions may only start at  (k * period_ms + start_ms)  of UTC (period_ms divides 86 400 000).
 * now: UTC time as seconds (of day, or any count of seconds since a UTC midnight / the Unix epoch)
 * plus milliseconds.  Returns the milliseconds to wait until the next allowed start (0 = start
 * now).  A start that passed at most late_ok_ms ago counts as "now" (returns 0); anything later
 * waits for the next period: never start mid-period. */
uint32_t txs_ms_to_next_start(uint32_t utc_s, uint32_t ms, uint32_t period_ms, uint32_t start_ms,
                              uint32_t late_ok_ms);

#endif
