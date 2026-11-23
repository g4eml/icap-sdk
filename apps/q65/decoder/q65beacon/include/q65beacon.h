/* SPDX-License-Identifier: GPL-3.0-or-later
 * q65beacon — C ABI for the IC-9700 ICAP app's C shim.
 *
 * The C shim owns audio capture, RTC/UTC alignment, SD I/O and display.
 * It writes the captured minute as raw little-endian f32 mono-12kHz PCM
 * to the SD card, then calls q65b_decode().  Milestone-1 host/ARM form
 * takes SD file paths; an on-radio callback form can replace the file
 * backing with the radio's own FS (see q65beacon::source traits).
 *
 * IMPORTANT (measured milestone-1): a faithful Q65-60D EME decode needs
 * ~336 KB of heap working RAM (blind BP) or ~198 KB (template).  This
 * does NOT fit the ~48 KB ICAP resident window; the loader/ABI must grant
 * a working-RAM region (recommend >=384 KB) from the radio's ~10 MiB.
 * Static RAM (.data+.bss) of the decoder itself is ~0 (QRA tables are in
 * flash/.rodata).
 */
#ifndef Q65BEACON_H
#define Q65BEACON_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float    freq_hz;     /* tone-0 (sync) frequency, Hz                 */
    float    dt_s;        /* frame-start offset vs nominal, seconds      */
    float    snr_db;      /* WSJT-X 2500 Hz-reference SNR estimate       */
    uint32_t iterations;  /* QRA BP iterations (blind); 0 for template   */
    float    llh;         /* template log-likelihood; 0 for blind        */
    int32_t  ok;          /* 1 = decoded, 0 = no decode                  */
    char     text[32];    /* NUL-terminated decoded message              */
} q65b_result_t;

/* Decode one captured T/R period of Q65 audio (period_s 15/30/60, submode 0..4 = A..E).
 *
 *  pcm_path       SD path to raw LE-f32 mono-12kHz capture of the minute
 *  spectra_path   SD scratch path for the streamed narrow sync spectra
 *  log_path       SD debug-log path, or NULL to disable (see format below)
 *  nominal_start_sample  T/R-boundary sample index inside the capture
 *  fmin_hz,fmax_hz       frequency search window (e.g. 900..1100 beacon)
 *  candidates     flattened ncand*63 int32 template codewords, or NULL
 *                 for blind belief-propagation decode
 *  ncand          number of template codewords (0 for blind)
 *  period_s       T/R period: 15, 30 or 60 s
 *  submode        0..4 = A..E (not 15D/15E at 8 kHz)
 *  out            result (must be non-NULL)
 *
 * returns 0 = success & decoded, 1 = ran but no decode, <0 = error
 *         (-1 bad out, -2 bad path arg, -3 open pcm failed, -4 spectra).
 */
int32_t q65b_decode(const char   *pcm_path,
                    const char   *spectra_path,
                    const char   *log_path,
                    uint32_t      nominal_start_sample,
                    float         fmin_hz,
                    float         fmax_hz,
                    const int32_t *candidates,
                    uint32_t      ncand,
                    uint32_t      period_s,
                    uint32_t      submode,
                    q65b_result_t *out);

/* ---------------------------------------------------------------------
 * Debug-log line format (greppable; one line per stage), written to
 * log_path on the SD card so a failed on-air decode is diagnosable:
 *
 *   Q65B PARAMS  period= sub= fs= nsps= bpt= nom_start= fmin= fmax=
 *   Q65B SPECTRA rows= cols= row_min= row_max_start= noise=
 *   Q65B CAND    i= bin= freq= start= syncpwr= score=      (per candidate)
 *   Q65B ENERGY  bin= wide_bins=
 *   Q65B RESULT  ibw= b90= freq= dt_s= snr_db= iters= text=...
 *   Q65B DECODE  ok=0 reason=...        (b90 sweep exhausted for a cand)
 *   Q65B ABORT   where=... reason=...   (any early failure path)
 * ------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif
#endif /* Q65BEACON_H */
