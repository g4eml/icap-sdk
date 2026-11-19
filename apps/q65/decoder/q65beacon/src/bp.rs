// SPDX-License-Identifier: GPL-3.0-or-later
//! Q65 QRA(15,65)/GF(64) belief-propagation decoder: the same message passing as mfsk-core's
//! `QraCode::extrinsic` + `map_decode` + the `Q65Codec` CRC-12 / puncturing layer (a port of
//! qracodes.c / q65.c), with the same operations in the same order, so the result (messages, iteration
//! count, decoded symbols) is bit-identical to the stock path - but
//!
//!  * the scratch is 216 + 151 message rows of 64 f32 (94 KB, borrowed from the caller's arena)
//!    instead of 144 KB + 16 KB: the 65 intrinsic rows (which stock copies into the head of its
//!    check->variable array) are read where the caller already has them, and the extrinsic / MAP rows
//!    are read in place;
//!  * the element-wise kernels (64-point Walsh-Hadamard, products, scaling, max) use NEON on the
//!    Cortex-A9, 4 lanes at a time.  Element-wise IEEE add/sub/mul give the same bits as the scalar
//!    code; sums stay sequential scalar (their order matters).
//!
//! `selftest` (feature) runs both kernels on ARM and compares the bits.
use mfsk_core::fec::qra::q65::crc12;
use mfsk_core::fec::qra::QraCode;
use mfsk_core::fec::qra15_65_64::QRA15_65_64_IRR_E23;

pub const M: usize = 64;
const UNI: f32 = 1.0 / 64.0;

// ── code tables, as bytes ────────────────────────────────────────────────────────────────────────
// Compile-time copies of the QRA15_65_64_IRR_E23 tables (i32 in mfsk-core).  The decoder references
// only these, so the 16 KiB i32 permutation matrix and the other i32 tables are not linked into the
// app's .rodata (which shares the 240 KiB region with the decoder heap).
const CODE: &QraCode = &QRA15_65_64_IRR_E23;
const fn tab<const N: usize>(src: &[i32]) -> [u8; N] {
    assert!(src.len() == N);
    let mut t = [0u8; N];
    let mut i = 0;
    while i < N {
        t[i] = src[i] as u8;
        i += 1;
    }
    t
}
const V: usize = 65; // variable nodes (K info + NC checks symbols)
const C: usize = 116; // V + factor nodes
const K_INFO: usize = 15;
const N_USER: usize = 13;
const NC: usize = 50;
const NMSG: usize = 216;
const MAXVDEG: usize = 5;
const MAXCDEG: usize = 3;
const _: () = assert!(
    CODE.V == V && CODE.C == C && CODE.K == K_INFO && CODE.NC == NC && CODE.NMSG == NMSG && CODE.M == M
        && CODE.MAXVDEG == MAXVDEG && CODE.MAXCDEG == MAXCDEG && CODE.a == 1
);
static PERM: [u8; 63 * M] = tab(CODE.gfpmat);
static V2CM: [u8; V * MAXVDEG] = tab(CODE.v2cmidx);
static C2VM: [u8; C * MAXCDEG] = tab(CODE.c2vmidx);
static MSGW: [u8; NMSG] = tab(CODE.msgw);
static VDEG: [u8; V] = tab(CODE.vdeg);
static CDEG: [u8; C] = tab(CODE.cdeg);
static ACC_IDX: [u8; NC + 1] = tab(CODE.acc_input_idx);
static ACC_WLOG: [u8; NC + 1] = tab(CODE.acc_input_wlog);
static GFLOG: [u8; M] = tab(CODE.gflog);
static GFEXP: [u8; M - 1] = tab(CODE.gfexp);

/// `QraCode::encode` for this code (a == 1): 15 information symbols -> 65 codeword symbols.
fn encode(x: &[i32; K_INFO], y: &mut [i32; V]) {
    y[..K_INFO].copy_from_slice(x);
    let mut chk = 0i32;
    for k in 0..NC {
        let t = x[ACC_IDX[k] as usize] as usize & (M - 1);
        if t != 0 {
            let lg = (GFLOG[t] as usize + ACC_WLOG[k] as usize) % (M - 1);
            chk ^= GFEXP[lg] as i32;
        }
        y[K_INFO + k] = chk;
    }
}

