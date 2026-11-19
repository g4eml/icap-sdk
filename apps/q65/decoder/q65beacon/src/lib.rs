// SPDX-License-Identifier: GPL-3.0-or-later
//! # q65beacon — low-RAM segmented Q65-60D decoder
//!
//! Milestone-1 port of the WSJT-X Q65-60D decode path to run inside the
//! IC-9700 ICAP SD app (~48 KB resident window, SD card as scratch).
//!
//! The reference path in `mfsk-core` builds a whole-band spectrogram
//! (819 × 3600 × 4 B = 11.8 MB, cloned once for the noise floor ⇒ a
//! ~26 MB peak) and runs an `nsps`-point (7200) complex FFT per symbol
//! (a 57.6 KB resident buffer on its own). Neither fits the radio.
//!
//! This crate replaces that front end with a **streaming one-window FFT** ([`fft`]): the bulk PCM
//! lives on the SD card and is read strictly forward in small blocks ([`SampleSource`]), one
//! `nsps`-point real FFT per spectrogram row / symbol, and only what the next stage needs is kept:
//!
//! 1. **sync pass** - every 1/8-symbol row is folded at once into the Costas-comb accumulator
//!    (`n_starts x nbins`), the only large resident array;
//! 2. **extraction pass(es)** - one for the strongest sync candidate, one more for all the others:
//!    each data-symbol window gives the wide energy row, reduced at once to the fast-fading metric of
//!    every b90 step (the 157 KiB wide spectrogram is never stored).  The first candidate of a pass
//!    stays in RAM, the others are spilled to the SD-backed scratch ([`SpectraStore`]);
//! 3. **b90 sweep** - QRA belief propagation ([`bp`]) straight on the metric blocks;
//! 4. **SNR pass** - only after a decode.
//!
//! All working memory is ONE arena re-carved per phase (see [`decode`]), ~210 KB at 8 kHz.
//! (The first version used a Goertzel resonator per bin and re-read the wide spectrogram from the
//! card for every b90 step: ~55 s per minute on the IC-705's Cortex-A9 instead of ~2.5 s.)
//!
//! The **FEC + message layer** is `mfsk-core`'s (GPL-3.0): the QRA(15,65) code tables, the Q65
//! fast-fading metric tables, CRC-12 and `Q65Message` unpack; the belief propagation itself is the
//! same algorithm re-laid-out in [`bp`] (bit-identical results, NEON kernels, 94 KB instead of
//! 160 KB of scratch).
//!
//! Two decode modes:
//! - [`Mode::Blind`] — full QRA belief-propagation decode (any message).
//! - [`Mode::Template`] — BP-free `check_codeword_llh` against a precomputed candidate codeword
//!   list (a beacon's known message(s)).

#![cfg_attr(not(feature = "std"), no_std)]
#![cfg_attr(feature = "radio", feature(optimize_attribute))]
#![cfg_attr(target_arch = "arm", feature(stdarch_arm_neon_intrinsics, arm_target_feature))]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;

use mfsk_core::engine::{DecodeContext, MessageCodec, ModulationParams};
use mfsk_core::fec::qra::{lorentz_taps, pdmath};
use mfsk_core::msg::Q65Message;
use mfsk_core::msg::q65::unpack_symbols_to_bits77;
use mfsk_core::q65::Q65d60;
use mfsk_core::q65::sync_pattern::Q65_SYNC_POSITIONS;

use num_traits::Float;

// Standalone-link stubs for the bare no_std staticlib (size/compile
// proof only). The radio's C shim/runtime provides the real allocator
// and panic handler; never enable `bare` in that integration.
#[cfg(all(not(feature = "std"), feature = "bare"))]
mod bare {
    use core::alloc::{GlobalAlloc, Layout};
    struct StubAlloc;
    unsafe impl GlobalAlloc for StubAlloc {
        unsafe fn alloc(&self, _l: Layout) -> *mut u8 {
            core::ptr::null_mut()
        }
        unsafe fn dealloc(&self, _p: *mut u8, _l: Layout) {}
    }
    #[global_allocator]
    static A: StubAlloc = StubAlloc;
    #[panic_handler]
    fn ph(_: &core::panic::PanicInfo) -> ! {
        loop {}
    }
}

pub mod bp;
pub mod debuglog;
pub mod fft;
pub mod goertzel;
pub mod source;

#[cfg(feature = "std")]
pub mod ffi;

#[cfg(feature = "radio")]
pub mod ffi_cb;
#[cfg(feature = "radio")]
pub mod radio_rt;

pub use debuglog::{DebugLog, NullLog, StringLog};
pub use source::{SampleSource, SpectraStore};

/// Re-export so binaries linking this crate can reach the reused
/// FEC/message layer (e.g. to precompute template codewords).
pub use mfsk_core;

use bp::Bp;
use fft::RealFft;

const FS_REF: u32 = 12_000;
const N_SYMBOLS: usize = 85;
const N_DATA: usize = 63;
const N_TONES: usize = 64;
const M: usize = 64; // GF(64) alphabet size

// Q65-60D (tone-spacing letter D ⇒ bins_per_tone=8 ⇒ submode index 3).
// The automatic ("Fast") decode depth in `q65::rx` always runs the
// fast-fading (Lorentzian) metric over a submode-specific b90 sweep —
// q65_decode.f90:168-178 gives ibwa=8 for C/D/E, ibwb=min(15,ibwa+6).
const SUBMODE_IDX: u8 = 3;
const IBWA: i32 = 8;
const IBWB: i32 = 14;

