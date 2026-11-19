/* SPDX-License-Identifier: GPL-3.0-or-later
 * jt4enc.h - JT4 transmit encoder (tones + streaming 8 kHz audio) for the ICAP apps.
 *
 * Message: beacon convention "<CALL> <LOC4>".  WSJT-X packs this as a 72-bit FREE TEXT message
 *   (a standard JT message needs two callsigns), so it is limited to 13 characters of
 *   "0-9A-Z +-./?" ("SR6LEG/P JO81" fits; longer compound calls do not -> TXE_LEN, where WSJT-X
 *   would silently truncate).  Messages WSJT-X might pack as a standard message are refused
 *   (TXE_MSG) instead of being sent as text, so the encoding is always what WSJT-X sends.
 * Code: K=32 r=1/2 convolutional code (72 + 31 tail bits -> 206), JT4 bit-reversal interleave,
 *   tone = 2*data + sync (npr), 206 x 4-FSK.
 * Audio: 4.375 Hz * {1,2,4,9,18,36,72} spacing (A..G), 2520/11025 s symbols = 12800/7 samples
 *   at 8 kHz (exact fractional timing); f0 = tone 0 frequency (= WSJT-X "Tx frequency").
 */
#ifndef JT4ENC_H
#define JT4ENC_H
#include <stdint.h>
#include "txsynth.h"

#define JT4_NSYM             206
#define JT4_PERIOD_MS        60000u
#define JT4_TX_START_MS      1000u        /* WSJT-X: 1.0 s after the minute */
#define JT4_SPS_NUM          12800u       /* samples/symbol at 8 kHz = 12800/7 = 1828.571... */
#define JT4_SPS_DEN          7u
#define JT4_TX_SAMPLES       376686u      /* ceil(206 * 12800 / 7) */
#define JT4_TX_MS            47089u       /* 47.0884 s, rounded up */
#define JT4_SUBMODE_DEFAULT  6            /* 0..6 = A..G; G */

/* Builds "<call> <loc4>" (LOC6 accepted, sent as LOC4) into out (>= 24 bytes). 0 or TXE_*. */
int jt4_beacon_text(const char *call, const char *loc, char *out);
/* msg -> 206 tones (0..3).  Returns 0 or TXE_*. */
int jt4_encode(const char *msg, uint8_t tones[JT4_NSYM]);

typedef struct {
    uint8_t tones[JT4_NSYM];
    txs_mfsk_t s;
} jt4_tx_t;

int jt4_tx_init(jt4_tx_t *t, const char *msg, double f0_hz, int submode, int level, uint32_t ramp);
static inline int jt4_tx_fill(jt4_tx_t *t, int16_t *buf, int n) { return txs_mfsk_fill(&t->s, buf, n); }

#endif
