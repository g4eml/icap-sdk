// SPDX-License-Identifier: GPL-3.0-or-later
//! Real-input DFT of one symbol window (`n` = nsps samples) by a mixed-radix (5/3/2/4) complex FFT of
//! half the length (the even/odd-sample trick), single precision.
//!
//! Replaces the per-bin Goertzel bank: one transform gives every bin of the window, so a sync row
//! (721 bins at 8 kHz) costs one FFT instead of `bins x nsps` resonator steps (~12 M instructions).
//! `powers()` returns the same unnormalised `|X[k]|^2` of the same `n`-point DFT (samples scaled to
//! +-1.0), so everything downstream is unchanged.
//!
//! The butterflies are written once, generic over a "lane" type: plain `f32`, or four lanes of NEON
//! on the Cortex-A9 (stages whose sub-length is a multiple of 4, i.e. all but the last for 8/12 kHz;
//! the last, twiddle-free radix-4 stage has its own NEON form).  Every lane performs the same IEEE
//! single-precision operations in the same order, so the ARM and the x86 builds give bit-identical
//! spectra.
//!
//! Memory (n = 4800), all borrowed from the caller (the decoder's arena): work 2400 complex
//! (19.2 KB), per-stage twiddles (19.2 KB), input permutation u16 (4.8 KB), bin twiddles (0.8 KB).

const MAXFAC: usize = 12;
const SC: f32 = 1.0 / 32768.0;

pub struct RealFft<'a> {
    n: usize,
    m: usize,
    fac: [(u16, u16); MAXFAC], // (radix, sub-length), top-down as in kiss_fft
    off: [u32; MAXFAC],        // start of each stage's twiddles in `tw`
    nfac: usize,
    tw: &'a mut [f32],   // per stage, per q = 1..radix: re[sub-length], im[sub-length] of exp(-2*pi*i*k*q*stride/m)
    bt: &'a mut [f32],   // bin twiddles exp(-2*pi*i*k/n) = A[k >> 6] * B[k & 63], (re, im) pairs
    perm: &'a mut [u16], // work[j] = z[perm[j]]
    work: &'a mut [f32], // m complex (re, im)
    epi3: f32,           // im exp(-2*pi*i/3)
    ya: (f32, f32),      // exp(-2*pi*i/5)
    yb: (f32, f32),      // exp(-4*pi*i/5)
}

/// (sin x, cos x) for x in [0, pi/4], f64 Taylor (error < 1e-12): no libm trig in the app window.
fn sincos_q(x: f64) -> (f64, f64) {
    let x2 = x * x;
    let s = x
        * (1.0
            - x2 * (1.0 / 6.0
                - x2 * (1.0 / 120.0
                    - x2 * (1.0 / 5040.0 - x2 * (1.0 / 362880.0 - x2 * (1.0 / 39916800.0 - x2 * (1.0 / 6227020800.0)))))));
    let c = 1.0
        - x2 * (1.0 / 2.0
            - x2 * (1.0 / 24.0
                - x2 * (1.0 / 720.0 - x2 * (1.0 / 40320.0 - x2 * (1.0 / 3628800.0 - x2 * (1.0 / 479001600.0))))));
    (s, c)
}

/// exp(-2*pi*i*num/den) as (re, im), any `num` (reduced mod `den`).
fn root(num: usize, den: usize) -> (f32, f32) {
    use core::f64::consts::PI;
    let num = num % den;
    let (num, lower) = if 2 * num > den { (den - num, true) } else { (num, false) }; // angle in [0, pi]
    let t = 2.0 * PI * (num as f64) / (den as f64);
    let (t1, neg) = if t > PI / 2.0 { (PI - t, true) } else { (t, false) };
    let (s, c) = if t1 > PI / 4.0 {
        let (s, c) = sincos_q(PI / 2.0 - t1);
        (c, s)
    } else {
        sincos_q(t1)
    };
    let c = if neg { -c } else { c };
    (c as f32, if lower { s as f32 } else { -(s as f32) })
}

