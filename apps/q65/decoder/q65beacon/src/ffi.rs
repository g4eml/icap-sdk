// SPDX-License-Identifier: GPL-3.0-or-later
//! C-callable ABI for the ICAP app's C shim.
//!
//! The shim owns audio capture, RTC/UTC alignment, SD I/O and the
//! display. This milestone exposes the decoder as a freestanding
//! C-linkable entry plus the debug-log hook. The host form below takes
//! **SD file paths** (the simplest capture→decode contract); the
//! on-radio form is identical but the shim may instead supply read/
//! write callbacks that wrap the radio's FS (see `q65b_decode_cb`).
//!
//! ```c
//! typedef struct {
//!     float    freq_hz;      // tone-0 frequency
//!     float    dt_s;         // start offset vs nominal, seconds
//!     float    snr_db;       // WSJT-X 2500 Hz-ref SNR
//!     uint32_t iterations;   // BP iterations (blind) / 0 (template)
//!     float    llh;          // template log-likelihood / 0 (blind)
//!     int32_t  ok;           // 1 = decoded, 0 = no decode
//!     char     text[32];     // NUL-terminated message
//! } q65b_result_t;
//!
//! // candidates: flattened ncand×63 i32 codewords; NULL/0 => blind BP.
//! // returns 0 ok+decoded, 1 ok+no-decode, <0 on error.
//! int32_t q65b_decode(const char* pcm_path, const char* spectra_path,
//!                     const char* log_path, uint32_t nominal_start_sample,
//!                     float fmin_hz, float fmax_hz,
//!                     const int32_t* candidates, uint32_t ncand,
//!                     uint32_t period_s, uint32_t submode,
//!                     q65b_result_t* out);
//! ```

use core::ffi::{c_char, c_int};

use crate::debuglog::{FileLog, NullLog};
use crate::source::{FilePcmSource, FileSpectraStore};
use crate::{Decoded, Mode, Params};

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

unsafe fn cstr<'a>(p: *const c_char) -> Option<&'a str> {
    if p.is_null() {
        return None;
    }
    core::ffi::CStr::from_ptr(p).to_str().ok()
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

/// See module-level C signature.
///
/// # Safety
/// All non-NULL pointers must be valid; `candidates` (if non-NULL)
/// must point to `ncand*63` i32; `out` must be a valid `Q65bResult`.
#[no_mangle]
pub unsafe extern "C" fn q65b_decode(
    pcm_path: *const c_char,
    spectra_path: *const c_char,
    log_path: *const c_char,
    nominal_start_sample: u32,
    fmin_hz: f32,
    fmax_hz: f32,
    candidates: *const i32,
    ncand: u32,
    period_s: u32,
    submode: u32,
    out: *mut Q65bResult,
) -> c_int {
    let Some(out) = (unsafe { out.as_mut() }) else { return -1 };
    out.ok = 0;
    out.text[0] = 0;

    let Some(pcm) = (unsafe { cstr(pcm_path) }) else { return -2 };
    let Some(spec) = (unsafe { cstr(spectra_path) }) else { return -2 };

    let Ok(src) = FilePcmSource::open(pcm) else { return -3 };
    let Ok(mut store) = FileSpectraStore::create(spec) else { return -4 };

    let params = Params {
        period_s,
        submode: submode.min(255) as u8,
        nominal_start_sample: nominal_start_sample as usize,
        freq_min_hz: fmin_hz,
        freq_max_hz: fmax_hz,
        ..Params::default()
    };

    // Rebuild the candidate codewords if a template list was supplied.
    let owned: alloc::vec::Vec<[i32; 63]> = if !candidates.is_null() && ncand > 0 {
        let flat = unsafe { core::slice::from_raw_parts(candidates, ncand as usize * 63) };
        flat.chunks_exact(63).map(|c| {
            let mut a = [0i32; 63];
            a.copy_from_slice(c);
            a
        }).collect()
    } else {
        alloc::vec::Vec::new()
    };
    let mode = if owned.is_empty() { Mode::Blind } else { Mode::Template(&owned) };

    let logpath = unsafe { cstr(log_path) };
    let decoded = match logpath {
        Some(lp) => {
            let mut log = FileLog::create(lp).ok();
            match log.as_mut() {
                Some(l) => crate::decode(&src, &mut store, l, &params, mode),
                None => crate::decode(&src, &mut store, &mut NullLog, &params, mode),
            }
        }
        None => crate::decode(&src, &mut store, &mut NullLog, &params, mode),
    };

    match decoded {
        Some(d) => {
            fill(out, &d);
            0
        }
        None => 1,
    }
}
