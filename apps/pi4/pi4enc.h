/* SPDX-License-Identifier: MIT
 * pi4enc.h - PI4 beacon transmit encoder (symbols + the standard PI4 beacon minute as streaming
 * 8 kHz audio) for the ICAP apps.  Own implementation from the published PI4 specification
 * (OZ2M / DJ5HG, https://rudius.net/oz2m/ngnb/pi4.htm); proven bit-exact against the OZ2M
 * reference encoder PI4Symbols.cpp.
 *
 * Symbols: 8 chars of "0-9A-Z /" -> 42-bit number -> K=32 r=1/2 convolutional code
 *   (0xF2D05351, 0xE4613C47; 42 + 31 tail bits -> 146) -> bit-reversal interleave ->
 *   symbol = sync + 2*data, 146 x 4-FSK, 1/6 s symbols (4000/3 samples at 8 kHz, exact).
 * Tones: tone n = tone0 + n * 234.375 Hz; CW carrier = tone0 + 117.1875 Hz
 *   (tone0 682.8125 Hz <-> 800 Hz carrier).
 *
 * Beacon minute (spec "mixed mode PI4 + CW + carrier"), times from the UTC minute:
 *   0.000 .. 24.333  PI4 frame
 *  24.333 .. 25.000  pause (no signal)
 *  25.000 ..         CW ID at the carrier frequency: callsign, 500 ms, locator, 500 ms
 *                    (100 ms dot = 12 WPM; dash 3, element gap 1, character gap 3 dots)
 *        .. 59.500   carrier
 *  59.500 .. 60.000  pause (no signal); the caller unkeys when pi4_tx_fill returns 0
 * CW is on/off keyed by default; cw_fsk_shift_hz > 0 sends F1A instead (key-up = carrier -
 * shift, continuous signal 25.0 .. 59.5 s; the spec gives 250 Hz for 30 MHz-1 GHz, 400 Hz above).
 */
#ifndef PI4ENC_H
#define PI4ENC_H
#include <stdint.h>
#include "txsynth.h"

#ifndef PI4_NSYM
#define PI4_NSYM 146
#endif
#define PI4_TONE_SPACING_HZ    234.375
#define PI4_CARRIER_OFFSET_HZ  117.1875      /* carrier = tone0 + this */
#define PI4_TONE0_DEFAULT_HZ   682.8125      /* 800 Hz carrier */
#define PI4_SPS_NUM            4000u         /* samples/symbol = 4000/3 */
#define PI4_SPS_DEN            3u

/* minute layout (TX starts at :00 of every minute) */
#define PI4_PERIOD_MS          60000u
#define PI4_TX_START_MS        0u
#define PI4_FRAME_SAMPLES      194667u       /* ceil(146 * 4000 / 3) */
#define PI4_FRAME_MS           24334u        /* 24.3333 s, rounded up */
#define PI4_CW_START_MS        25000u
#define PI4_CW_DOT_MS          100u
#define PI4_CW_WORD_GAP_MS     500u          /* after the callsign and after the locator */
#define PI4_CARRIER_END_MS     59500u
#define PI4_TX_SAMPLES         476000u       /* keyed part of the minute, 0 .. 59.5 s */
#define PI4_TX_MS              59500u

/* msg: up to 8 chars of 0-9 A-Z (any case), space, '/'; padded with spaces.  0 or TXE_*. */
int pi4enc_symbols(const char *msg, uint8_t sym[PI4_NSYM]);

typedef struct {
    uint8_t  sym[PI4_NSYM];
    uint8_t  key[44];           /* CW key-down bitmap, one bit per 100 ms unit from 25.0 s */
    uint16_t units;             /* CW units incl. the final 500 ms gap */
    uint8_t  cw;                /* 0 = PI4 frame only */
    uint8_t  fsk;
    txs_mfsk_t s;               /* the PI4 frame */
    uint32_t n;                 /* sample index from the minute */
    uint32_t phase, inc_c, inc_sp;
    float    level;
    uint32_t ramp;
} pi4_tx_t;

/* msg: PI4 payload (callsign, <= 8 chars).  cw_call/cw_loc: CW ID (cw_call NULL = PI4 frame
 * only, no CW / carrier; cw_loc NULL or "" = callsign only; LOC4 or LOC6).  tone0_hz: PI4 tone 0
 * (carrier - 117.1875).  cw_fsk_shift_hz: 0 = on/off CW.  level 0 / ramp 0 = defaults.
 * Returns 0 or TXE_* (TXE_LEN if the CW ID does not end before 59.5 s). */
int pi4_tx_init(pi4_tx_t *t, const char *msg, const char *cw_call, const char *cw_loc,
                double tone0_hz, double cw_fsk_shift_hz, int level, uint32_t ramp);
int pi4_tx_fill(pi4_tx_t *t, int16_t *buf, int n);
/* time of the carrier start, ms from the minute (CW end + 500 ms); 0 if no CW */
uint32_t pi4_carrier_start_ms(const pi4_tx_t *t);

#endif