fn build_perm(perm: &mut [u16], out_base: usize, in_off: usize, fstride: usize, fac: &[(u16, u16)]) {
    let (p, m) = (fac[0].0 as usize, fac[0].1 as usize);
    if m == 1 {
        for i in 0..p {
            perm[out_base + i] = (in_off + i * fstride) as u16;
        }
    } else {
        for i in 0..p {
            build_perm(perm, out_base + i * m, in_off + i * fstride, fstride * p, &fac[1..]);
        }
    }
}

// ── lanes ────────────────────────────────────────────────────────────────────────────────────────
/// One (`f32`) or four (NEON) values the butterflies compute on.
trait Lane: Copy {
    const W: usize;
    /// `W` complex values from interleaved (re, im) storage.
    unsafe fn ld2(p: *const f32) -> (Self, Self);
    unsafe fn st2(p: *mut f32, re: Self, im: Self);
    /// `W` consecutive f32.
    unsafe fn ld(p: *const f32) -> Self;
    fn splat(x: f32) -> Self;
    fn add(self, o: Self) -> Self;
    fn sub(self, o: Self) -> Self;
    fn mul(self, o: Self) -> Self;
}
impl Lane for f32 {
    const W: usize = 1;
    #[inline(always)]
    unsafe fn ld2(p: *const f32) -> (f32, f32) {
        (*p, *p.add(1))
    }
    #[inline(always)]
    unsafe fn st2(p: *mut f32, re: f32, im: f32) {
        *p = re;
        *p.add(1) = im;
    }
    #[inline(always)]
    unsafe fn ld(p: *const f32) -> f32 {
        *p
    }
    #[inline(always)]
    fn splat(x: f32) -> f32 {
        x
    }
    #[inline(always)]
    fn add(self, o: f32) -> f32 {
        self + o
    }
    #[inline(always)]
    fn sub(self, o: f32) -> f32 {
        self - o
    }
    #[inline(always)]
    fn mul(self, o: f32) -> f32 {
        self * o
    }
}
#[cfg(target_arch = "arm")]
mod neon {
    use super::Lane;
    use core::arch::arm::*;
    #[derive(Copy, Clone)]
    pub struct V4(pub float32x4_t);
    impl Lane for V4 {
        const W: usize = 4;
        #[inline(always)]
        unsafe fn ld2(p: *const f32) -> (V4, V4) {
            let v = vld2q_f32(p);
            (V4(v.0), V4(v.1))
        }
        #[inline(always)]
        unsafe fn st2(p: *mut f32, re: V4, im: V4) {
            vst2q_f32(p, float32x4x2_t(re.0, im.0));
        }
        #[inline(always)]
        unsafe fn ld(p: *const f32) -> V4 {
            V4(vld1q_f32(p))
        }
        #[inline(always)]
        fn splat(x: f32) -> V4 {
            unsafe { V4(vdupq_n_f32(x)) }
        }
        #[inline(always)]
        fn add(self, o: V4) -> V4 {
            unsafe { V4(vaddq_f32(self.0, o.0)) }
        }
        #[inline(always)]
        fn sub(self, o: V4) -> V4 {
            unsafe { V4(vsubq_f32(self.0, o.0)) }
        }
        #[inline(always)]
        fn mul(self, o: V4) -> V4 {
            unsafe { V4(vmulq_f32(self.0, o.0)) }
        }
    }