/// Search / alignment parameters for a Q65-60D beacon decode.
#[derive(Clone, Debug)]
pub struct Params {
    pub sample_rate: u32,
    /// Nominal frame-start sample (the T/R boundary).
    pub nominal_start_sample: usize,
    pub freq_min_hz: f32,
    pub freq_max_hz: f32,
    pub time_early_s: f32,
    pub time_late_s: f32,
    /// How many top sync candidates to try decoding (first success wins).
    pub max_candidates: usize,
}

impl Default for Params {
    fn default() -> Self {
        // Beacon defaults: narrow window around a ~997 Hz tone, the
        // asymmetric WSJT-X 60-s T/R search window.
        Self {
            sample_rate: FS_REF,
            nominal_start_sample: 0,
            freq_min_hz: 900.0,
            freq_max_hz: 1100.0,
            time_early_s: 1.0,
            time_late_s: 4.0,  // ~+4 s launch-jitter tolerance (bounded)
            max_candidates: 4,
        }
    }
}

/// Decode strategy.
pub enum Mode<'a> {
    /// Full QRA belief-propagation decode (needs ~145 KB BP scratch).
    Blind,
    /// Template match against known candidate codewords (no BP scratch).
    Template(&'a [[i32; N_DATA]]),
}

/// A successful decode.
#[derive(Clone, Debug)]
pub struct Decoded {
    pub text: String,
    pub freq_hz: f32,
    pub dt_s: f32,
    pub snr_db: f32,
    /// BP iterations (blind) or 0 (template).
    pub iterations: u32,
    /// Template log-likelihood (template) or 0 (blind).
    pub llh: f32,
}

/// Geometry derived once from the sub-mode + sample rate.
struct Geom {
    nsps: usize,
    df: f32,
    bpt: usize,
    t_step: usize,
    rows_per_sym: usize,
}

impl Geom {
    fn new(sample_rate: u32) -> Self {
        let nsps = (sample_rate as f32 * <Q65d60 as ModulationParams>::SYMBOL_DT).round() as usize;
        let df = sample_rate as f32 / nsps as f32;
        let bpt = (<Q65d60 as ModulationParams>::TONE_SPACING_HZ / df).round() as usize;
        let t_step = (nsps / 8).max(1);
        let rows_per_sym = (nsps / t_step).max(1);
        Self { nsps, df, bpt, t_step, rows_per_sym }
    }
}

/// Es/No metric, identical to `q65::rx::default_es_no_metric`.
fn default_es_no_metric() -> f32 {
    let eb_no = 1.9054607_f32; // 10^0.28 (was 10.0_f32.powf(2.8/10.0)); drops libm powf
    6.0 * (15.0 / 65.0) * eb_no
}

fn q65_bw_offset_db() -> f32 {
    10.0 * (2500.0 / <Q65d60 as ModulationParams>::TONE_SPACING_HZ).log10()
}

/// `snr_db_from_sig_noi`, ported from `q65::rx`.
fn snr_db_from_sig_noi(xsig: f32, xnoi: f32, bw_offset_db: f32) -> f32 {
    const FLOOR: f32 = -24.0;
    const CEIL: f32 = 49.0;
    if xnoi < f32::EPSILON {
        return if xsig < f32::EPSILON { FLOOR } else { CEIL };
    }
    let ratio = xsig / xnoi - 1.0;
    if ratio <= 0.001 {
        return FLOOR;
    }
    (10.0 * ratio.log10() - bw_offset_db).clamp(FLOOR, CEIL)
}

/// Symbol-window front end: the FFT and the s16 window it transforms (the PCM is s16 on the card;
/// keeping the window as s16 halves its size).  All of it lives in caller memory (the arena).
struct Front<'a> {
    fft: RealFft<'a>,
    win: &'a mut [i16],
    pos: usize,
    have: bool,
}

#[inline]
fn f2s16(v: f32) -> i16 {
    let x = v * 32768.0;
    let r = if x >= 0.0 { x + 0.5 } else { x - 0.5 };
    (r as i32).clamp(-32768, 32767) as i16
}

/// Read samples `[start, start + out.len())` as s16, through the f32 scratch `tmp`.
fn read_i16<S: SampleSource>(src: &S, start: usize, out: &mut [i16], tmp: &mut [f32]) {
    let mut off = 0;
    while off < out.len() {
        let n = (out.len() - off).min(tmp.len());
        src.read_block(start + off, &mut tmp[..n]);
        for i in 0..n {
            out[off + i] = f2s16(tmp[i]);
        }
        off += n;
    }
}

impl<'a> Front<'a> {
    /// f32 units of memory the front end needs for `nsps`-sample windows.
    fn mem_len(nsps: usize) -> usize {
        RealFft::mem_len(nsps) + nsps / 2
    }
    #[inline(never)]
    fn new_in(nsps: usize, mem: &'a mut [f32]) -> Option<Self> {
        if nsps % 2 != 0 || mem.len() < Self::mem_len(nsps) {
            return None;
        }
        let (w, rest) = mem.split_at_mut(nsps / 2);
        // SAFETY: nsps/2 f32 hold nsps i16; f32 alignment satisfies i16; the region is exclusively ours.
        let win = unsafe { core::slice::from_raw_parts_mut(w.as_mut_ptr() as *mut i16, nsps) };
        let fft = RealFft::new_in(nsps, rest)?;
        Some(Self { fft, win, pos: 0, have: false })
    }
    /// Transform the `nsps` window at `start`.  Successive calls must not go backwards; a window
    /// that overlaps the previous one only reads the new samples (the card is read once, forward).
    #[inline(never)]
    fn load<S: SampleSource>(&mut self, src: &S, start: usize) {
        let n = self.win.len();
        if self.have && start >= self.pos && start < self.pos + n {
            let d = start - self.pos;
            if d > 0 {
                self.win.copy_within(d.., 0);
                read_i16(src, self.pos + n, &mut self.win[n - d..], self.fft.scratch());
            }
        } else {
            read_i16(src, start, self.win, self.fft.scratch());
        }
        self.pos = start;
        self.have = true;
        self.fft.forward(self.win);
    }
}

