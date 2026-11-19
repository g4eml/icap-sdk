/* SPDX-License-Identifier: GPL-3.0-or-later
 * jt4dec.h - JT4 (A..G) decoder for the IC-705 ICAP digi app. Freestanding C,
 * no malloc, no libc. Algorithm and tables follow WSJT-X (lib/jt4*.f90, gen4,
 * sync4/xcor4, decode4, extract4, fano232, packjt), GPL-3.0.
 */
#ifndef JT4DEC_H
#define JT4DEC_H
#include <stdint.h>
#include "digi_util.h"   /* dsrc_t */

typedef struct {
    char  msg[24];       /* decoded text, NUL terminated (<=22 chars) */
    float freq_hz;       /* audio frequency of tone 0 (WSJT-X "Freq" convention) */
    float dt_s;          /* signal start minus nominal_start_s (WSJT-X "DT" when nominal=1.0) */
    float snr_db;        /* SNR in 2500 Hz reference bandwidth */
    int   quality;       /* sync strength: 10 x sync z-score (always >= 50; pure-noise peaks reach ~45) */
} jt4_result_t;

#define JT4_WORK_MIN   (96u * 1024u)   /* below this the call fails; the app gives ~240 KiB */
#define JT4_ERR_ARGS   (-1)
#define JT4_ERR_ABORT  (-2)            /* aborted before anything was decoded */
#define JT4_ERR_IO     (-3)

/* Decode one ~60 s JT4 period.
 *   src           : audio (fs 6000..8960 Hz, 8000 Hz expected), read only via src->read
 *   submode       : 0..6 = A..G
 *   f_lo, f_hi    : search range for tone 0 frequency (Hz); clipped so tone 3 stays < fs/2
 *   nominal_start_s: expected TX start inside the buffer (s); searched -1..+5 s around it
 *                   (JT4_DT_LO/JT4_DT_HI; +5 covers EME delay). Capture >= nominal+52 s.
 *   out/maxout    : results in try order (strongest sync first), one entry per distinct message
 *   work/work_len : scratch (>= JT4_WORK_MIN). 240 KiB: the sync search of B..G over 200..3000 Hz
 *                   takes 2 passes over the file (noise, CCF), JT4A 3; all sync candidates are
 *                   then extracted in 1 more pass. Less memory = more passes, same result.
 *   should_abort  : polled every few ms of CPU (may be 0); ~0.2 ms apart on x86, est. <10 ms on A9
 * returns number of decodes (>=0; if aborted, what was decoded so far) or JT4_ERR_*.
 */
int jt4_decode(const dsrc_t *src, int submode, float f_lo, float f_hi, float nominal_start_s,
               jt4_result_t *out, int maxout, void *work, uint32_t work_len, int (*should_abort)(void));

#endif
