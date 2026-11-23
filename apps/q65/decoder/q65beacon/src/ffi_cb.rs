// SPDX-License-Identifier: GPL-3.0-or-later
//! no_std, callback-based C ABI for the on-radio ICAP shim.
//!
//! Mirrors the std path-based [`crate::ffi::q65b_decode`] but, instead of
//! opening files itself, takes a table of C callbacks the shim backs with
//! the radio's own SD file system (ICAP `svc->f_*`).  The decoder core
//! ([`crate::decode`]) only ever holds a few small resident buffers; the
//! bulk PCM and the intermediate sync spectra live behind these callbacks
//! on the SD card, exactly as the host harness keeps them in files.
//!
//! The matching C declarations are in `shim/q65beacon_cb.h`.
use core::ffi::{c_char, c_int, c_void};

use alloc::vec::Vec;

use crate::debuglog::DebugLog;
use crate::source::{SampleSource, SpectraStore};
use crate::{Decoded, Mode, Params};

/// Q65 data-symbol count (one template codeword length).  Kept in sync
/// with `crate::N_DATA` (private); the candidate buffer is `ncand * NDATA`.
const NDATA: usize = 63;

#[repr(C)]
pub struct Q65bResult {
    pub freq_hz: f32,
    pub dt_s: f32,
    pub snr_db: f32,
    pub iterations: u32,
    pub llh: f32,
    pub ok: i32,
    pub text: [c_char; 32],
}

/// Table of SD-backed callbacks the shim supplies.  `ctx` is passed back
/// to every callback unchanged.  Pointers are into the shim's own memory.
#[repr(C)]
pub struct Q65Cb {
    pub ctx: *mut c_void,
    /// Total 12 kHz f32 samples available in the capture.
    pub pcm_len: extern "C" fn(*mut c_void) -> usize,
    /// Fill `out[0..n]` with samples `[start, start+n)`; past the end must
    /// read as 0.0.  Returns the count of real samples read.
    pub pcm_read: extern "C" fn(*mut c_void, start: usize, out: *mut f32, n: usize) -> usize,
    /// Begin a fresh spectra matrix with `ncols` f32 per row (truncates).
    pub spec_begin: extern "C" fn(*mut c_void, ncols: usize),
    /// Append one row of `ncols` f32.
    pub spec_append: extern "C" fn(*mut c_void, row: *const f32, ncols: usize),
    /// Read row `idx` (`ncols` f32) into `out`.
    pub spec_read: extern "C" fn(*mut c_void, idx: usize, out: *mut f32, ncols: usize),
    /// Number of rows appended since the last `begin`.
    pub spec_nrows: extern "C" fn(*mut c_void) -> usize,
    /// Columns per row set by the last `begin`.
    pub spec_ncols: extern "C" fn(*mut c_void) -> usize,
    /// Append one debug-log line (no trailing newline), `len` bytes.
    pub log_line: extern "C" fn(*mut c_void, s: *const c_char, len: usize),
    /// Cooperative abort poll: non-zero when the user pressed STOP.
    pub should_abort: extern "C" fn(*mut c_void) -> c_int,
}

struct CbSrc<'a>(&'a Q65Cb);
impl SampleSource for CbSrc<'_> {
    fn len(&self) -> usize {
        (self.0.pcm_len)(self.0.ctx)
    }
    fn read_block(&self, start: usize, out: &mut [f32]) -> usize {
        (self.0.pcm_read)(self.0.ctx, start, out.as_mut_ptr(), out.len())
    }
}

struct CbStore<'a> {
    cb: &'a Q65Cb,
    ncols: usize,
}
impl SpectraStore for CbStore<'_> {
    fn begin(&mut self, ncols: usize) {
        self.ncols = ncols;
        (self.cb.spec_begin)(self.cb.ctx, ncols);
    }
    fn append_row(&mut self, row: &[f32]) {
        (self.cb.spec_append)(self.cb.ctx, row.as_ptr(), row.len());
    }
    fn read_row(&self, idx: usize, out: &mut [f32]) {
        (self.cb.spec_read)(self.cb.ctx, idx, out.as_mut_ptr(), out.len());
    }
    fn nrows(&self) -> usize {
        (self.cb.spec_nrows)(self.cb.ctx)
    }
    fn ncols(&self) -> usize {
        self.ncols
    }
}

struct CbLog<'a>(&'a Q65Cb);
impl DebugLog for CbLog<'_> {
    fn line(&mut self, s: &str) {
        (self.0.log_line)(self.0.ctx, s.as_ptr() as *const c_char, s.len());
    }
    fn aborted(&mut self) -> bool {
        (self.0.should_abort)(self.0.ctx) != 0
    }
}

fn fill(out: &mut Q65bResult, d: &Decoded) {
    out.freq_hz = d.freq_hz;
    out.dt_s = d.dt_s;
    out.snr_db = d.snr_db;
    out.iterations = d.iterations;
    out.llh = d.llh;
    out.ok = 1;
    let bytes = d.text.as_bytes();
    let n = bytes.len().min(31);
    for i in 0..n {
        out.text[i] = bytes[i] as c_char;
    }
    out.text[n] = 0;
}

/// Decode one captured T/R period via SD-backed callbacks.
///
/// `period_s`: T/R period, 15 / 30 / 60 s; `submode`: 0..=4 = A..E.
/// `candidates`/`ncand`: flattened `ncand*63` i32 template codewords, or
/// NULL/0 for a blind belief-propagation decode (the default).
/// `sample_rate`: capture rate in Hz (0 -> 12000).
///
/// Returns 0 = decoded, 1 = ran but no decode (also for an unsupported period / submode, logged as
/// `Q65B ABORT where=params`), <0 = bad argument.
///
/// # Safety
/// `cb` and `out` must be valid; `candidates` (if non-NULL) must point to
/// `ncand*63` i32.  `q65b_heap_init` must have run first.
#[no_mangle]
pub unsafe extern "C" fn q65b_decode_cb(
    cb: *const Q65Cb,
    period_s: u32,
    submode: u32,
    nominal_start_sample: u32,
    sample_rate: u32,
    fmin_hz: f32,
    fmax_hz: f32,
    candidates: *const i32,
    ncand: u32,
    out: *mut Q65bResult,
) -> c_int {
    let Some(out) = out.as_mut() else { return -1 };
    out.ok = 0;
    out.text[0] = 0;
    let Some(cb) = cb.as_ref() else { return -1 };

    let src = CbSrc(cb);
    let mut store = CbStore { cb, ncols: 0 };
    let mut log = CbLog(cb);

    let params = Params {
        period_s,
        submode: submode.min(255) as u8,
        nominal_start_sample: nominal_start_sample as usize,
        sample_rate: if sample_rate == 0 { 12_000 } else { sample_rate },
        freq_min_hz: fmin_hz,
        freq_max_hz: fmax_hz,
        ..Params::default()
    };

    let owned: Vec<[i32; NDATA]> = if !candidates.is_null() && ncand > 0 {
        let flat = core::slice::from_raw_parts(candidates, ncand as usize * NDATA);
        flat.chunks_exact(NDATA)
            .map(|c| {
                let mut a = [0i32; NDATA];
                a.copy_from_slice(c);
                a
            })
            .collect()
    } else {
        Vec::new()
    };
    let mode = if owned.is_empty() {
        Mode::Blind
    } else {
        Mode::Template(&owned)
    };

    match crate::decode(&src, &mut store, &mut log, &params, mode) {
        Some(d) => {
            fill(out, &d);
            0
        }
        None => 1,
    }
}
