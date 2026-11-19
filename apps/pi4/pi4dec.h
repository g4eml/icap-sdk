/* SPDX-License-Identifier: GPL-3.0-or-later
 * pi4dec.h - PI4 (PharusIgnis4, OZ2M / DJ5HG) beacon decoder, freestanding C.
 *
 * Spec: https://rudius.net/oz2m/ngnb/pi4.htm (tone set K=40, standard PI4).
 *   146 4-FSK symbols of 2000/12000 s (1/6 s), message starts at the UTC
 *   minute; tone n = tone0 + n * 234.375 Hz; tone0 = carrier - 117.1875 Hz
 *   (682.8125 Hz audio for the 800 Hz CW-carrier convention).
 *
 * No malloc, no libc: all memory comes from `work` (see PI4_WORK_BYTES).
 */
#ifndef PI4DEC_H
#define PI4DEC_H

#include <stdint.h>

#ifndef DSRC_T_DEFINED
#define DSRC_T_DEFINED
/* Sample source: read(ctx, start, out, n) fills out[0..n) with samples
 * [start, start+n) (float, any scale) and returns the number of samples
 * written (>= 0), or < 0 on I/O error. Samples past `len` may be left
 * unwritten; the decoder zero-fills them. */
typedef struct {
    void *ctx;
    int (*read)(void *ctx, uint32_t start, float *out, uint32_t n);
    uint32_t len;   /* total samples available */
    uint32_t fs;    /* sample rate, Hz (8000 expected; 6000..12000 accepted) */
} dsrc_t;
#endif

typedef struct {
    char  msg[12];   /* decoded 8-char message, trailing spaces trimmed, NUL terminated */
    float freq_hz;   /* audio frequency of tone 0 (lowest tone) */
    float dt_s;      /* start of symbol 0 minus nominal_start_s, seconds */
    float snr_db;    /* S/N in 2500 Hz reference bandwidth (WSJT convention) */
    int   quality;   /* % of the 146 data bits whose hard decision agrees with the decode */
} pi4_result_t;

#define PI4_NSYM        146
#define PI4_WORK_BYTES  (240u * 1024u)  /* 200..3000 Hz in one FFT pass + 2 refinement passes; less = more passes */
#define PI4_WORK_MIN    (96u * 1024u)   /* below this the call fails */
#define PI4_STATBINS    166             /* candidate statistics are taken per block of this many 3.9 Hz bins */

/* Error returns of pi4_decode */
#define PI4_ERR_ABORT   (-1)
#define PI4_ERR_ARG     (-2)
#define PI4_ERR_IO      (-3)

/*
 * Decode one period.
 *   f_lo, f_hi       search range for the audio frequency of tone 0, Hz
 *                    (e.g. 682.8125 +/- 300 for the 800 Hz carrier convention)
 *   nominal_start_s  position of the UTC minute (expected symbol 0) in the
 *                    sample stream, seconds; start is searched -1 .. +2 s around it
 *   out/maxout       results (distinct messages/frequencies)
 *   work/work_len    scratch memory, >= PI4_WORK_MIN (the result does not depend on the size)
 *   should_abort     optional (may be NULL); polled every few ms of work
 * Returns number of results (>= 0) or PI4_ERR_*.
 */
int pi4_decode(const dsrc_t *src, float f_lo, float f_hi, float nominal_start_s,
               pi4_result_t *out, int maxout, void *work, uint32_t work_len,
               int (*should_abort)(void));

/* Reference encoder (spec section "C source code"): msg is up to 8 chars of
 * "0-9A-Z /" (lower case accepted, others become space), padded with spaces.
 * Writes 146 symbols 0..3. Returns 0. */
int pi4_encode(const char *msg, uint8_t sym[PI4_NSYM]);

#endif