    /// The last stage for radix 4, sub-length 1 (no twiddles): `nblk` blocks of 4 complex, two
    /// blocks per step; the same sums and differences as `super::bfly4_m1`.
    #[target_feature(enable = "neon")]
    pub unsafe fn bfly4_m1(f: *mut f32, nblk: usize) {
        let mut b = 0;
        while b + 2 <= nblk {
            let p = f.add(8 * b);
            let v = vld4q_f32(p); // .0 = [r0 r2 r0' r2'] .1 = [i0 i2 ..] .2 = [r1 r3 ..] .3 = [i1 i3 ..]
            let (x0, x1, x2, x3) = (vrev64q_f32(v.0), vrev64q_f32(v.1), vrev64q_f32(v.2), vrev64q_f32(v.3));
            let (s02r, d02r) = (vaddq_f32(v.0, x0), vsubq_f32(v.0, x0)); // lanes 0, 2 are valid
            let (s02i, d02i) = (vaddq_f32(v.1, x1), vsubq_f32(v.1, x1));
            let (s13r, d13r) = (vaddq_f32(v.2, x2), vsubq_f32(v.2, x2));
            let (s13i, d13i) = (vaddq_f32(v.3, x3), vsubq_f32(v.3, x3));
            let (f0r, f0i) = (vaddq_f32(s02r, s13r), vaddq_f32(s02i, s13i));
            let (f2r, f2i) = (vsubq_f32(s02r, s13r), vsubq_f32(s02i, s13i));
            let (f1r, f1i) = (vaddq_f32(d02r, d13i), vsubq_f32(d02i, d13r));
            let (f3r, f3i) = (vsubq_f32(d02r, d13i), vaddq_f32(d02i, d13r));
            let o = float32x4x4_t(vtrnq_f32(f0r, f2r).0, vtrnq_f32(f0i, f2i).0, vtrnq_f32(f1r, f3r).0, vtrnq_f32(f1i, f3i).0);
            vst4q_f32(p, o);
            b += 2;
        }
        if b < nblk {
            super::bfly4_m1(f.add(8 * b), nblk - b);
        }
    }

    /// `w[i] = (w[i] as i32 bits) as f32 * SC`, in place.
    #[target_feature(enable = "neon")]
    pub unsafe fn int_to_f32(w: *mut f32, n: usize) {
        let sc = vdupq_n_f32(super::SC);
        let mut i = 0;
        while i + 4 <= n {
            let v = vld1q_s32(w.add(i) as *const i32);
            vst1q_f32(w.add(i), vmulq_f32(vcvtq_f32_s32(v), sc));
            i += 4;
        }
        while i < n {
            *w.add(i) = *(w.add(i) as *const i32) as f32 * super::SC;
            i += 1;
        }
    }
}

#[inline(always)]
fn cm<L: Lane>(a: (L, L), b: (L, L)) -> (L, L) {
    (a.0.mul(b.0).sub(a.1.mul(b.1)), a.0.mul(b.1).add(a.1.mul(b.0)))
}
/// twiddle q (1-based) of a stage: re at `t + (q-1)*2*mi`, im `mi` further
#[inline(always)]
unsafe fn tq<L: Lane>(t: *const f32, mi: usize, q: usize, k: usize) -> (L, L) {
    let p = t.add((q - 1) * 2 * mi + k);
    (L::ld(p), L::ld(p.add(mi)))
}

#[inline(always)]
unsafe fn bfly2<L: Lane>(f: *mut f32, t: *const f32, mi: usize) {
    let mut k = 0;
    while k < mi {
        let a = L::ld2(f.add(2 * k));
        let x = cm(L::ld2(f.add(2 * (k + mi))), tq::<L>(t, mi, 1, k));
        L::st2(f.add(2 * (k + mi)), a.0.sub(x.0), a.1.sub(x.1));
        L::st2(f.add(2 * k), a.0.add(x.0), a.1.add(x.1));
        k += L::W;
    }
}

#[inline(always)]
unsafe fn bfly4<L: Lane>(f: *mut f32, t: *const f32, mi: usize) {
    let mut k = 0;
    while k < mi {
        let s0 = cm(L::ld2(f.add(2 * (k + mi))), tq::<L>(t, mi, 1, k));
        let s1 = cm(L::ld2(f.add(2 * (k + 2 * mi))), tq::<L>(t, mi, 2, k));
        let s2 = cm(L::ld2(f.add(2 * (k + 3 * mi))), tq::<L>(t, mi, 3, k));
        let a = L::ld2(f.add(2 * k));
        let s5 = (a.0.sub(s1.0), a.1.sub(s1.1));
        let a = (a.0.add(s1.0), a.1.add(s1.1));
        let s3 = (s0.0.add(s2.0), s0.1.add(s2.1));
        let s4 = (s0.0.sub(s2.0), s0.1.sub(s2.1));
        L::st2(f.add(2 * (k + 2 * mi)), a.0.sub(s3.0), a.1.sub(s3.1));
        L::st2(f.add(2 * k), a.0.add(s3.0), a.1.add(s3.1));
        L::st2(f.add(2 * (k + mi)), s5.0.add(s4.1), s5.1.sub(s4.0));
        L::st2(f.add(2 * (k + 3 * mi)), s5.0.sub(s4.1), s5.1.add(s4.0));
        k += L::W;
    }
}