/// Element-wise kernels over one 64-entry message.
///
/// # Safety
/// every pointer addresses 64 valid f32; `out` never overlaps `a`.
pub trait Kern {
    /// In-place 64-point Walsh-Hadamard transform, stages h = 1, 2, 4, .. 32 (as `npfwht::fwht`).
    unsafe fn fwht(b: *mut f32);
    /// `out[j] = c * a[j]`.
    unsafe fn set_scaled(out: *mut f32, a: *const f32, c: f32);
    /// `out[j] *= a[j]`.
    unsafe fn mul_assign(out: *mut f32, a: *const f32);
    /// `out[j] *= c`.
    unsafe fn scale(out: *mut f32, c: f32);
    /// max(0, max_j a[j]) (as `pdmath::max`).
    unsafe fn max0(a: *const f32) -> f32;
}

pub struct Scalar;
impl Kern for Scalar {
    #[inline(always)]
    unsafe fn fwht(b: *mut f32) {
        let mut h = 1;
        while h < M {
            let mut i = 0;
            while i < M {
                for j in i..i + h {
                    let x = *b.add(j);
                    let y = *b.add(j + h);
                    *b.add(j) = x + y;
                    *b.add(j + h) = x - y;
                }
                i += h * 2;
            }
            h *= 2;
        }
    }
    #[inline(always)]
    unsafe fn set_scaled(out: *mut f32, a: *const f32, c: f32) {
        for j in 0..M {
            *out.add(j) = c * *a.add(j);
        }
    }
    #[inline(always)]
    unsafe fn mul_assign(out: *mut f32, a: *const f32) {
        for j in 0..M {
            *out.add(j) *= *a.add(j);
        }
    }
    #[inline(always)]
    unsafe fn scale(out: *mut f32, c: f32) {
        for j in 0..M {
            *out.add(j) *= c;
        }
    }
    #[inline(always)]
    unsafe fn max0(a: *const f32) -> f32 {
        let mut m = 0.0f32;
        for j in 0..M {
            let v = *a.add(j);
            if v >= m {
                m = v;
            }
        }
        m
    }
}

#[cfg(target_arch = "arm")]
pub struct Neon;
#[cfg(target_arch = "arm")]
mod neon {
    use super::{Kern, Neon};
    use core::arch::arm::*;

