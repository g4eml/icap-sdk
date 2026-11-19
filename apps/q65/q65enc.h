/* SPDX-License-Identifier: GPL-3.0-or-later
 * q65enc.h - Q65 transmit encoder (tones + streaming 8 kHz audio) for the ICAP apps.
 *
 * Message: WSJT-X 77-bit standard message via msg77 (beacon convention "DE <CALL> <LOC4>").
 * Code: QRA (15,65) over GF(64) with CRC-12, 2 CRC symbols punctured -> 63 symbols,
 *       + 22 sync symbols (tone 0) = 85 symbols; data tone = codeword symbol + 1.
 * Audio: 65-FSK, tone k at f0 + k * 2^submode * baud (f0 = sync tone = WSJT-X "Tx frequency").
 */
#ifndef Q65ENC_H
#define Q65ENC_H
#include <stdint.h>
#include "txsynth.h"

#define Q65_NSYM 85

/* Q65-60 (the beacon mode) constants */
#define Q65_60_PERIOD_MS     60000u
#define Q65_60_TX_START_MS   1000u        /* WSJT-X: 1.0 s after the minute (0.5 s for Q65-15/30) */
#define Q65_60_NSPS          4800u        /* samples / symbol at 8 kHz (7200 at 12 kHz) */
#define Q65_60_TX_SAMPLES    408000u      /* 85 * 4800 */
#define Q65_60_TX_MS         51000u
#define Q65_SUBMODE_DEFAULT  3            /* 0..4 = A..E; D */

/* Period table helpers: period_s in {15, 30, 60, 120, 300}.  Return 0 for an unknown period. */
uint32_t q65_period_ms(int period_s);
uint32_t q65_tx_start_ms(int period_s);   /* 500 for 15/30 s, else 1000 (WSJT-X Modulator) */
uint32_t q65_tx_samples(int period_s);    /* exact: 85 * nsps(8 kHz), rounded up */

/* msg -> 85 tones (0..64).  Returns 0 or TXE_*. */
int q65_encode(const char *msg, uint8_t tones[Q65_NSYM]);

typedef struct {
    uint8_t tones[Q65_NSYM];
    txs_mfsk_t s;
} q65_tx_t;

/* period_s: 15/30/60/120/300; submode 0..4 (A..E); f0_hz: sync tone (tone 0) frequency;
 * level: peak amplitude (0 = TXS_LEVEL_DEFAULT); ramp: samples (0 = default). */
int q65_tx_init(q65_tx_t *t, const char *msg, double f0_hz, int period_s, int submode,
                int level, uint32_t ramp);
/* Write up to n samples of the transmission (starting at the TX start offset); 0 = done. */
static inline int q65_tx_fill(q65_tx_t *t, int16_t *buf, int n) { return txs_mfsk_fill(&t->s, buf, n); }

#endif