/// radix 4, sub-length 1: `nblk` consecutive blocks of 4 complex, no twiddles.
unsafe fn bfly4_m1(f: *mut f32, nblk: usize) {
    for b in 0..nblk {
        let p = f.add(8 * b);
        let (r0, i0, r1, i1) = (*p, *p.add(1), *p.add(2), *p.add(3));
        let (r2, i2, r3, i3) = (*p.add(4), *p.add(5), *p.add(6), *p.add(7));
        let (s02r, d02r, s02i, d02i) = (r0 + r2, r0 - r2, i0 + i2, i0 - i2);
        let (s13r, d13r, s13i, d13i) = (r1 + r3, r1 - r3, i1 + i3, i1 - i3);
        *p = s02r + s13r;
        *p.add(1) = s02i + s13i;
        *p.add(2) = d02r + d13i;
        *p.add(3) = d02i - d13r;
        *p.add(4) = s02r - s13r;
        *p.add(5) = s02i - s13i;
        *p.add(6) = d02r - d13i;
        *p.add(7) = d02i + d13r;
    }
}

#[inline(always)]
unsafe fn bfly3<L: Lane>(f: *mut f32, t: *const f32, mi: usize, epi3: f32) {
    let e = L::splat(epi3);
    let h = L::splat(0.5);
    let mut k = 0;
    while k < mi {
        let s1 = cm(L::ld2(f.add(2 * (k + mi))), tq::<L>(t, mi, 1, k));
        let s2 = cm(L::ld2(f.add(2 * (k + 2 * mi))), tq::<L>(t, mi, 2, k));
        let s3 = (s1.0.add(s2.0), s1.1.add(s2.1));
        let s0 = (s1.0.sub(s2.0), s1.1.sub(s2.1));
        let a = L::ld2(f.add(2 * k));
        let b = (a.0.sub(h.mul(s3.0)), a.1.sub(h.mul(s3.1)));
        let s0 = (s0.0.mul(e), s0.1.mul(e));
        L::st2(f.add(2 * k), a.0.add(s3.0), a.1.add(s3.1));
        L::st2(f.add(2 * (k + 2 * mi)), b.0.add(s0.1), b.1.sub(s0.0));
        L::st2(f.add(2 * (k + mi)), b.0.sub(s0.1), b.1.add(s0.0));
        k += L::W;
    }
}

#[inline(always)]
unsafe fn bfly5<L: Lane>(f: *mut f32, t: *const f32, mi: usize, ya: (f32, f32), yb: (f32, f32)) {
    let (yar, yai, ybr, ybi) = (L::splat(ya.0), L::splat(ya.1), L::splat(yb.0), L::splat(yb.1));
    let mut u = 0;
    while u < mi {
        let s0 = L::ld2(f.add(2 * u));
        let s1 = cm(L::ld2(f.add(2 * (u + mi))), tq::<L>(t, mi, 1, u));
        let s2 = cm(L::ld2(f.add(2 * (u + 2 * mi))), tq::<L>(t, mi, 2, u));
        let s3 = cm(L::ld2(f.add(2 * (u + 3 * mi))), tq::<L>(t, mi, 3, u));
        let s4 = cm(L::ld2(f.add(2 * (u + 4 * mi))), tq::<L>(t, mi, 4, u));
        let s7 = (s1.0.add(s4.0), s1.1.add(s4.1));
        let s10 = (s1.0.sub(s4.0), s1.1.sub(s4.1));
        let s8 = (s2.0.add(s3.0), s2.1.add(s3.1));
        let s9 = (s2.0.sub(s3.0), s2.1.sub(s3.1));
        L::st2(f.add(2 * u), s0.0.add(s7.0.add(s8.0)), s0.1.add(s7.1.add(s8.1)));
        let s5 = (s0.0.add(s7.0.mul(yar).add(s8.0.mul(ybr))), s0.1.add(s7.1.mul(yar).add(s8.1.mul(ybr))));
        let s6 = (s10.1.mul(yai).add(s9.1.mul(ybi)), L::splat(0.0).sub(s10.0.mul(yai)).sub(s9.0.mul(ybi)));
        L::st2(f.add(2 * (u + mi)), s5.0.sub(s6.0), s5.1.sub(s6.1));
        L::st2(f.add(2 * (u + 4 * mi)), s5.0.add(s6.0), s5.1.add(s6.1));
        let s11 = (s0.0.add(s7.0.mul(ybr).add(s8.0.mul(yar))), s0.1.add(s7.1.mul(ybr).add(s8.1.mul(yar))));
        let s12 = (s9.1.mul(yai).sub(s10.1.mul(ybi)), s10.0.mul(ybi).sub(s9.0.mul(yai)));
        L::st2(f.add(2 * (u + 2 * mi)), s11.0.add(s12.0), s11.1.add(s12.1));
        L::st2(f.add(2 * (u + 3 * mi)), s11.0.sub(s12.0), s11.1.sub(s12.1));
        u += L::W;
    }
}