    /// stages h = 1, 2 inside one vector
    #[inline(always)]
    unsafe fn h12(x: float32x4_t) -> float32x4_t {
        let r = vrev64q_f32(x); // [a1 a0 a3 a2]
        let s = vaddq_f32(x, r); // [a0+a1 . a2+a3 .]
        let d = vsubq_f32(x, r); // [a0-a1 . a2-a3 .]
        let h1 = vtrnq_f32(s, d).0; // [a0+a1 a0-a1 a2+a3 a2-a3]
        let (lo, hi) = (vget_low_f32(h1), vget_high_f32(h1));
        vcombine_f32(vadd_f32(lo, hi), vsub_f32(lo, hi))
    }
    /// stages h = 1 .. 16 of the 32 floats at `p` (8 vectors), left in registers
    #[inline(always)]
    unsafe fn half(p: *const f32) -> [float32x4_t; 8] {
        let a0 = h12(vld1q_f32(p));
        let a1 = h12(vld1q_f32(p.add(4)));
        let a2 = h12(vld1q_f32(p.add(8)));
        let a3 = h12(vld1q_f32(p.add(12)));
        let a4 = h12(vld1q_f32(p.add(16)));
        let a5 = h12(vld1q_f32(p.add(20)));
        let a6 = h12(vld1q_f32(p.add(24)));
        let a7 = h12(vld1q_f32(p.add(28)));
        // h = 4
        let (b0, b1) = (vaddq_f32(a0, a1), vsubq_f32(a0, a1));
        let (b2, b3) = (vaddq_f32(a2, a3), vsubq_f32(a2, a3));
        let (b4, b5) = (vaddq_f32(a4, a5), vsubq_f32(a4, a5));
        let (b6, b7) = (vaddq_f32(a6, a7), vsubq_f32(a6, a7));
        // h = 8
        let (c0, c2) = (vaddq_f32(b0, b2), vsubq_f32(b0, b2));
        let (c1, c3) = (vaddq_f32(b1, b3), vsubq_f32(b1, b3));
        let (c4, c6) = (vaddq_f32(b4, b6), vsubq_f32(b4, b6));
        let (c5, c7) = (vaddq_f32(b5, b7), vsubq_f32(b5, b7));
        // h = 16
        [
            vaddq_f32(c0, c4),
            vaddq_f32(c1, c5),
            vaddq_f32(c2, c6),
            vaddq_f32(c3, c7),
            vsubq_f32(c0, c4),
            vsubq_f32(c1, c5),
            vsubq_f32(c2, c6),
            vsubq_f32(c3, c7),
        ]
    }
    #[target_feature(enable = "neon")]
    unsafe fn fwht_n(p: *mut f32) {
        let lo = half(p);
        for (i, x) in lo.iter().enumerate() {
            vst1q_f32(p.add(4 * i), *x);
        }
        let hi = half(p.add(32));
        // h = 32
        for (i, y) in hi.iter().enumerate() {
            let x = vld1q_f32(p.add(4 * i));
            vst1q_f32(p.add(4 * i), vaddq_f32(x, *y));
            vst1q_f32(p.add(32 + 4 * i), vsubq_f32(x, *y));
        }
    }
    #[target_feature(enable = "neon")]
    unsafe fn set_scaled_n(o: *mut f32, s: *const f32, c: f32) {
        let cv = vdupq_n_f32(c);
        let mut i = 0;
        while i < 64 {
            vst1q_f32(o.add(i), vmulq_f32(cv, vld1q_f32(s.add(i))));
            vst1q_f32(o.add(i + 4), vmulq_f32(cv, vld1q_f32(s.add(i + 4))));
            vst1q_f32(o.add(i + 8), vmulq_f32(cv, vld1q_f32(s.add(i + 8))));
            vst1q_f32(o.add(i + 12), vmulq_f32(cv, vld1q_f32(s.add(i + 12))));
            i += 16;
        }
    }
    #[target_feature(enable = "neon")]
    unsafe fn mul_assign_n(o: *mut f32, s: *const f32) {
        let mut i = 0;
        while i < 64 {
            vst1q_f32(o.add(i), vmulq_f32(vld1q_f32(o.add(i)), vld1q_f32(s.add(i))));
            vst1q_f32(o.add(i + 4), vmulq_f32(vld1q_f32(o.add(i + 4)), vld1q_f32(s.add(i + 4))));
            vst1q_f32(o.add(i + 8), vmulq_f32(vld1q_f32(o.add(i + 8)), vld1q_f32(s.add(i + 8))));
            vst1q_f32(o.add(i + 12), vmulq_f32(vld1q_f32(o.add(i + 12)), vld1q_f32(s.add(i + 12))));
            i += 16;
        }
    }
    #[target_feature(enable = "neon")]
    unsafe fn scale_n(o: *mut f32, c: f32) {
        let cv = vdupq_n_f32(c);
        let mut i = 0;
        while i < 64 {
            vst1q_f32(o.add(i), vmulq_f32(vld1q_f32(o.add(i)), cv));
            vst1q_f32(o.add(i + 4), vmulq_f32(vld1q_f32(o.add(i + 4)), cv));
            vst1q_f32(o.add(i + 8), vmulq_f32(vld1q_f32(o.add(i + 8)), cv));
            vst1q_f32(o.add(i + 12), vmulq_f32(vld1q_f32(o.add(i + 12)), cv));
            i += 16;
        }
    }
    #[target_feature(enable = "neon")]
    unsafe fn max0_n(s: *const f32) -> f32 {
        let mut m = vdupq_n_f32(0.0);
        let mut i = 0;
        while i < 64 {
            m = vmaxq_f32(m, vld1q_f32(s.add(i)));
            m = vmaxq_f32(m, vld1q_f32(s.add(i + 4)));
            m = vmaxq_f32(m, vld1q_f32(s.add(i + 8)));
            m = vmaxq_f32(m, vld1q_f32(s.add(i + 12)));
            i += 16;
        }
        let h = vmax_f32(vget_low_f32(m), vget_high_f32(m));
        let h = vpmax_f32(h, h);
        vget_lane_f32::<0>(h)
    }

    impl Kern for Neon {
        #[inline(always)]
        unsafe fn fwht(b: *mut f32) {
            fwht_n(b)
        }
        #[inline(always)]
        unsafe fn set_scaled(out: *mut f32, a: *const f32, c: f32) {
            set_scaled_n(out, a, c)
        }
        #[inline(always)]
        unsafe fn mul_assign(out: *mut f32, a: *const f32) {
            mul_assign_n(out, a)
        }
        #[inline(always)]
        unsafe fn scale(out: *mut f32, c: f32) {
            scale_n(out, c)
        }
        #[inline(always)]
        unsafe fn max0(a: *const f32) -> f32 {
            max0_n(a)
        }
    }
}

