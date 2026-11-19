// SPDX-License-Identifier: GPL-3.0-or-later
//! Single-bin DFT (Goertzel) power, streamed one sample at a time.
//!
//! Replaces the crate's full `nsps`-point rustfft (a 7200-point
//! complex buffer = 57.6 KB) used by `engine::spectrogram` and
//! `q65::rx::extract_data_energies`. Goertzel computes `|X[k]|^2` for
//! a chosen set of bins with O(1) memory per bin and a single forward
//! pass over the real samples — so the samples never have to be held
//! resident; they can be read from the SD backing store in small
//! blocks. The returned power is the *unnormalised* DFT magnitude
//! squared, matching `rustfft`'s `buf[bin].norm_sqr()`, so it feeds
//! `QraCode::mfsk_bessel_metric` identically to the reference path.

use num_traits::Float;

/// Accurate f64 cosine for w in [0, PI) (the only range the bank uses:
/// w = TAU*k/nsps, k < nsps/2).  Avoids libm`s general `cos`, whose
/// Payne-Hanek range reduction (`rem_pio2`) is ~2.8 KiB of code we
/// cannot afford in the 48 KiB app window.  Max error ~6e-9 on [0,PI).
fn cos_reduced(w: f64) -> f64 {
    use core::f64::consts::PI;
    let mut x = w;
    let mut sign = 1.0f64;
    if x > PI / 2.0 { x = PI - x; sign = -1.0; }   // cos(PI-x) = -cos(x)
    let x2 = x * x;
    let c = 1.0
        - x2 * (1.0 / 2.0
        - x2 * (1.0 / 24.0
        - x2 * (1.0 / 720.0
        - x2 * (1.0 / 40320.0
        - x2 * (1.0 / 3628800.0)))));
    sign * c
}

/// A bank of Goertzel resonators sharing one streamed window of
/// `nsps` real samples. One resonator per requested bin index.
pub struct GoertzelBank {
    // coeff[i] = 2*cos(2*pi*bin/nsps). f64 state: f32 accumulation over
    // 7200 MACs loses enough precision at -14 dB to break BP; f64 keeps
    // the bin powers bit-close to a reference DFT.
    coeff: alloc::vec::Vec<f32>,
    s1: alloc::vec::Vec<f32>, // s_prev
    s2: alloc::vec::Vec<f32>, // s_prev2
    n: usize,
}


/// NEON resonator step for 4 bins at a time (Cortex-A9, f32).
/// `s = x + coeff*s1 - s2; s2 = s1; s1 = s`, per bin.
#[cfg(all(feature = "radio", target_arch = "arm"))]
#[target_feature(enable = "neon")]
unsafe fn push_neon(coeff: &[f32], s1: &mut [f32], s2: &mut [f32], x: f32) {
    use core::arch::arm::*;
    let n = coeff.len();
    let xv = vdupq_n_f32(x);
    let (cp, ap, bp) = (coeff.as_ptr(), s1.as_mut_ptr(), s2.as_mut_ptr());
    let mut i = 0usize;
    while i + 4 <= n {
        let c = vld1q_f32(cp.add(i));
        let a = vld1q_f32(ap.add(i));
        let b = vld1q_f32(bp.add(i));
        let s = vsubq_f32(vaddq_f32(xv, vmulq_f32(c, a)), b);
        vst1q_f32(bp.add(i), a);
        vst1q_f32(ap.add(i), s);
        i += 4;
    }
    while i < n {
        let s = x + *cp.add(i) * *ap.add(i) - *bp.add(i);
        *bp.add(i) = *ap.add(i);
        *ap.add(i) = s;
        i += 1;
    }
}

impl GoertzelBank {
    /// Build resonators for `bins` over an `nsps`-point DFT.
    pub fn new(bins: &[usize], nsps: usize) -> Self {
        let mut coeff = alloc::vec::Vec::with_capacity(bins.len());
        let tau = core::f64::consts::TAU;
        for &k in bins {
            let w = tau * (k as f64) / (nsps as f64);
            coeff.push((2.0 * cos_reduced(w)) as f32);
        }
        let z = alloc::vec![0.0f32; bins.len()];
        Self { coeff, s1: z.clone(), s2: z, n: nsps }
    }

    /// Number of bins in the bank.
    #[inline]
    pub fn len(&self) -> usize {
        self.coeff.len()
    }
    #[inline]
    pub fn is_empty(&self) -> bool {
        self.coeff.is_empty()
    }

    /// Clear accumulators to start a fresh window.
    #[inline]
    pub fn reset(&mut self) {
        for v in self.s1.iter_mut() {
            *v = 0.0;
        }
        for v in self.s2.iter_mut() {
            *v = 0.0;
        }
    }

    /// Feed one sample into every resonator.
    #[cfg_attr(feature = "radio", optimize(speed))]
    #[inline]
    pub fn push(&mut self, x: f32) {
        // The per-sample inner loop dominates the sync/energy front end
        // (windows * nsps * bins). On the Cortex-A9, NEON (f32-only) does
        // the resonator recurrence 4 bins at a time; fat-LTO at opt-z does
        // not autovectorize, so the ARM path uses explicit NEON intrinsics.
        #[cfg(all(feature = "radio", target_arch = "arm"))]
        unsafe {
            push_neon(&self.coeff, &mut self.s1, &mut self.s2, x);
        }
        #[cfg(not(all(feature = "radio", target_arch = "arm")))]
        for ((c, s1), s2) in self.coeff.iter().zip(self.s1.iter_mut()).zip(self.s2.iter_mut()) {
            let s = x + *c * *s1 - *s2;
            *s2 = *s1;
            *s1 = s;
        }
    }

    /// Feed a contiguous block of samples.
    #[inline]
    pub fn push_block(&mut self, xs: &[f32]) {
        for &x in xs {
            self.push(x);
        }
    }

    /// `|X[bin_i]|^2` for every resonator, written into `out`
    /// (`out.len()` must equal [`Self::len`]). Does not reset.
    #[cfg_attr(feature = "radio", optimize(speed))]
    pub fn powers(&self, out: &mut [f32]) {
        debug_assert_eq!(out.len(), self.coeff.len());
        for (o, ((c, s1), s2)) in out.iter_mut().zip(self.coeff.iter().zip(self.s1.iter()).zip(self.s2.iter())) {
            *o = *s1 * *s1 + *s2 * *s2 - *c * *s1 * *s2;
        }
    }

    #[inline]
    pub fn nsps(&self) -> usize {
        self.n
    }
}