/// One stage over all its blocks, with lane type `L`.
#[cfg_attr(feature = "radio", optimize(speed))]
#[allow(clippy::too_many_arguments)]
unsafe fn stage<L: Lane>(work: *mut f32, t: *const f32, p: usize, mi: usize, nblk: usize, epi3: f32, ya: (f32, f32), yb: (f32, f32)) {
    for blk in 0..nblk {
        let f = work.add(2 * blk * p * mi);
        match p {
            2 => bfly2::<L>(f, t, mi),
            3 => bfly3::<L>(f, t, mi, epi3),
            4 => bfly4::<L>(f, t, mi),
            _ => bfly5::<L>(f, t, mi, ya, yb),
        }
    }
}

impl<'a> RealFft<'a> {
    /// f32 units of caller memory a transform of `n` real samples needs.
    pub const fn mem_len(n: usize) -> usize {
        // work (n) + stage twiddles (< n) + permutation (n/2 u16) + bin twiddles
        n + n + n / 4 + 2 * (((n / 2) >> 6) + 1 + 64)
    }

    /// `n` real samples per transform, tables and work area in `mem` (`mem_len(n)` f32);
    /// `None` if `n` is not a multiple of 8, too long or `n/2` has a prime factor > 5.
    pub fn new_in(n: usize, mem: &'a mut [f32]) -> Option<Self> {
        if n < 16 || n % 8 != 0 || n / 2 > 65_535 || mem.len() < Self::mem_len(n) {
            return None;
        }
        let m = n / 2;
        // factors top-down: 5s, 3s, one 2 (odd power of two), 4s - so every stage but the last
        // has a sub-length that is a multiple of 4
        let (mut n5, mut n3, mut n2) = (0usize, 0usize, 0usize);
        let mut rem = m;
        while rem % 5 == 0 {
            rem /= 5;
            n5 += 1;
        }
        while rem % 3 == 0 {
            rem /= 3;
            n3 += 1;
        }
        while rem % 2 == 0 {
            rem /= 2;
            n2 += 1;
        }
        if rem != 1 || n5 + n3 + (n2 + 1) / 2 > MAXFAC {
            return None;
        }
        let mut fac = [(0u16, 0u16); MAXFAC];
        let mut off = [0u32; MAXFAC];
        let mut nfac = 0usize;
        let mut rem = m;
        for i in 0..n5 + n3 + n2 % 2 + n2 / 2 {
            let p = if i < n5 {
                5
            } else if i < n5 + n3 {
                3
            } else if i < n5 + n3 + n2 % 2 {
                2
            } else {
                4
            };
            rem /= p;
            fac[nfac] = (p as u16, rem as u16);
            nfac += 1;
        }

        let (work, rest) = mem.split_at_mut(n);
        let (tw, rest) = rest.split_at_mut(n);
        let (pm, rest) = rest.split_at_mut(n / 4);
        let ha = (m >> 6) + 1;
        let bt = &mut rest[..2 * (ha + 64)];
        // SAFETY: n/4 f32 hold m u16; f32 alignment satisfies u16; the region is exclusively ours.
        let perm = unsafe { core::slice::from_raw_parts_mut(pm.as_mut_ptr() as *mut u16, m) };

        // per-stage twiddles
        let mut o = 0usize;
        let mut fs = 1usize;
        for i in 0..nfac {
            let (p, mi) = (fac[i].0 as usize, fac[i].1 as usize);
            off[i] = o as u32;
            if !(p == 4 && mi == 1) {
                if o + 2 * (p - 1) * mi > tw.len() {
                    return None;
                }
                for q in 1..p {
                    for k in 0..mi {
                        let (re, im) = root(k * fs * q, m);
                        tw[o + k] = re;
                        tw[o + mi + k] = im;
                    }
                    o += 2 * mi;
                }
            }
            fs *= p;
        }
        for j in 0..ha {
            let (re, im) = root(j << 6, n);
            bt[2 * j] = re;
            bt[2 * j + 1] = im;
        }
        for j in 0..64 {
            let (re, im) = root(j, n);
            bt[2 * (ha + j)] = re;
            bt[2 * (ha + j) + 1] = im;
        }
        build_perm(perm, 0, 0, 1, &fac[..nfac]);
        Some(Self { n, m, fac, off, nfac, tw, bt, perm, work, epi3: root(1, 3).1, ya: root(1, 5), yb: root(2, 5) })
    }