/// The kernel used by the decoder on this target.
#[cfg(target_arch = "arm")]
pub type Fast = Neon;
#[cfg(not(target_arch = "arm"))]
pub type Fast = Scalar;

/// Sequential sum of 64 values, exactly `pd.iter().sum()` (the order matters), unrolled.
#[inline(always)]
unsafe fn seq_sum(a: *const f32) -> f32 {
    let mut s = 0.0f32;
    let mut i = 0;
    while i < M {
        s += *a.add(i);
        s += *a.add(i + 1);
        s += *a.add(i + 2);
        s += *a.add(i + 3);
        s += *a.add(i + 4);
        s += *a.add(i + 5);
        s += *a.add(i + 6);
        s += *a.add(i + 7);
        i += 8;
    }
    s
}
#[inline(always)]
unsafe fn fill(o: *mut f32, c: f32) {
    for j in 0..M {
        *o.add(j) = c;
    }
}
/// `pdmath::fwdperm`: `dst[j] = src[perm[j]]` (bit copies through integer registers).
#[inline(always)]
unsafe fn fwdperm(dst: *mut f32, src: *const f32, perm: *const u8) {
    let (d, s) = (dst as *mut u32, src as *const u32);
    let mut j = 0;
    while j < M {
        *d.add(j) = *s.add(*perm.add(j) as usize);
        *d.add(j + 1) = *s.add(*perm.add(j + 1) as usize);
        *d.add(j + 2) = *s.add(*perm.add(j + 2) as usize);
        *d.add(j + 3) = *s.add(*perm.add(j + 3) as usize);
        j += 4;
    }
}
/// `pdmath::bwdperm`: `dst[perm[j]] = src[j]`.
#[inline(always)]
unsafe fn bwdperm(dst: *mut f32, src: *const f32, perm: *const u8) {
    let (d, s) = (dst as *mut u32, src as *const u32);
    let mut j = 0;
    while j < M {
        *d.add(*perm.add(j) as usize) = *s.add(j);
        *d.add(*perm.add(j + 1) as usize) = *s.add(j + 1);
        *d.add(*perm.add(j + 2) as usize) = *s.add(j + 2);
        *d.add(*perm.add(j + 3) as usize) = *s.add(j + 3);
        j += 4;
    }
}

/// Why a decode attempt produced nothing.
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum BpError {
    NotConverged,
    CrcMismatch,
}

/// A decoded frame: BP iterations, the 13 user symbols, the 63 channel symbols.
pub struct BpOk {
    pub iterations: u32,
    pub msg: [i32; 13],
    pub codeword: [i32; 63],
}

/// Rows of intrinsics the decoder takes: the 65 code symbols (15 information symbols - of which
/// symbols 13 and 14, the punctured CRC, are uniform - then the 50 check symbols).
pub const INTR_ROWS: usize = V;
/// f32 units of scratch [`Bp`] needs.
pub const BP_MEM: usize = (NMSG + NMSG - V) * M;

pub struct Bp<'a> {
    v2c: &'a mut [f32],   // NMSG rows: [0, V) extrinsic outputs, [V, NMSG) edge messages
    edges: &'a mut [f32], // check->variable messages V .. NMSG
}

/// Channel symbol (0..63) -> its row in the 65-row intrinsics block.
#[inline]
pub fn intr_row(k: usize) -> usize {
    if k < N_USER {
        k
    } else {
        k + 2
    }
}

impl<'a> Bp<'a> {
    /// Decoder over `mem` (`BP_MEM` f32 of scratch).
    pub fn new_in(mem: &'a mut [f32]) -> Self {
        let (v2c, rest) = mem.split_at_mut(NMSG * M);
        Self { v2c, edges: &mut rest[..(NMSG - V) * M] }
    }

    /// Blind decode of `intr` (`INTR_ROWS` x 64 probabilities; rows `intr_row(k)` are the channel
    /// symbols, rows 13 and 14 are set to uniform here).
    pub fn decode(&mut self, intr: &mut [f32], max_iters: u32) -> Result<BpOk, BpError> {
        self.decode_k::<Fast>(intr, max_iters)
    }

