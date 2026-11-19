/* SPDX-License-Identifier: MIT
 * ft8dec.h - FT8 decoder for the IC-705 ICAP digi app.  Freestanding C, no malloc, no libc.
 * Built on kgoba/ft8_lib (MIT, vendored unmodified in ft8/, see ft8/UPSTREAM): its tables, CRC, message
 * (un)packing and encoder, plus adapted copies of its sync search and BP decoder; the waterfall front end, SD
 * streaming, signal subtraction, OSD, the SNR/DT/frequency estimates and the callsign hash table are here.
 */
#ifndef FT8DEC_H
#define FT8DEC_H
#include <stdint.h>
#include "digi_util.h"   /* dsrc_t */

typedef struct {
    char  msg[36];       /* decoded text (WSJT-X style), NUL terminated (<= 35 chars) */
    float freq_hz;       /* audio frequency of tone 0 (WSJT-X "Freq") */
    float dt_s;          /* signal start minus nominal_start_s (WSJT-X "DT": TX starts 0.5 s into the period) */
    float snr_db;        /* SNR in 2500 Hz reference bandwidth, clipped at -24 like WSJT-X */
    int   quality;       /* sync score (ft8_lib units, >= FT8_MIN_SCORE) */
} ft8_result_t;

/* Callsign hash table, kept by the caller across periods (hashed calls "<...>" resolve once the full call
 * has been decoded).  Zero-filled memory is a valid empty table. */
#define FT8_HASH_SLOTS 256
typedef struct {
    struct { char call[12]; uint32_t h22; uint16_t age; uint16_t used; } e[FT8_HASH_SLOTS];
    uint32_t n;
} ft8_state_t;

#define FT8_WORK_MIN   (96u * 1024u)    /* below this the call fails; ~232 KB = one pass over 200..3000 Hz */
#define FT8_ERR_ARGS   (-1)
#define FT8_ERR_ABORT  (-2)             /* aborted before anything was decoded */
#define FT8_ERR_IO     (-3)

/* Decode one 15 s FT8 period.
 *   src            : audio (fs 6000..12000 Hz with fs*0.16 an integer, 8000 expected), read only via src->read,
 *                    front to back once per pass (1 + up to 2 subtraction passes; times the band passes when
 *                    the range does not fit the work area)
 *   f_lo, f_hi     : search range for the tone-0 frequency (Hz); clipped so tone 7 stays below fs/2
 *   nominal_start_s: position in the buffer where a DT = 0 signal starts (period start + 0.5 s);
 *                    DT is searched about -2.2 .. +2.5 s around it
 *   out/maxout     : results, one per distinct message (no duplicates within the call)
 *   work/work_len  : scratch, 8-byte aligned (>= FT8_WORK_MIN)
 *   should_abort   : polled every few ms of CPU (may be 0)
 *   st             : callsign hash table kept across calls (may be 0: hashed calls print as <...>)
 * returns number of decodes (>=0; if aborted, what was decoded so far) or FT8_ERR_*.
 */
int ft8_decode(const dsrc_t *src, float f_lo, float f_hi, float nominal_start_s,
               ft8_result_t *out, int maxout, void *work, uint32_t work_len, int (*should_abort)(void),
               ft8_state_t *st);

/* bytes of `work` used by the last ft8_decode call (high-water mark) */
uint32_t ft8_work_peak(void);

#endif
