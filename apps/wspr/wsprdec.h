/* SPDX-License-Identifier: GPL-3.0-or-later
 * wsprdec.h - WSPR-2 (2-minute WSPR) decoder for the ICAP digi apps.
 *
 * Detector / demodulator / decoder ported from WSJT-X wsprd (Copyright
 * 2001-2018 Joe Taylor K1JT, 2014-2018 Steven Franke K9AN, GPL-3.0): the
 * windowed-FFT candidate search, coarse/fine sync (sync_and_demodulate),
 * noncoherent sequence detection, symbol subtraction and the soft-decision
 * Fano decode of the K=32 r=1/2 convolutional code with WSJT-X's metric
 * tables.  This module is therefore GPL-3.0-or-later.
 *
 * Rewritten freestanding: no libc, no malloc, no libm, no FFTW.  The 8 kHz
 * capture is down-converted (mix at 1500 Hz) and resampled to the 375 sps
 * complex stream wsprd works on by a polyphase FIR instead of a giant FFT;
 * the candidate FFT is a radix-2 512-point transform.  All scratch memory is
 * taken from the caller's `work` buffer.
 */
#ifndef WSPRDEC_H
#define WSPRDEC_H
#include <stdint.h>

#ifndef DSRC_T_DEFINED
#define DSRC_T_DEFINED
/* Sample source: read(ctx, start, out, n) fills out[0..n) with samples
 * [start, start+n) (float, any scale) and returns the number of samples
 * written (>= 0), or < 0 on I/O error.  Samples past `len` may be left
 * unwritten; the reader zero-fills them. */
typedef struct {
    void *ctx;
    int (*read)(void *ctx, uint32_t start, float *out, uint32_t n);
    uint32_t len;   /* total samples available */
    uint32_t fs;    /* sample rate, Hz (8000 expected) */
} dsrc_t;
#endif

typedef struct {
    char  call[16];   /* callsign (or "<...>"/"<CALL>" for a hashed call) */
    char  grid[8];    /* 4- or 6-char Maidenhead locator, NUL terminated  */
    char  msg[24];    /* full "call grid power" string as wsprd prints it  */
    int   dbm;        /* power, dBm                                        */
    float snr_db;     /* S/N in 2500 Hz reference bandwidth (wsprd scale)  */
    float dt_s;       /* time offset of the signal, seconds (wsprd dt)     */
    float freq_hz;    /* audio frequency offset from 1500 Hz, Hz           */
    float drift_hz;   /* linear drift over the transmission, Hz            */
} wspr_result_t;

#define WSPR_NPOINTS    45000u            /* 375 sps complex points (120 s) */
#define WSPR_NSYM_D     162
#define WSPR_WORK_BYTES (216u * 1024u)    /* work-arena size (see .c)       */
#define WSPR_WORK_MIN   (212u * 1024u)    /* below this the call fails      */

#define WSPR_ERR_ABORT  (-1)
#define WSPR_ERR_ARG    (-2)
#define WSPR_ERR_IO     (-3)
#define WSPR_ERR_WORK   (-4)

/* Decode one 2-minute period from an 8 kHz real capture.
 *   src              8 kHz samples; src sample 0 = top of the even UTC minute
 *                    (the WSPR signal nominally starts at +1.0 s).
 *   f_lo, f_hi       audio search window as an OFFSET from 1500 Hz, Hz
 *                    (e.g. -150..+150 covers the whole WSPR sub-band).
 *   nominal_start_s  added to the stream origin; 0 for the normal convention.
 *   out/maxout       distinct decodes.
 *   work/work_len    scratch, >= WSPR_WORK_MIN (result is independent of size).
 *   should_abort     optional (may be NULL); polled during the work.
 * Returns the number of decodes (>= 0) or WSPR_ERR_*.  Cut off by should_abort: the decodes found so far (> 0),
 * or WSPR_ERR_ABORT when there are none.  The same for the I/Q entry points below. */
int wspr_decode(const dsrc_t *src, float f_lo, float f_hi, float nominal_start_s,
                wspr_result_t *out, int maxout, void *work, uint32_t work_len,
                int (*should_abort)(void));

/* Decode a float 375 sps complex stream (I = idat, Q = qdat, npoints <=
 * WSPR_NPOINTS), e.g. a WSJT-X .c2 file.  The samples are auto-scaled into the
 * s16 work arena; the caller's float arrays are left unchanged. */