    pub fn decode_k<K: Kern>(&mut self, intr: &mut [f32], max_iters: u32) -> Result<BpOk, BpError> {
        assert!(intr.len() == INTR_ROWS * M && self.v2c.len() == NMSG * M && self.edges.len() == (NMSG - V) * M);
        for x in intr[N_USER * M..K_INFO * M].iter_mut() {
            *x = UNI;
        }
        // SAFETY: v2c holds NMSG rows, intr V rows, edges NMSG - V rows; every message index in the
        // code tables is < NMSG and every permutation entry is < 64 (checked by `tables_in_range`).
        let iterations =
            unsafe { extrinsic::<K>(self.v2c.as_mut_ptr(), intr.as_mut_ptr(), self.edges.as_mut_ptr(), max_iters) }
                .ok_or(BpError::NotConverged)?;

        // MAP decision on the information symbols (`QraCode::map_decode`)
        let mut px = [0i32; K_INFO];
        for (k, x) in px.iter_mut().enumerate() {
            let e = &self.v2c[M * k..M * k + M];
            let i = &intr[M * k..M * k + M];
            let mut cmax = 0.0f32;
            let mut imax: Option<usize> = None;
            for j in (0..M).rev() {
                let v = e[j] * i[j];
                if v >= cmax {
                    cmax = v;
                    imax = Some(j);
                }
            }
            *x = imax.ok_or(BpError::NotConverged)? as i32;
        }
        let crc = crc12(&px[..N_USER]);
        if crc[0] != px[N_USER] || crc[1] != px[N_USER + 1] {
            return Err(BpError::CrcMismatch);
        }
        let mut py = [0i32; V];
        encode(&px, &mut py);
        let mut msg = [0i32; 13];
        msg.copy_from_slice(&px[..N_USER]);
        let mut codeword = [0i32; 63];
        codeword[..N_USER].copy_from_slice(&py[..N_USER]);
        codeword[N_USER..].copy_from_slice(&py[K_INFO..]);
        Ok(BpOk { iterations, msg, codeword })
    }

    /// FNV-1a over the bits of the message arrays (self-test / cross-target comparison).
    pub fn state_hash(&self) -> u32 {
        let mut h = 0x811c_9dc5u32;
        for a in [&self.v2c[..], &self.edges[..]] {
            for &x in a.iter() {
                h = (h ^ x.to_bits()).wrapping_mul(0x0100_0193);
            }
        }
        h
    }
}

/// `QraCode::extrinsic`; the check->variable array is split into `intr` (rows < V, the intrinsics,
/// never written) and `edges` (rows >= V).  `Some(iterations)` when converged.
/// Same operations in the same order as stock; a message whose edge weight is 0 is computed
/// straight into its destination row instead of through a temporary + copy.
#[cfg_attr(feature = "radio", optimize(speed))]
unsafe fn extrinsic<K: Kern>(v2c: *mut f32, intr: *mut f32, edges: *mut f32, maxiter: u32) -> Option<u32> {
    let mut tmp = core::mem::MaybeUninit::<[f32; M]>::uninit();
    let tmp = tmp.as_mut_ptr() as *mut f32;
    let rowp = |a: *mut f32, i: usize| a.add(M * i);
    // check->variable message row i
    let c2vp = |i: usize| if i < V { intr.add(M * i) } else { edges.add(M * (i - V)) };

    for nv in 0..V {
        let ndeg = VDEG[nv] as usize;
        for k in 1..ndeg {
            let imsg = V2CM[nv * MAXVDEG + k] as usize;
            core::ptr::copy_nonoverlapping(c2vp(nv) as *const f32, rowp(v2c, imsg), M);
        }
    }

    for nit in 0..maxiter {
        // check nodes
        for nc in V..C {
            let ndeg = CDEG[nc] as usize;
            if ndeg == 1 {
                return None;
            }
            let idx = &C2VM[nc * MAXCDEG..nc * MAXCDEG + MAXCDEG];
            for k in 0..ndeg {
                K::fwht(rowp(v2c, idx[k] as usize));
            }
            for k in 0..ndeg {
                let imsg = idx[k] as usize;
                let wmsg = MSGW[imsg] as usize;
                let dst = c2vp(imsg);
                let out = if wmsg == 0 { dst } else { tmp };
                let mut first = true;
                for kk in 0..ndeg {
                    if kk != k {
                        let src = rowp(v2c, idx[kk] as usize);
                        if first {
                            K::set_scaled(out, src, UNI);
                            first = false;
                        } else {
                            K::mul_assign(out, src);
                        }
                    }
                }
                *out += 1e-7;
                K::fwht(out);
                if wmsg != 0 {
                    bwdperm(dst, out, PERM.as_ptr().add(wmsg * M));
                }
            }
        }
        // variable nodes
        for nv in 0..V {
            let ndeg = VDEG[nv] as usize;
            let idx = &V2CM[nv * MAXVDEG..nv * MAXVDEG + MAXVDEG];
            for k in 0..ndeg {
                let imsg = idx[k] as usize;
                let wmsg = MSGW[imsg] as usize;
                let dst = rowp(v2c, imsg);
                let out = if wmsg == 0 { dst } else { tmp };
                let mut first = true;
                for kk in 0..ndeg {
                    if kk != k {
                        let src = c2vp(idx[kk] as usize);
                        if first {
                            K::set_scaled(out, src, UNI);
                            first = false;
                        } else {
                            K::mul_assign(out, src);
                        }
                    }
                }
                if first {
                    fill(out, UNI);
                }
                // pdmath::norm
                let sum = seq_sum(out);
                if sum <= 0.0 {
                    fill(out, UNI);
                } else {
                    K::scale(out, 1.0 / sum);
                }
                if wmsg != 0 {
                    fwdperm(dst, out, PERM.as_ptr().add(wmsg * M));
                }
            }
        }
        // convergence
        let mut totex = 0.0f32;
        for nv in 0..V {
            totex += K::max0(rowp(v2c, nv));
        }
        if totex > V as f32 - 0.01 {
            return Some(nit);
        }
    }
    None
}

