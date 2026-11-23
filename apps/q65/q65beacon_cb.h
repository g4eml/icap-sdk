/* SPDX-License-Identifier: GPL-3.0-or-later
 * C view of the q65beacon no_std callback ABI (Rust src/ffi_cb.rs,
 * src/radio_rt.rs).  The ICAP C shim backs the callbacks with the
 * radio's SD file system and hands the decoder heap over the granted
 * ABI v2 region.
 */
#ifndef Q65BEACON_CB_H
#define Q65BEACON_CB_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    float    freq_hz;      /* tone-0 (sync) frequency, Hz                */
    float    dt_s;         /* frame-start offset vs nominal, seconds     */
    float    snr_db;       /* WSJT-X 2500 Hz-reference SNR               */
    uint32_t iterations;   /* QRA BP iterations (blind)                  */
    float    llh;          /* template log-likelihood (template)        */
    int32_t  ok;           /* 1 = decoded                               */
    char     text[32];     /* NUL-terminated message                    */
} q65b_result_t;

/* SD-backed callbacks.  ctx is passed back to each unchanged. */
typedef struct {
    void   *ctx;
    size_t (*pcm_len)(void *ctx);
    size_t (*pcm_read)(void *ctx, size_t start, float *out, size_t n);
    void   (*spec_begin)(void *ctx, size_t ncols);
    void   (*spec_append)(void *ctx, const float *row, size_t ncols);
    void   (*spec_read)(void *ctx, size_t idx, float *out, size_t ncols);
    size_t (*spec_nrows)(void *ctx);
    size_t (*spec_ncols)(void *ctx);
    void   (*log_line)(void *ctx, const char *s, size_t len);
    int    (*should_abort)(void *ctx);   /* non-zero => STOP, decode returns cleanly */
} q65b_cb_t;

/* Initialise the decoder heap over the ABI v2 region slice [base,base+size). */
void q65b_heap_init(void *base, size_t size);

/* Decode one captured T/R period.  period_s: 15, 30 or 60; submode: 0..4 = A..E
 * (at 8 kHz all but 15D/15E; an unsupported pair returns 1, logged "Q65B ABORT where=params").
 * candidates/ncand: must be NULL/0 (blind BP; this radio build has no template mode, else -1).
 * sample_rate: Hz (0->12000).  returns 0 = decoded, 1 = no decode, <0 = bad argument. */
int32_t q65b_decode_cb(const q65b_cb_t *cb,
                       uint32_t period_s, uint32_t submode,
                       uint32_t nominal_start_sample,
                       uint32_t sample_rate,
                       float fmin_hz, float fmax_hz,
                       const int32_t *candidates, uint32_t ncand,
                       q65b_result_t *out);

#endif