/// `dst[i] += src[i]` (element-wise: NEON and scalar give the same bits).
#[cfg(target_arch = "arm")]
#[target_feature(enable = "neon")]
unsafe fn add_rows_neon(dst: &mut [f32], src: &[f32]) {
    use core::arch::arm::*;
    let n = dst.len().min(src.len());
    let (d, s) = (dst.as_mut_ptr(), src.as_ptr());
    let mut i = 0usize;
    while i + 4 <= n {
        vst1q_f32(d.add(i), vaddq_f32(vld1q_f32(d.add(i)), vld1q_f32(s.add(i))));
        i += 4;
    }
    while i < n {
        *d.add(i) += *s.add(i);
        i += 1;
    }
}
#[cfg_attr(feature = "radio", optimize(speed))]
#[inline]
fn add_rows(dst: &mut [f32], src: &[f32]) {
    #[cfg(target_arch = "arm")]
    unsafe {
        add_rows_neon(dst, src);
    }
    #[cfg(not(target_arch = "arm"))]
    for (d, &s) in dst.iter_mut().zip(src.iter()) {
        *d += s;
    }
}

/// WSJT-X's displayed Q65 SNR (`q65_snr`, q65.f90:744-793; stock mfsk-core `q65::snr::q65_snr_db`:
/// composite tone-aligned power spectrum over all 85 symbols, two guard bands of `nsum` bins as the
/// baseline, integrated excess power - 10*log10(2500/df), no -24 dB floor) and the old
/// `snr_db_wide` (the fallback, logged as `snr_wide=`), both from ONE forward pass over the frame:
/// one FFT per symbol.  Returns `(snr, snr_wide)`; `snr == snr_wide` when the q65 window does not fit.
fn snr_pass<S: SampleSource>(
    src: &S,
    g: &Geom,
    mem: &mut [f32],
    start_sample: usize,
    base_bin: usize,
    codeword: &[i32; N_DATA],
) -> Option<(f32, f32)> {
    let nsps = g.nsps;
    let df = g.df;
    let n_freq = (nsps / 2) as i64;
    let mode_q65 = ((<Q65d60 as ModulationParams>::TONE_SPACING_HZ / df).round() as i64).max(1);
    let nsum = (10 * mode_q65).max((50.0 / df).round() as i64).max(1);
    let bb = base_bin as i64;
    let ia = (bb - 2 * nsum).max(0);
    let ib = (bb + 2 * nsum).min(n_freq - 1);
    let q65_ok = ib - ia >= 2 * nsum;
    let width = if q65_ok { (ib - ia + 1) as usize } else { 0 };
    let (fmem, rest) = mem.split_at_mut(Front::mem_len(nsps));
    let mut fe = Front::new_in(nsps, fmem)?;
    let (spec, rest) = rest.split_at_mut(width);
    let pw = &mut rest[..width];
    for v in spec.iter_mut() {
        *v = 0.0;
    }
    let central = base_bin + g.bpt;
    let (mut xsig, mut xnoi) = (0.0f32, 0.0f32);
    let mut sync_i = 0usize;
    let mut data_k = 0usize;
    for sym in 0..N_SYMBOLS {
        let is_sync = sync_i < Q65_SYNC_POSITIONS.len() && Q65_SYNC_POSITIONS[sync_i] as usize == sym;
        fe.load(src, start_sample + sym * nsps);
        let itone: i64 = if is_sync {
            sync_i += 1;
            0
        } else {
            let t = codeword[data_k] as usize;
            data_k += 1;
            if t < N_TONES {
                // snr_db_wide: the decoded tone against the mean of the other 63 tone centres
                let mut sig = 0.0f32;
                let mut total = 0.0f32;
                for tone in 0..N_TONES {
                    let v = fe.fft.power(central + tone * g.bpt);
                    total += v;
                    if tone == t {
                        sig = v;
                    }
                }
                xsig += sig;
                xnoi += (total - sig) / 63.0;
            }
            t as i64 + 1
        };
        if q65_ok {
            let shift = mode_q65 * itone;
            // bins ii = i + shift for i in [ia, ib] that lie below Nyquist (stock skips the others)
            let lo = ia + shift;
            let hi = (ib + shift).min(n_freq - 1);
            if hi >= lo {
                let n = (hi - lo + 1) as usize;
                fe.fft.powers(lo as usize, &mut pw[..n]);
                for idx in 0..n {
                    spec[idx] += pw[idx];
                }
            }
        }
    }
    let snr_wide = snr_db_from_sig_noi(xsig, xnoi, q65_bw_offset_db());
    if !q65_ok {
        return Some((snr_wide, snr_wide));
    }
    let ns = nsum as usize;
    let sum1: f32 = spec[..ns].iter().sum();
    let sum2: f32 = spec[width - ns..].iter().sum();
    let avg = (sum1 + sum2) / (2.0 * ns as f32);
    if avg.is_nan() || avg <= 0.0 {
        return Some((snr_wide, snr_wide));
    }
    let sig_area: f32 = spec[ns..width - ns].iter().map(|&x| x / avg - 1.0).sum();
    Some((10.0 * sig_area.max(1.0).log10() - 10.0 * (2500.0 / df).log10(), snr_wide))
}