int wspr_decode_iq(float *idat, float *qdat, int npoints,
                   wspr_result_t *out, int maxout, void *work, uint32_t work_len,
                   int (*should_abort)(void));

/* Decode an s16 375 sps complex stream the loader already produced (e.g. by
 * streaming the down-convert during capture -- the recommended IC-705 path,
 * which avoids re-reading the raw 8 kHz capture from SD).  Scale is irrelevant:
 * the decoder is scale-invariant, so any consistent I/Q scaling works. */
int wspr_decode_iq16(const int16_t *idat, const int16_t *qdat, int npoints,
                     wspr_result_t *out, int maxout, void *work, uint32_t work_len,
                     int (*should_abort)(void));

/* Decode s16 375-sps I/Q supplied by a callback that writes straight into the
 * decoder arena, so the caller needs NO second 180 KB I/Q buffer beside the
 * work arena (the on-chip region holds only one).  fill_iq(ctx, idst, qdst,
 * npoints) must fill idst[0..npoints) and qdst[0..npoints) (they point inside
 * the arena) and return the count written (>=0) or <0 on error; a short count
 * is zero-filled.  This is the streamed-capture path: the app down-converts the
 * 8 kHz audio to s16 I/Q during the 120 s capture, stores only the I/Q (e.g. a
 * 176 KB SD file), then streams it in here at decode time with no raw-8 kHz
 * re-read.  Same scale-invariant decode core as wspr_decode_iq16. */
int wspr_decode_iq16_cb(int (*fill_iq)(void *ctx, int16_t *idst, int16_t *qdst, int npoints),
                        void *ctx, int npoints, wspr_result_t *out, int maxout,
                        void *work, uint32_t work_len, int (*should_abort)(void));

/* fill_iq contract (the app relies on it): idst and qdst each have room for WSPR_NPOINTS s16 (not just npoints)
 * and nothing in the arena is live while the callback runs, so the callback may use the not yet filled tail of
 * qdst as its read buffer (wsprapp.c reads the interleaved file there and de-interleaves in place). */

/* Candidate search window of the I/Q entry points above, as an OFFSET from 1500 Hz (clamped to -150..+150; an
 * empty window selects the full one).  Default +-150 Hz; wsprd's own default is +-110 Hz.  Stays in force. */
void wspr_set_band(float f_lo, float f_hi);

/* ---- streaming 8 kHz -> 375 sps down-converter --------------------------------------------------------------
 * The same 1500 Hz mix and 3-phase polyphase FIR as wspr_decode()'s file path (fc 180 Hz, 384 taps per phase),
 * but fed the audio as it arrives, so an app stores only the s16 I/Q of a period (wspr_decode_iq16*).  Output
 * point m is centred on input sample 64*m/3; input sample 0 = stream origin (top of the even minute).
 * `gain` scales the stored I/Q (decode is scale-invariant; use it to keep very quiet audio above the s16 step);
 * values past s16 are clipped and counted in `clip`.  All state is in the caller's struct (~6.8 KB, no .bss). */
#define WSPR_DC_RING 512u                 /* input history, power of two, >= 384 + the largest push burst between emits */
typedef void (*wspr_dc_put_t)(void *ctx, int16_t i, int16_t q);
typedef struct {
    float    tap[3 * 384];
    float    cmix[16], smix[16];
    float    ring[WSPR_DC_RING];
    float    gain;
    uint32_t nin, mout, cap;              /* samples consumed, points produced, stop after `cap` points */
    uint32_t need, phase, i0;             /* next point: needs nin >= need; its FIR phase and centre sample */
    uint32_t clip;                        /* points clipped at s16 */
} wspr_dc_t;
void wspr_dc_init(wspr_dc_t *d);                                        /* build the tables (once per struct) */
void wspr_dc_reset(wspr_dc_t *d, float gain, uint32_t maxpoints);       /* start a new period */
/* feed n samples (x == NULL: n zero samples); put() is called once per completed I/Q point */
void wspr_dc_push(wspr_dc_t *d, const int16_t *x, uint32_t n, wspr_dc_put_t put, void *ctx);

/* Peak bytes carved from the caller's work buffer by the last decode (0 before
 * any decode).  For reporting the resident footprint. */
uint32_t wspr_work_peak(void);

#endif /* WSPRDEC_H */