/// Deterministic test vectors: a random valid frame through a noisy 64-FSK channel model, turned
/// into 63 rows of channel intrinsics; `level` scales the noise (around 1.0 = near the threshold).
pub fn test_vector(seed: u32, level: f32, intr: &mut [f32]) {
    let mut s = seed.wrapping_mul(2654435761).wrapping_add(12345);
    let mut rnd = move || {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        s
    };
    let mut px = [0i32; 15];
    for x in px[..13].iter_mut() {
        *x = (rnd() % 64) as i32;
    }
    let crc = crc12(&px[..13]);
    px[13] = crc[0];
    px[14] = crc[1];
    let mut py = [0i32; 65];
    encode(&px, &mut py);
    for k in 0..63 {
        let sym = if k < 13 { py[k] } else { py[k + 2] } as usize;
        let r = &mut intr[M * k..M * (k + 1)];
        let mut sum = 0.0f32;
        for (j, o) in r.iter_mut().enumerate() {
            // pseudo log-likelihoods: uniform noise, the sent tone favoured by 1/level
            let u = (rnd() >> 8) as f32 / 16_777_216.0;
            let mut ll = 6.0 * u * u;
            if j == sym {
                ll += 3.2 / level;
            }
            let e = num_traits::Float::exp(ll);
            *o = e;
            sum += e;
        }
        for o in r.iter_mut() {
            *o /= sum;
        }
    }
}

/// Spread 63 channel rows into the 65-row block [`Bp::decode`] takes.
pub fn lay_out(intr63: &[f32], intr65: &mut [f32]) {
    for k in 0..63 {
        let r = intr_row(k);
        intr65[M * r..M * r + M].copy_from_slice(&intr63[M * k..M * k + M]);
    }
}

/// ARM self-test: NEON and scalar kernels must give the same bits.  Returns
/// `(mismatches, decoded, hash)` over `n` vectors.
#[cfg(target_arch = "arm")]
pub fn selftest(n: u32) -> (u32, u32, u32) {
    let mut ma = alloc::vec![0.0f32; BP_MEM];
    let mut mb = alloc::vec![0.0f32; BP_MEM];
    let mut t = alloc::vec![0.0f32; 63 * M];
    let mut ia = alloc::vec![0.0f32; INTR_ROWS * M];
    let mut ib = alloc::vec![0.0f32; INTR_ROWS * M];
    let mut a = Bp::new_in(&mut ma);
    let mut b = Bp::new_in(&mut mb);
    let (mut bad, mut ok, mut hh) = (0u32, 0u32, 0u32);
    for i in 0..n {
        let level = 0.8 + 0.05 * (i % 12) as f32;
        test_vector(i, level, &mut t);
        lay_out(&t, &mut ia);
        lay_out(&t, &mut ib);
        let ra = a.decode_k::<Neon>(&mut ia, 50);
        let rb = b.decode_k::<Scalar>(&mut ib, 50);
        let same = match (&ra, &rb) {
            (Ok(x), Ok(y)) => x.iterations == y.iterations && x.msg == y.msg && x.codeword == y.codeword,
            (Err(x), Err(y)) => x == y,
            _ => false,
        };
        if !same || a.state_hash() != b.state_hash() {
            bad += 1;
        }
        if ra.is_ok() {
            ok += 1;
        }
        hh = hh.wrapping_mul(31).wrapping_add(a.state_hash());
    }
    (bad, ok, hh)
}