/// A sync candidate.
#[derive(Clone, Copy)]
struct Cand {
    base_bin: usize,
    freq_hz: f32,
    start_sample: usize,
    syncpwr: f32,
    score: f32,
}

/// Geometry of the sync search (rows, bins, start range).
struct SyncGeom {
    fmin_bin: usize,
    nbins: usize,
    row_min: usize,
    row_max_start: usize,
    last_needed: usize,
    n_starts: usize,
}

fn sync_geom<L: DebugLog>(total: usize, g: &Geom, p: &Params, log: &mut L) -> Option<SyncGeom> {
    if total < g.nsps {
        log.line("Q65B ABORT where=spectra reason=audio_shorter_than_one_symbol");
        return None;
    }
    let n_time_total = (total - g.nsps) / g.t_step + 1;

    let fmin_bin = (p.freq_min_hz / g.df).floor().max(0.0) as usize;
    let fmax_bin = (p.freq_max_hz / g.df).ceil() as usize;
    let fmax_bin = fmax_bin.min((g.nsps / 2).saturating_sub(64 * g.bpt + 1));
    if fmax_bin < fmin_bin {
        log.line("Q65B ABORT where=spectra reason=freq_window_empty_after_headroom");
        return None;
    }
    let nbins = fmax_bin - fmin_bin + 1;

    let rows_per_sec = p.sample_rate as f32 / g.t_step as f32;
    let early_rows = (p.time_early_s.max(0.0) * rows_per_sec).round() as i64;
    let late_rows = (p.time_late_s.max(0.0) * rows_per_sec).round() as i64;
    let nominal_row = (p.nominal_start_sample / g.t_step) as i64;
    let row_min = (nominal_row - early_rows).max(0) as usize;
    let last_sync_offset = (*Q65_SYNC_POSITIONS.last().unwrap() as usize) * g.rows_per_sym;
    let max_start_by_frame = n_time_total.saturating_sub(84 * g.rows_per_sym + 1);
    let row_max_start = ((nominal_row + late_rows) as usize).min(max_start_by_frame);
    if row_max_start < row_min {
        log.line("Q65B ABORT where=spectra reason=time_window_empty");
        return None;
    }
    // Cap the start-row search so the comb accumulator (n_starts * nbins f32) always fits the
    // region heap, regardless of how long the capture is or how wide the frequency window:
    // 36k floats = 144 KiB.  A wider window trades time-search depth for frequency coverage.
    let max_starts = (36_000usize / nbins.max(1)).max(1);
    let row_max_start = row_max_start.min(row_min + max_starts - 1);
    let last_needed = (row_max_start + last_sync_offset).min(n_time_total - 1);
    let n_starts = row_max_start - row_min + 1;
    Some(SyncGeom { fmin_bin, nbins, row_min, row_max_start, last_needed, n_starts })
}