    /// The work area, free for the caller between transforms (`n` f32; `forward` overwrites it).
    #[inline]
    pub fn scratch(&mut self) -> &mut [f32] {
        self.work
    }

    #[inline]
    pub fn len(&self) -> usize {
        self.n
    }

    /// Transform `win` (`n` samples, s16; scaled by 1/32768 like the old f32 path).
    #[cfg_attr(feature = "radio", optimize(speed))]
    pub fn forward(&mut self, win: &[i16]) {
        assert!(win.len() == self.n);
        let m = self.m;
        let w = self.work.as_mut_ptr();
        // SAFETY: perm[j] < m, win.len() == 2*m, work.len() == 2*m; the blocks of a stage tile `work`.
        unsafe {
            // permuted load as integers, then one conversion pass
            let wi = w as *mut i32;
            let x = win.as_ptr();
            let pm = self.perm.as_ptr();
            for j in 0..m {
                let i = *pm.add(j) as usize;
                *wi.add(2 * j) = *x.add(2 * i) as i32;
                *wi.add(2 * j + 1) = *x.add(2 * i + 1) as i32;
            }
            #[cfg(target_arch = "arm")]
            neon::int_to_f32(w, 2 * m);
            #[cfg(not(target_arch = "arm"))]
            for i in 0..2 * m {
                *w.add(i) = *(wi.add(i)) as f32 * SC;
            }

            for i in (0..self.nfac).rev() {
                let (p, mi) = (self.fac[i].0 as usize, self.fac[i].1 as usize);
                let nblk = m / (p * mi);
                let t = self.tw.as_ptr().add(self.off[i] as usize);
                if p == 4 && mi == 1 {
                    #[cfg(target_arch = "arm")]
                    neon::bfly4_m1(w, nblk);
                    #[cfg(not(target_arch = "arm"))]
                    bfly4_m1(w, nblk);
                    continue;
                }
                #[cfg(target_arch = "arm")]
                if mi % 4 == 0 {
                    stage::<neon::V4>(w, t, p, mi, nblk, self.epi3, self.ya, self.yb);
                    continue;
                }
                stage::<f32>(w, t, p, mi, nblk, self.epi3, self.ya, self.yb);
            }
        }
    }

    /// `X[k]` of the last transform (0 <= k < n/2).
    #[inline(always)]
    fn bin(&self, k: usize) -> (f32, f32) {
        let m = self.m;
        debug_assert!(k < m);
        // SAFETY: k < m, (m-k)%m < m, k >> 6 <= m >> 6, table sizes as built in `new_in`.
        unsafe {
            let w = self.work.as_ptr();
            let (ar, ai) = (*w.add(2 * k), *w.add(2 * k + 1));
            let kb = if k == 0 { 0 } else { m - k };
            let (br, bi) = (*w.add(2 * kb), *w.add(2 * kb + 1));
            let t = self.bt.as_ptr();
            let ha = (m >> 6) + 1;
            let (hr, hi) = (*t.add(2 * (k >> 6)), *t.add(2 * (k >> 6) + 1));
            let (lr, li) = (*t.add(2 * (ha + (k & 63))), *t.add(2 * (ha + (k & 63)) + 1));
            let wr = hr * lr - hi * li;
            let wi = hr * li + hi * lr;
            // even samples E = (a + conj b)/2, odd samples O = (a - conj b)/(2i); X = E + w*O
            let er = 0.5 * (ar + br);
            let ei = 0.5 * (ai - bi);
            let or = 0.5 * (ai + bi);
            let oi = -0.5 * (ar - br);
            (er + (wr * or - wi * oi), ei + (wr * oi + wi * or))
        }
    }