#[cfg(all(test, feature = "std"))]
mod tests {
    use super::*;
    use mfsk_core::fec::qra::Q65Codec;

    #[test]
    fn tables_in_range() {
        let c = &QRA15_65_64_IRR_E23;
        assert!(PERM.iter().all(|&p| (p as usize) < M));
        assert!(MSGW.iter().all(|&w| (w as usize) < 63));
        for nv in 0..V {
            assert!(VDEG[nv] as usize <= MAXVDEG && VDEG[nv] >= 1);
            assert_eq!(V2CM[nv * MAXVDEG] as usize, nv);
            for k in 0..VDEG[nv] as usize {
                assert!((V2CM[nv * MAXVDEG + k] as usize) < NMSG);
            }
        }
        for nc in V..C {
            assert!(CDEG[nc] as usize <= MAXCDEG && CDEG[nc] >= 2);
            for k in 0..CDEG[nc] as usize {
                assert!((C2VM[nc * MAXCDEG + k] as usize) < NMSG);
                assert!((C2VM[nc * MAXCDEG + k] as usize) >= V, "a check node writes an intrinsic row");
            }
        }
        // our encoder == stock
        let mut s = 7u32;
        for _ in 0..200 {
            let mut x = [0i32; K_INFO];
            for v in x.iter_mut() {
                s = s.wrapping_mul(1664525).wrapping_add(1013904223);
                *v = (s >> 26) as i32;
            }
            let mut y = [0i32; V];
            encode(&x, &mut y);
            let mut y2 = vec![0i32; V];
            c.encode(&x, &mut y2);
            assert_eq!(&y[..], &y2[..]);
        }
    }

    /// Bit-identical to stock `Q65Codec::decode` (iterations, message, error kind) on vectors from
    /// easy to undecodable.
    #[test]
    fn matches_stock_codec() {
        let mut stock = Q65Codec::new(&QRA15_65_64_IRR_E23);
        let mut mem = vec![0.0f32; BP_MEM];
        let mut bp = Bp::new_in(&mut mem);
        let mut i65 = vec![0.0f32; INTR_ROWS * M];
        let mut intr = vec![0.0f32; 63 * M];
        let (mut nok, mut nfail) = (0, 0);
        for i in 0..600u32 {
            let level = 0.7 + 0.05 * (i % 14) as f32;
            test_vector(i, level, &mut intr);
            let mut msg = [0i32; 13];
            let want = stock.decode(&intr, &mut msg, 50);
            lay_out(&intr, &mut i65);
            let got = bp.decode(&mut i65, 50);
            match (want, got) {
                (Ok(it), Ok(g)) => {
                    assert_eq!(it, g.iterations, "vector {i}");
                    assert_eq!(msg, g.msg, "vector {i}");
                    let mut cw = [0i32; 63];
                    stock.encode(&msg, &mut cw);
                    assert_eq!(cw, g.codeword, "vector {i}");
                    nok += 1;
                }
                (Err(e), Err(g)) => {
                    let same = matches!(
                        (e, g),
                        (mfsk_core::fec::qra::Q65DecodeError::NotConverged, BpError::NotConverged)
                            | (mfsk_core::fec::qra::Q65DecodeError::CrcMismatch, BpError::CrcMismatch)
                    );
                    assert!(same, "vector {i}: {e:?} vs {g:?}");
                    nfail += 1;
                }
                (w, g) => panic!("vector {i}: stock {w:?} vs ours ok={}", g.is_ok()),
            }
        }
        // the vector set must exercise both outcomes
        assert!(nok > 100 && nfail > 100, "ok {nok} fail {nfail}");
    }
}