/// Streaming sync scan: ONE forward pass over the PCM.
///
/// Every narrow tone-0 spectrogram row (window hop = nsps/8) is one FFT of the sliding symbol
/// window; it is folded at once into the comb accumulator
/// `comb[si*nbins + b] = sum_pos row_{s+pos*rows_per_sym}[b]`, so the resident state is `comb`
/// (`n_starts x nbins`) + the FFT.  The candidate list is the same 22-position Costas-comb search as
/// before (same rows, same sums in the same order); only the way a row is computed changed
/// (FFT instead of one Goertzel resonator per bin).  `mem`: front end + row + comb.
fn scan_sync<S: SampleSource, L: DebugLog>(
    src: &S,
    g: &Geom,
    p: &Params,
    sg: &SyncGeom,
    mem: &mut [f32],
    log: &mut L,
) -> Option<Vec<Cand>> {
    let (fmin_bin, nbins, row_min, row_max_start) = (sg.fmin_bin, sg.nbins, sg.row_min, sg.row_max_start);
    let (fmem, rest) = mem.split_at_mut(Front::mem_len(g.nsps));
    let mut fe = Front::new_in(g.nsps, fmem)?;
    let (row, rest) = rest.split_at_mut(nbins);
    let comb = &mut rest[..sg.n_starts * nbins];
    for v in comb.iter_mut() {
        *v = 0.0;
    }
    let mut noise_sum = 0.0f64;
    let mut noise_cnt = 0u64;
    let rps = g.rows_per_sym;
    let mut nrows_built = 0usize;
    for r in row_min..=sg.last_needed {
        fe.load(src, r * g.t_step);
        fe.fft.powers(fmin_bin, row);
        for &v in row.iter() {
            noise_sum += v as f64;
            noise_cnt += 1;
        }
        nrows_built += 1;
        if log.aborted() {
            log.line("Q65B ABORT where=sync reason=stop");
            return None;
        }
        for &pos in Q65_SYNC_POSITIONS.iter() {
            let off = pos as usize * rps;
            if r < row_min + off {
                continue;
            }
            let s = r - off;
            if s < row_min || s > row_max_start {
                continue;
            }
            let base = (s - row_min) * nbins;
            add_rows(&mut comb[base..base + nbins], row);
        }
    }
    let noise_floor = if noise_cnt > 0 {
        (noise_sum / noise_cnt as f64) as f32
    } else {
        1.0
    };

    {
        let mut msg = String::new();
        fmt_line(&mut msg, "Q65B SPECTRA", &[
            ("rows", Num::U(nrows_built as u64)),
            ("cols", Num::U(nbins as u64)),
            ("row_min", Num::U(row_min as u64)),
            ("row_max_start", Num::U(row_max_start as u64)),
            ("noise", Num::F(noise_floor)),
        ]);
        log.line(&msg);
    }

    // Best (start, power) per base bin over all candidate starts.  `row` is reused as best_pwr and
    // the (now idle) FFT work area holds the best start index per bin.
    let best_pwr = row;
    for v in best_pwr.iter_mut() {
        *v = 0.0;
    }
    let best_si = &mut fe.fft.scratch()[..nbins];
    for v in best_si.iter_mut() {
        *v = 0.0;
    }
    for si in 0..sg.n_starts {
        let base = si * nbins;
        for b in 0..nbins {
            let v = comb[base + b];
            if v > best_pwr[b] {
                best_pwr[b] = v;
                best_si[b] = si as f32;
            }
        }
    }

    // Local-max over base bins (neighbourhood = bpt), sort desc, top-N.
    let mut cands: Vec<Cand> = Vec::with_capacity(nbins / (g.bpt + 1) + 2);
    for b in 0..nbins {
        let pwr = best_pwr[b];
        if pwr <= 0.0 {
            continue;
        }
        let lo = b.saturating_sub(g.bpt);
        let hi = (b + g.bpt).min(nbins - 1);
        if !best_pwr[lo..=hi].iter().all(|&o| o <= pwr) {
            continue;
        }
        let s = row_min + best_si[b] as usize;
        let denom = pwr + noise_floor * Q65_SYNC_POSITIONS.len() as f32;
        cands.push(Cand {
            base_bin: fmin_bin + b,
            freq_hz: (fmin_bin + b) as f32 * g.df,
            start_sample: s * g.t_step,
            syncpwr: pwr,
            score: if denom > 0.0 { pwr / denom } else { 0.0 },
        });
    }
    // Partial selection of the top-N by sync power (avoids pulling the generic merge sort into the
    // app window).
    let topn = p.max_candidates.max(1);
    let n = cands.len();
    for i in 0..topn.min(n) {
        let mut m = i;
        for j in (i + 1)..n {
            if cands[j].syncpwr > cands[m].syncpwr {
                m = j;
            }
        }
        cands.swap(i, m);
    }
    cands.truncate(topn);
    cands.shrink_to_fit();

    for (i, c) in cands.iter().enumerate() {
        let mut msg = String::new();
        fmt_line(&mut msg, "Q65B CAND", &[
            ("i", Num::U(i as u64)),
            ("bin", Num::U(c.base_bin as u64)),
            ("freq", Num::F(c.freq_hz)),
            ("start", Num::U(c.start_sample as u64)),
            ("syncpwr", Num::F(c.syncpwr)),
            ("score", Num::F(c.score)),
        ]);
        log.line(&msg);
    }
    Some(cands)
}

/// One step of the b90 sweep: the fast-fading (Lorentzian) weights `t/(1+t)` for that spread and the
/// degraded Es/No metric.  The `1/noise_var` factor of `fast_fading_row_ctx` is applied when the
/// mean energy is known (after the extraction pass).
struct Fade {
    ibw: i32,
    b90: f32,
    es_no_metric: f32,
    w: Vec<f32>,
}

/// `fast_fading_row`'s weighted window sums for one wide row, without the `1/noise_var` factor.
#[cfg_attr(feature = "radio", optimize(speed))]
fn metric_row(w: &[f32], row: &[f32], bpt: usize, out: &mut [f32]) {
    let hhsz = w.len() - 1;
    let hlast = 2 * hhsz;
    let first_bin = M - hhsz;
    for (k, o) in out.iter_mut().enumerate() {
        let centre = first_bin + k * bpt;
        let win = &row[centre..centre + hlast + 1];
        let mut acc = 0.0f32;
        for j in 0..hhsz {
            acc += w[j] * (win[j] + win[hlast - j]);
        }
        acc += w[hhsz] * win[hhsz];
        *o = acc;
    }
}

/// The second half of `fast_fading_row`: metric (divided by the noise variance here) -> normalised
/// symbol probabilities, in place.
fn metric_to_prob(r: &mut [f32], noise_var: f32) {
    let mut max_logp = 0.0f32;
    for v in r.iter_mut() {
        *v /= noise_var;
        if *v > max_logp {
            max_logp = *v;
        }
    }
    let mut sum = 0.0f32;
    for v in r.iter_mut() {
        let x = (*v - max_logp).clamp(-85.0, 85.0);
        let e = x.exp();
        *v = e;
        sum += e;
    }
    let inv = if sum > 0.0 { 1.0 / sum } else { 0.0 };
    for v in r.iter_mut() {
        *v *= inv;
    }
    pdmath::norm(r);
}

/// Spilled metric records per SD row (4 x 7 x 64 f32 = 7 KiB per write / read).
const RPR: usize = 4;

/// f32 per b90 step of a candidate's metric block: the 65 x 64 intrinsics layout the BP decoder
/// reads in place (row `bp::intr_row(k)` = data symbol k).
const BLOCK: usize = bp::INTR_ROWS * M;

/// The frame-relative symbol index of data symbol `k` (0..63).
fn data_pos() -> [u8; N_DATA] {
    let mut out = [0u8; N_DATA];
    let (mut k, mut si) = (0usize, 0usize);
    for sym in 0..N_SYMBOLS {
        if si < Q65_SYNC_POSITIONS.len() && Q65_SYNC_POSITIONS[si] as usize == sym {
            si += 1;
        } else {
            out[k] = sym as u8;
            k += 1;
        }
    }
    out
}

