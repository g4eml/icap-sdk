/* SPDX-License-Identifier: GPL-3.0-or-later
 * wsprenc.h - WSPR-2 (2-minute WSPR) transmit encoder for the ICAP digi apps
 * (IC-705 / IC-9700).  Produces the 162 channel symbols (tones 0..3) and a
 * streaming 8 kHz s16 synthesizer for the standard WSPR-2 beacon minute.
 *
 * WSPR-2 spec:  162 symbols, 4-FSK, symbol rate 12000/8192 = 1.4648 baud,
 *   tone spacing 12000/8192 = 1.4648 Hz, continuous-phase; occupied BW ~6 Hz.
 *   TX starts at even UTC minute + 1 s and runs 110.592 s.
 *   Message -> 50 bits (28-bit call n + 22-bit grid/power m) -> K=32 r=1/2
 *   convolutional code (POLY1 0xF2D05351, POLY2 0xE4613C47, 50 info + 31 tail
 *   = 162 output bits) -> bit-reversal interleave -> tone = 2*databit + sync.
 *
 * Message packing (type 1 call+grid4+power, type 2 compound call, type 3 hashed
 * call + 6-char grid) matches WSJT-X get_wspr_channel_symbols()/wsprsim exactly;
 * anything not representable is refused with TXE_* (see txsynth.h).  The message
 * packing logic is derived from WSJT-X (GPL-3.0) wsprsim_utils.c; hence this
 * module is GPL-3.0-or-later.
 *
 * Freestanding: no libc, no malloc, no libm (synth uses txsynth).
 */
#ifndef WSPRENC_H
#define WSPRENC_H
#include <stdint.h>
#include "txsynth.h"

#define WSPR_NSYM            162
#define WSPR_TONE_SPACING_HZ (12000.0 / 8192.0)   /* 1.46484375 Hz           */
#define WSPR_BAUD            (12000.0 / 8192.0)    /* symbol rate, Hz          */
#define WSPR_SPS_NUM         16384u                /* samples/symbol = 16384/3 */
#define WSPR_SPS_DEN         3u                    /*  = 5461.333 @ 8000 Hz    */
#define WSPR_TX_SAMPLES      884736u               /* 162*16384/3, 110.592 s   */
#define WSPR_PERIOD_MS       120000u               /* 2-minute cycle           */
#define WSPR_TX_START_MS     1000u                 /* even minute + 1 s        */
#define WSPR_F0_DEFAULT_HZ   1500.0                /* audio tone-0 frequency   */

/* Pack a full WSPR message string into 162 tones (values 0..3).
 *   "K1ABC FN42 33"      type 1 (standard call, 4-char grid, power dBm)
 *   "PJ4/K1ABC 37"       type 2 (compound/portable call, power)
 *   "<PJ4/K1ABC> FK52UD 37"  type 3 (hashed call, 6-char grid, power)
 * Returns 0, or TXE_MSG / TXE_CALL / TXE_GRID / TXE_ARG if not representable. */
int wspr_pack_message(const char *msg, uint8_t tones[WSPR_NSYM]);

/* Type-1 convenience: standard call + 4-char Maidenhead grid + power dBm.
 * Returns 0 or TXE_*. */
int wspr_encode(const char *call, const char *grid4, int dbm, uint8_t tones[WSPR_NSYM]);

/* ---- streaming synthesizer (phase-continuous 4-FSK, raised-cosine edges) --- */
typedef struct {
    uint8_t    tones[WSPR_NSYM];
    txs_mfsk_t s;
} wspr_tx_t;

/* Initialise a type-1 WSPR transmission.  f0_hz = CENTER audio frequency of the
 * 4 tones (WSPR convention: the reported/dial audio freq is the tone centre;
 * e.g. 1400..1600; 0 -> WSPR_F0_DEFAULT_HZ).  level 0 -> TXS default (-2 dBFS),
 * ramp 0 -> TXS default.  Returns 0 or TXE_*. */
int wspr_tx_init(wspr_tx_t *t, const char *call, const char *grid4, int dbm,
                 double f0_hz, int level, uint32_t ramp);
/* Initialise from any supported WSPR message string (type 1/2/3). */
int wspr_tx_init_msg(wspr_tx_t *t, const char *msg, double f0_hz, int level, uint32_t ramp);
/* Fill up to n s16 samples; returns number written (0 when the frame is done). */
int wspr_tx_fill(wspr_tx_t *t, int16_t *buf, int n);

/* Milliseconds to wait until the next allowed WSPR-2 start (even minute + 1 s).
 * utc_s = UTC seconds, ms = fractional milliseconds.  0 = start now. */
uint32_t wspr_ms_to_next_start(uint32_t utc_s, uint32_t ms);

#endif /* WSPRENC_H */
