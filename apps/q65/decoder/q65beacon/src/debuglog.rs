// SPDX-License-Identifier: GPL-3.0-or-later
//! Stage/metric debug logging. On the host harness this writes a text
//! file; on the radio the ICAP shim points it at an SD-card file so a
//! failed on-air decode can be diagnosed from the log afterwards.
//!
//! ## Line format (stable, greppable)
//!
//! ```text
//! Q65B <STAGE> <key=value> <key=value> ...
//! ```
//!
//! Stages, in emission order:
//! - `OPEN`    src_samples, mode
//! - `PARAMS`  fs, nsps, bpt, nom_start, fmin, fmax, early_s, late_s
//! - `SPECTRA` rows, cols, noise (narrow tone-0 sync spectrogram built)
//! - `CAND`    i, bin, freq, row, start, syncpwr, score (one per kept candidate)
//! - `PICK`    bin, freq, start, score (candidate handed to decode)
//! - `ENERGY`  k_data=63 (per-symbol data-tone energies extracted)
//! - `METRIC`  sigma (bessel noise estimate)
//! - `DECODE`  mode, ok, iters|llh (BP iters, or template log-likelihood)
//! - `RESULT`  text, freq, dt_s, snr_db
//! - `ABORT`   where, reason  (any early return / failure path)

#[cfg(not(feature = "std"))]
use alloc::string::String;

/// Sink for one formatted log line (no trailing newline).
pub trait DebugLog {
    fn line(&mut self, s: &str);
    /// Cooperative abort poll (STOP on the radio). Default never aborts;
    /// the on-radio CbLog overrides it to poll the ICAP should_stop().
    fn aborted(&mut self) -> bool {
        false
    }
}

/// Discards everything.
pub struct NullLog;
impl DebugLog for NullLog {
    #[inline]
    fn line(&mut self, _s: &str) {}
}

/// Buffers lines in memory (handy for the C ABI to hand back, or for
/// tests). Each pushed line carries no newline; join with '\n'.
#[derive(Default)]
pub struct StringLog {
    pub lines: alloc::vec::Vec<String>,
}
impl DebugLog for StringLog {
    fn line(&mut self, s: &str) {
        self.lines.push(String::from(s));
    }
}

#[cfg(feature = "std")]
mod host {
    use super::*;
    use std::fs::File;
    use std::io::{BufWriter, Write};

    /// Appends lines to an SD-card/host text file.
    pub struct FileLog {
        w: BufWriter<File>,
    }
    impl FileLog {
        pub fn create(path: &str) -> std::io::Result<Self> {
            Ok(Self { w: BufWriter::new(File::create(path)?) })
        }
    }
    impl DebugLog for FileLog {
        fn line(&mut self, s: &str) {
            let _ = writeln!(self.w, "{s}");
            let _ = self.w.flush();
        }
    }
}
#[cfg(feature = "std")]
pub use host::FileLog;