/// The data-symbol windows of a group of candidates in ascending start order (ties: lower candidate
/// first) - the order one forward pass over the PCM visits them, and the order the spilled metric
/// records are stored in.
struct Merge<'c> {
    starts: &'c [Option<usize>],
    dpos: [u8; N_DATA],
    next: [u8; MAX_GROUP],
    nsps: usize,
}
const MAX_GROUP: usize = 8;
impl<'c> Merge<'c> {
    fn new(starts: &'c [Option<usize>], nsps: usize) -> Self {
        Self { starts, dpos: data_pos(), next: [0; MAX_GROUP], nsps }
    }
    /// `(candidate, data symbol, window start)`
    fn next(&mut self) -> Option<(usize, usize, usize)> {
        let mut best: Option<(usize, usize, usize)> = None;
        for (ci, st) in self.starts.iter().enumerate().take(MAX_GROUP) {
            let Some(st) = *st else { continue };
            let k = self.next[ci] as usize;
            if k >= N_DATA {
                continue;
            }
            let w = st + self.dpos[k] as usize * self.nsps;
            if best.map_or(true, |b| w < b.2) {
                best = Some((ci, k, w));
            }
        }
        if let Some((ci, _, _)) = best {
            self.next[ci] += 1;
        }
        best
    }
}

/// Frame geometry check of a candidate: the first wide bin, or `None` when the frame or the wide
/// window does not fit.
fn wide_start_of(g: &Geom, total: usize, c: &Cand) -> Option<usize> {
    if c.base_bin + g.bpt < 64 {
        return None;
    }
    let wide_start = c.base_bin + g.bpt - 64;
    let bps = N_TONES * (2 + g.bpt);
    if c.start_sample + N_SYMBOLS * g.nsps > total || wide_start + bps > g.nsps / 2 {
        return None;
    }
    Some(wide_start)
}

/// ONE forward pass over the PCM for a group of candidates: one FFT per data symbol of each
/// candidate gives its wide energy row (`bins_per_symbol = 64*(2+bpt)` bins, the layout of
/// `q65::rx::extract_data_energies_wide`), which is reduced at once to the fast-fading metric of
/// EVERY b90 step (7 x 64 values) - the wide spectrogram itself (157 KiB) is never stored.
/// The first candidate's metrics go straight into `blocks` (RAM); the others are spilled to the
/// SD-backed store as `fades.len()*64`-float records (`RPR` per row) in pass order and loaded by [`load_spilled`]
/// when their turn comes.  `raw_sum[c]` receives the candidate's total wide energy.
/// Returns `false` on abort.
#[allow(clippy::too_many_arguments)]
fn extract_group<S: SampleSource, T: SpectraStore, L: DebugLog>(
    src: &S,
    store: &mut T,
    g: &Geom,
    fades: &[Fade],
    starts: &[Option<usize>],
    wide: &[usize],
    scratch: &mut [f32],
    blocks: &mut [f32],
    raw_sum: &mut [f64],
    log: &mut L,
) -> bool {
    let bps = N_TONES * (2 + g.bpt);
    let nb = fades.len();
    let (fmem, rest) = scratch.split_at_mut(Front::mem_len(g.nsps));
    let Some(mut fe) = Front::new_in(g.nsps, fmem) else { return false };
    let (row, rest) = rest.split_at_mut(bps);
    let recs = &mut rest[..RPR * nb * M];
    let mut nrec = 0usize; // spilled records so far
    let mut merge = Merge::new(starts, g.nsps);
    while let Some((ci, k, start)) = merge.next() {
        // abort poll per symbol: STOP / next-capture deadline
        if log.aborted() {
            return false;
        }
        fe.load(src, start);
        fe.fft.powers(wide[ci], row);
        for &v in row.iter() {
            raw_sum[ci] += v as f64;
        }
        if ci == 0 {
            let r = bp::intr_row(k) * M;
            for (bi, f) in fades.iter().enumerate() {
                metric_row(&f.w, row, g.bpt, &mut blocks[bi * BLOCK + r..bi * BLOCK + r + M]);
            }
        } else {
            if nrec == 0 {
                store.begin(RPR * nb * M);
            }
            let o = (nrec % RPR) * nb * M;
            for (bi, f) in fades.iter().enumerate() {
                metric_row(&f.w, row, g.bpt, &mut recs[o + bi * M..o + bi * M + M]);
            }
            nrec += 1;
            if nrec % RPR == 0 {
                store.append_row(recs);
            }
        }
    }
    if nrec % RPR != 0 {
        store.append_row(recs); // the last, partly filled row
    }
    true
}

/// Load the spilled metric records of candidate `ci` (> 0) of the group into `blocks`
/// (one forward read of the store).  Returns `false` on abort.
fn load_spilled<T: SpectraStore, L: DebugLog>(
    store: &T,
    g: &Geom,
    nb: usize,
    starts: &[Option<usize>],
    ci: usize,
    rec: &mut [f32],
    blocks: &mut [f32],
    log: &mut L,
) -> bool {
    let recs = &mut rec[..RPR * nb * M];
    let mut merge = Merge::new(starts, g.nsps);
    let mut idx = 0usize;
    let mut have_row = usize::MAX;
    while let Some((cj, k, _)) = merge.next() {
        if cj == 0 {
            continue;
        }
        if cj == ci {
            if have_row != idx / RPR {
                if log.aborted() {
                    return false;
                }
                have_row = idx / RPR;
                store.read_row(have_row, recs);
            }
            let o = (idx % RPR) * nb * M;
            let r = bp::intr_row(k) * M;
            for bi in 0..nb {
                blocks[bi * BLOCK + r..bi * BLOCK + r + M].copy_from_slice(&recs[o + bi * M..o + bi * M + M]);
            }
            if k == N_DATA - 1 {
                break;
            }
        }
        idx += 1;
    }
    true
}