    /// `|X[k]|^2` for one bin.
    #[inline(never)]
    pub fn power(&self, k: usize) -> f32 {
        if k >= self.m {
            return 0.0;
        }
        let (r, i) = self.bin(k);
        r * r + i * i
    }

    /// `|X[k]|^2` for `k = k0 .. k0 + out.len()` (bins at or above n/2 read as 0).
    #[cfg_attr(feature = "radio", optimize(speed))]
    pub fn powers(&self, k0: usize, out: &mut [f32]) {
        for (j, o) in out.iter_mut().enumerate() {
            let k = k0 + j;
            *o = if k < self.m {
                let (r, i) = self.bin(k);
                r * r + i * i
            } else {
                0.0
            };
        }
    }
}

#[cfg(all(test, feature = "std"))]
mod tests {
    use super::*;

    fn naive(win: &[i16], k: usize) -> f64 {
        let n = win.len();
        let (mut re, mut im) = (0.0f64, 0.0f64);
        for (i, &x) in win.iter().enumerate() {
            let a = -2.0 * std::f64::consts::PI * (k as f64) * (i as f64) / (n as f64);
            re += x as f64 / 32768.0 * a.cos();
            im += x as f64 / 32768.0 * a.sin();
        }
        re * re + im * im
    }

    fn check(n: usize) {
        let mut mem = vec![0.0f32; RealFft::mem_len(n)];
        let mut f = RealFft::new_in(n, &mut mem).expect("factorable");
        let mut s = 0x1234_5678u32;
        let win: Vec<i16> = (0..n)
            .map(|i| {
                s = s.wrapping_mul(1664525).wrapping_add(1013904223);
                let noise = ((s >> 16) as i32 - 32768) / 8;
                let tone = (3000.0 * (2.0 * std::f64::consts::PI * 997.3 * i as f64 / 8000.0).sin()) as i32;
                (noise + tone) as i16
            })
            .collect();
        f.forward(&win);
        let mut worst = 0.0f64;
        for k in (0..n / 2).step_by(7).chain([0, 1, 2, n / 2 - 1, n / 2 - 2, 600, 601, 1080]) {
            let want = naive(&win, k);
            let got = f.power(k) as f64;
            let rel = (got - want).abs() / want.max(1e-3);
            if rel > worst {
                worst = rel;
            }
        }
        assert!(worst < 2e-4, "n={n} worst rel err {worst}");
    }

    #[test]
    fn matches_naive_dft_4800() {
        check(4800);
    }
    #[test]
    fn matches_naive_dft_7200() {
        check(7200);
    }
    #[test]
    fn matches_naive_dft_other_shapes() {
        for n in [16, 48, 80, 96, 240, 1200, 2160] {
            let mut mem = vec![0.0f32; RealFft::mem_len(n)];
            let mut f = RealFft::new_in(n, &mut mem).expect("factorable");
            let win: Vec<i16> = (0..n).map(|i| (((i * 7919 + 13) % 4001) as i32 - 2000) as i16).collect();
            f.forward(&win);
            for k in 0..n / 2 {
                let want = naive(&win, k);
                let got = f.power(k) as f64;
                assert!((got - want).abs() <= 2e-4 * want.max(1e-2), "n={n} k={k} got {got} want {want}");
            }
        }
    }
    #[test]
    fn rejects_unsupported() {
        let mut mem = vec![0.0f32; 20000];
        assert!(RealFft::new_in(4802, &mut mem).is_none());
        assert!(RealFft::new_in(2 * 7 * 64, &mut mem).is_none());
        assert!(RealFft::new_in(4800, &mut mem[..100]).is_none());
    }
}
