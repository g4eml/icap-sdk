/* SPDX-License-Identifier: GPL-3.0-or-later
 * ft8enc.h - FT8 transmit encoder (tones + streaming 8 kHz GFSK audio) for the ICAP apps.
 *
 * Message: WSJT-X 77-bit standard message via msg77 (beacon convention "DE <CALL> <LOC4>").
 * Tones: ft8_lib ft8_encode (CRC-14, LDPC(174,91), Gray map, 3 Costas arrays) -> 79 x 8-FSK.
 * Audio: GFSK, BT = 2, 6.25 Hz spacing, 1280 samples/symbol at 8 kHz; WSJT-X gen_ft8wave
 *        shape incl. its nsps/8 (20 ms) raised-cosine ramp at both ends.  f0 = tone 0 frequency
 *        (= WSJT-X "Tx frequency").
 */
#ifndef FT8ENC_H
#define FT8ENC_H
#include <stdint.h>
#include "txsynth.h"

#define FT8_NSYM          79
#define FT8_PERIOD_MS     15000u     /* :00 :15 :30 :45 */
#define FT8_TX_START_MS   500u       /* WSJT-X Modulator: 0.5 s into the period */
#define FT8_NSPS          1280u      /* 1920 at 12 kHz */
#define FT8_TX_SAMPLES    101120u    /* 79 * 1280 */
#define FT8_TX_MS         12640u
#define FT8_RAMP          160u       /* nsps / 8, as WSJT-X */

/* msg -> 79 tones (0..7).  Returns 0 or TXE_*. */
int ft8_encode_msg(const char *msg, uint8_t tones[FT8_NSYM]);

typedef struct {
    uint8_t tones[FT8_NSYM];
    txs_mfsk_t s;
} ft8_tx_t;

/* f0_hz: tone 0 frequency; level: peak amplitude (0 = TXS_LEVEL_DEFAULT). */
int ft8_tx_init(ft8_tx_t *t, const char *msg, double f0_hz, int level);
static inline int ft8_tx_fill(ft8_tx_t *t, int16_t *buf, int n) { return txs_mfsk_fill(&t->s, buf, n); }

#endif