/// Main entry: decode one captured 60-s minute.
///
/// Memory: one arena for the whole decode, re-carved per phase -
/// sync `[front end | row | comb]`, extraction `[front end | row | record | .. | metric blocks]`,
/// b90 sweep `[BP scratch | metric blocks]`, SNR `[front end | spectrum]` - so the peak is fixed and
/// nothing fragments.  PCM passes: 1 (sync) + 1 (best candidate) [+ 1 (all other candidates)]
/// [+ 1 (SNR of the decode)].
pub fn decode<S: SampleSource, T: SpectraStore, L: DebugLog>(
    src: &S,
    store: &mut T,
    log: &mut L,
    p: &Params,
    mode: Mode<'_>,
) -> Option<Decoded> {
    let g = Geom::new(p.sample_rate);
    {
        let mut msg = String::new();
        fmt_line(&mut msg, "Q65B PARAMS", &[
            ("fs", Num::U(p.sample_rate as u64)),
            ("nsps", Num::U(g.nsps as u64)),
            ("bpt", Num::U(g.bpt as u64)),
            ("nom_start", Num::U(p.nominal_start_sample as u64)),
            ("fmin", Num::F(p.freq_min_hz)),
            ("fmax", Num::F(p.freq_max_hz)),
        ]);
        log.line(&msg);
    }
    let total = src.len();
    let sg = sync_geom(total, &g, p, log)?;

    let ctx = DecodeContext { callsign_hash_table: None };
    let esno = default_es_no_metric();
    let baud = 1.0 / <Q65d60 as ModulationParams>::SYMBOL_DT;
    let bps = N_TONES * (2 + g.bpt);
    // Fast-fading (Lorentzian) metric swept over the submode b90 range - required for
    // Doppler-spread EME.  First ibw to pass CRC wins (matches q65::rx GridDepth::Fast).
    let fades: Vec<Fade> = (IBWA..=IBWB)
        .map(|ibw| {
            let b90 = 1.72f32.powi(ibw);
            let lt = lorentz_taps(b90 / baud, esno);
            let w = lt
                .taps
                .iter()
                .map(|&t| {
                    let t = t * lt.es_no_metric;
                    t / (1.0 + t)
                })
                .collect();
            Fade { ibw, b90, es_no_metric: lt.es_no_metric, w }
        })
        .collect();
    let nb = fades.len();
    let _ = SUBMODE_IDX;

    // the arena
    let front = Front::mem_len(g.nsps);
    let low = bp::BP_MEM.max(front + bps + RPR * nb * M).max(front + 4 * bps);
    let arena_len = (low + nb * BLOCK).max(front + sg.nbins + sg.n_starts * sg.nbins);
    let mut arena: Vec<f32> = Vec::new();
    if g.nsps % 8 != 0 || arena.try_reserve_exact(arena_len).is_err() {
        log.line("Q65B ABORT where=params reason=unsupported_rate_or_no_memory");
        return None;
    }
    arena.resize(arena_len, 0.0);

    let cands = scan_sync(src, &g, p, &sg, &mut arena, log)?;
    if cands.is_empty() {
        log.line("Q65B ABORT where=sync reason=no_candidates");
        return None;
    }

    // The strongest candidate alone first (nothing touches the card but the PCM), then the rest
    // in one more pass.
    let mut first = 0usize;
    while first < cands.len() {
        let grp = if first == 0 { &cands[..1] } else { &cands[first..cands.len().min(first + MAX_GROUP)] };
        first += grp.len();
        if log.aborted() {
            log.line("Q65B ABORT where=decode reason=stop");
            return None;
        }
        let mut starts = [None; MAX_GROUP];
        let mut wide = [0usize; MAX_GROUP];
        let mut raw_sum = [0.0f64; MAX_GROUP];
        for (ci, c) in grp.iter().enumerate() {
            if let Some(w) = wide_start_of(&g, total, c) {
                starts[ci] = Some(c.start_sample);
                wide[ci] = w;
            }
        }
        let starts = &starts[..grp.len()];
        {
            let (scratch, blocks) = arena.split_at_mut(low);
            if !extract_group(src, store, &g, &fades, starts, &wide, scratch, blocks, &mut raw_sum, log) {
                log.line("Q65B ABORT where=energy reason=stop");
                return None;
            }
        }
        for (ci, c) in grp.iter().enumerate() {
            if starts[ci].is_none() {
                log.line("Q65B ABORT where=energy reason=frame_off_buffer");
                continue;
            }
            let (scratch, blocks) = arena.split_at_mut(low);
            if ci > 0 && !load_spilled(store, &g, nb, starts, ci, scratch, blocks, log) {
                log.line("Q65B ABORT where=energy reason=stop");
                return None;
            }
            let raw_mean = (raw_sum[ci] / (bps as f64 * N_DATA as f64)) as f32;
            {
                let mut msg = String::new();
                fmt_line(&mut msg, "Q65B ENERGY", &[
                    ("bin", Num::U(c.base_bin as u64)),
                    ("wide_bins", Num::U(bps as u64)),
                    ("streamed_rows", Num::U(N_DATA as u64)),
                ]);
                log.line(&msg);
            }
            if !(raw_mean > 0.0) {
                log.line("Q65B DECODE ok=0 reason=no_energy");
                continue;
            }

            let mut found: Option<(usize, [i32; 13], u32, f32, [i32; 63])> = None;
            {
                let mut bpd = Bp::new_in(&mut scratch[..bp::BP_MEM]);
                for (bi, f) in fades.iter().enumerate() {
                    if log.aborted() {
                        log.line("Q65B ABORT where=b90 reason=stop");
                        return None;
                    }
                    let noise_var = raw_mean / (1.0 + f.es_no_metric / bps as f32);
                    let intr = &mut blocks[bi * BLOCK..(bi + 1) * BLOCK];
                    for s in 0..N_DATA {
                        let r = bp::intr_row(s) * M;
                        metric_to_prob(&mut intr[r..r + M], noise_var);
                    }
                    let res: Option<([i32; 13], u32, f32, [i32; 63])> = match &mode {
                        Mode::Blind => match bpd.decode(intr, 50) {
                            Ok(r) => Some((r.msg, r.iterations, 0.0, r.codeword)),
                            Err(_) => None,
                        },
                        Mode::Template(cands_cw) => {
                            // BP-free check_codeword_llh
                            let threshold = -260.0f32 + (cands_cw.len() as f32 / 3.0).ln();
                            let mut best = threshold;
                            let mut best_i: Option<usize> = None;
                            for (i, cw) in cands_cw.iter().enumerate() {
                                let mut t = 0.0f32;
                                for k in 0..N_DATA {
                                    let mut x = intr[bp::intr_row(k) * M + cw[k] as usize];
                                    if x < 1.0e-36 {
                                        x = 1.0e-36;
                                    }
                                    t += x.ln();
                                }
                                if t > best {
                                    best = t;
                                    best_i = Some(i);
                                }
                            }
                            best_i.map(|bi| {
                                let cw = cands_cw[bi];
                                let mut info = [0i32; 13];
                                info.copy_from_slice(&cw[..13]);
                                (info, 0, best, cw)
                            })
                        }
                    };
                    let Some((info, iterations, llh, codeword)) = res else { continue };
                    let bits77 = unpack_symbols_to_bits77(&info);
                    if Q65Message.unpack(&bits77, &ctx).is_none() {
                        continue;
                    }
                    found = Some((bi, info, iterations, llh, codeword));
                    break;
                }
            }
            let Some((bi, info, iterations, llh, codeword)) = found else {
                log.line("Q65B DECODE ok=0 reason=b90_sweep_exhausted");
                continue;
            };
            let f = &fades[bi];
            let text = Q65Message.unpack(&unpack_symbols_to_bits77(&info), &ctx)?;

            // WSJT-X SNR (stock q65::rx passes snr_db_wide only as the fallback of q65_snr_db)
            let (snr, snr_wide) = snr_pass(src, &g, &mut arena, c.start_sample, c.base_bin, &codeword)?;
            let dt_s = c.start_sample as f32 / p.sample_rate as f32
                - p.nominal_start_sample as f32 / p.sample_rate as f32;
            {
                let mut msg = String::new();
                fmt_line(&mut msg, "Q65B RESULT", &[
                    ("ibw", Num::U(f.ibw as u64)),
                    ("b90", Num::F(f.b90)),
                    ("freq", Num::F(c.freq_hz)),
                    ("dt_s", Num::F(dt_s)),
                    ("snr_db", Num::F(snr)),
                    ("snr_wide", Num::F(snr_wide)),
                    ("iters", Num::U(iterations as u64)),
                ]);
                msg.push_str(" text=");
                msg.push_str(&text);
                log.line(&msg);
            }
            return Some(Decoded { text, freq_hz: c.freq_hz, dt_s, snr_db: snr, iterations, llh });
        }
    }
    log.line("Q65B ABORT where=decode reason=all_candidates_failed");
    None
}

// ── tiny no_std-safe numeric formatter for log lines ─────────────────
enum Num {
    U(u64),
    F(f32),
}
fn push_u64(out: &mut String, v: u64) {
    if v == 0 { out.push(0x30 as char); return; }
    let mut buf = [0u8; 20];
    let mut i = 20usize;
    let mut n = v;
    while n > 0 { i -= 1; buf[i] = b'0' + (n % 10) as u8; n /= 10; }
    for &b in &buf[i..] { out.push(b as char); }
}
fn push_f4(out: &mut String, f: f32) {
    if f != f { out.push_str("nan"); return; }
    let neg = f < 0.0;
    let mut x = if neg { -f } else { f };
    if neg { out.push(0x2d as char); }
    if x > 4.2e9 { x = 4.2e9; }           // clamp to u64-safe integer part
    let ip = x as u64;
    push_u64(out, ip);
    out.push(0x2e as char);
    let mut fr = x - ip as f32;
    for _ in 0..4 {
        fr *= 10.0;
        let mut d = fr as u32;
        if d > 9 { d = 9; }
        out.push((b'0' + d as u8) as char);
        fr -= d as f32;
    }
}
fn fmt_line(out: &mut String, tag: &str, kvs: &[(&str, Num)]) {
    out.push_str(tag);
    for (k, v) in kvs {
        out.push(0x20 as char);
        out.push_str(k);
        out.push(0x3d as char);
        match v {
            Num::U(u) => push_u64(out, *u),
            Num::F(f) => push_f4(out, *f),
        }
    }
}
