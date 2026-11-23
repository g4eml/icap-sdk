// SPDX-License-Identifier: GPL-3.0-or-later
//! Host / QEMU harness for q65beacon.
//!
//! Usage:
//!   q65beacon_cli <wav> [--period 15|30|60] [--sub A..E]
//!                 [--nominal-s S] [--fmin HZ] [--fmax HZ]
//!                 [--mode blind|template]
//!                 [--call1 C --call2 C --grid G]   (template codewords)
//!                 [--log PATH]
//!
//! Default: Q65-60D.  Converts the mono WAV (12 or 8 kHz) to a raw LE-f32 "SD capture" file, then
//! runs the segmented decoder with SD-backed PCM + sync-spectra stores.
//! Prints a JSON result line and (with --features meter) the peak live
//! heap bytes measured over the decode window only.

use std::time::Instant;

#[cfg(feature = "meter")]
mod meter {
    use std::alloc::{GlobalAlloc, Layout, System};
    use std::sync::atomic::{AtomicUsize, Ordering};
    pub static CUR: AtomicUsize = AtomicUsize::new(0);
    pub static PEAK: AtomicUsize = AtomicUsize::new(0);
    pub struct Counting;
    unsafe impl GlobalAlloc for Counting {
        unsafe fn alloc(&self, l: Layout) -> *mut u8 {
            let p = unsafe { System.alloc(l) };
            if !p.is_null() {
                let c = CUR.fetch_add(l.size(), Ordering::Relaxed) + l.size();
                PEAK.fetch_max(c, Ordering::Relaxed);
            }
            p
        }
        unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
            CUR.fetch_sub(l.size(), Ordering::Relaxed);
            unsafe { System.dealloc(p, l) }
        }
        unsafe fn realloc(&self, p: *mut u8, l: Layout, ns: usize) -> *mut u8 {
            let np = unsafe { System.realloc(p, l, ns) };
            if !np.is_null() {
                if ns >= l.size() {
                    let c = CUR.fetch_add(ns - l.size(), Ordering::Relaxed) + (ns - l.size());
                    PEAK.fetch_max(c, Ordering::Relaxed);
                } else {
                    CUR.fetch_sub(l.size() - ns, Ordering::Relaxed);
                }
            }
            np
        }
    }
    pub fn reset_peak() {
        PEAK.store(CUR.load(Ordering::Relaxed), Ordering::Relaxed);
    }
    pub fn cur() -> usize {
        CUR.load(Ordering::Relaxed)
    }
    pub fn peak() -> usize {
        PEAK.load(Ordering::Relaxed)
    }
}
#[cfg(feature = "meter")]
#[global_allocator]
static GA: meter::Counting = meter::Counting;

/// Minimal mono-12 kHz WAV reader (PCM16 or float32).
fn read_wav(path: &str) -> Vec<f32> {
    let data = std::fs::read(path).expect("read wav");
    assert!(&data[0..4] == b"RIFF" && &data[8..12] == b"WAVE", "not a WAV");
    let mut pos = 12;
    let mut fmt_tag = 1u16;
    let mut bits = 16u16;
    let mut channels = 1u16;
    let mut rate = 0u32;
    let mut out = Vec::new();
    while pos + 8 <= data.len() {
        let id = &data[pos..pos + 4];
        let sz = u32::from_le_bytes([data[pos + 4], data[pos + 5], data[pos + 6], data[pos + 7]]) as usize;
        let body = pos + 8;
        if id == b"fmt " {
            fmt_tag = u16::from_le_bytes([data[body], data[body + 1]]);
            channels = u16::from_le_bytes([data[body + 2], data[body + 3]]);
            rate = u32::from_le_bytes([data[body + 4], data[body + 5], data[body + 6], data[body + 7]]);
            bits = u16::from_le_bytes([data[body + 14], data[body + 15]]);
        } else if id == b"data" {
            let end = (body + sz).min(data.len());
            let bytes_per = (bits / 8) as usize;
            let frame = bytes_per * channels as usize;
            let mut i = body;
            while i + frame <= end {
                // take channel 0 only
                let s = if fmt_tag == 3 && bits == 32 {
                    f32::from_le_bytes([data[i], data[i + 1], data[i + 2], data[i + 3]])
                } else if bits == 16 {
                    i16::from_le_bytes([data[i], data[i + 1]]) as f32 / 32768.0
                } else {
                    panic!("unsupported wav sample format tag={fmt_tag} bits={bits}");
                };
                out.push(s);
                i += frame;
            }
            break;
        }
        pos = body + sz + (sz & 1);
    }
    unsafe { WAV_RATE = rate; }
    out
}

static mut WAV_RATE: u32 = 12_000;

fn arg(args: &[String], key: &str) -> Option<String> {
    args.iter().position(|a| a == key).and_then(|i| args.get(i + 1).cloned())
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    // Q65_FTZ=1: flush denormals to zero (x86 MXCSR FTZ+DAZ), as NEON always does on the radio
    #[cfg(target_arch = "x86_64")]
    if std::env::var_os("Q65_FTZ").is_some() {
        #[allow(deprecated)]
        unsafe {
            use std::arch::x86_64::{_mm_getcsr, _mm_setcsr};
            _mm_setcsr(_mm_getcsr() | 0x8040);
        }
    }
    if args.len() < 2 {
        eprintln!("usage: q65beacon_cli <wav> [--period 15|30|60] [--sub A..E] [--nominal-s S] [--fmin HZ] [--fmax HZ] [--mode blind|template] [--call1 C --call2 C --grid G] [--log PATH]");
        std::process::exit(2);
    }
    let wav = &args[1];
    let nominal_s: f32 = arg(&args, "--nominal-s").map(|s| s.parse().unwrap()).unwrap_or(0.0);
    let fmin: f32 = arg(&args, "--fmin").map(|s| s.parse().unwrap()).unwrap_or(900.0);
    let fmax: f32 = arg(&args, "--fmax").map(|s| s.parse().unwrap()).unwrap_or(1100.0);
    let mode_s = arg(&args, "--mode").unwrap_or_else(|| "blind".into());
    let logp = arg(&args, "--log");
    let period_s: u32 = arg(&args, "--period").map(|s| s.parse().unwrap()).unwrap_or(60);
    let sub_letter = arg(&args, "--sub").unwrap_or_else(|| "D".into()).to_ascii_uppercase();
    let submode = match sub_letter.as_str() {
        "A" => 0u8,
        "B" => 1,
        "C" => 2,
        "D" => 3,
        "E" => 4,
        _ => panic!("--sub must be A..E"),
    };
    let mode_name = format!("Q65-{period_s}{sub_letter}");

    // --- setup (outside the measurement window): WAV → SD-capture file.
    let pcm = read_wav(wav);
    let pcm_path = "/tmp/q65b_pcm.f32";
    let spec_path = "/tmp/q65b_spectra.f32";
    {
        use std::io::Write;
        let mut f = std::io::BufWriter::new(std::fs::File::create(pcm_path).unwrap());
        let mut bytes = Vec::with_capacity(pcm.len() * 4);
        for &s in &pcm {
            bytes.extend_from_slice(&s.to_le_bytes());
        }
        f.write_all(&bytes).unwrap();
    }
    // Free the host-only whole-file PCM buffer so the measurement
    // baseline reflects only the SD-streaming decoder (on the radio the
    // PCM lives on the SD card, not in RAM).
    drop(pcm);

    // Template candidates (generated here = precomputed/flashed on-radio).
    let templates: Vec<[i32; 63]> = if mode_s == "template" {
        let c1 = arg(&args, "--call1").expect("--call1 required for template");
        let c2 = arg(&args, "--call2").expect("--call2 required for template");
        let grid = arg(&args, "--grid").unwrap_or_default();
        let cw = q65beacon::mfsk_core::q65::standard_qso_codewords(&c1, &c2, &grid);
        assert!(!cw.is_empty(), "no template codewords generated");
        cw
    } else {
        Vec::new()
    };

    let params = q65beacon::Params {
        period_s,
        submode,
        sample_rate: unsafe { WAV_RATE },
        nominal_start_sample: (nominal_s * unsafe { WAV_RATE } as f32).round() as usize,
        freq_min_hz: fmin,
        freq_max_hz: fmax,
        ..q65beacon::Params::default()
    };

    let src = q65beacon::source::FilePcmSource::open(pcm_path).unwrap();
    let mut store = q65beacon::source::FileSpectraStore::create(spec_path).unwrap();

    #[cfg(feature = "meter")]
    {
        eprintln!("meter: baseline live bytes before decode = {}", meter::cur());
        eprintln!("meter: template candidate count = {}", templates.len());
        meter::reset_peak();
    }

    let mode = if templates.is_empty() {
        q65beacon::Mode::Blind
    } else {
        q65beacon::Mode::Template(&templates)
    };

    let t0 = Instant::now();
    let result = if let Some(lp) = logp {
        // each line prefixed "+<ms> " since the decode started, as on the radio
        struct Timed(q65beacon::debuglog::FileLog, Instant);
        impl q65beacon::DebugLog for Timed {
            fn line(&mut self, s: &str) {
                let ms = self.1.elapsed().as_secs_f64() * 1000.0;
                self.0.line(&format!("+{ms:.1} {s}"));
            }
        }
        let mut log = Timed(q65beacon::debuglog::FileLog::create(&lp).unwrap(), Instant::now());
        q65beacon::decode(&src, &mut store, &mut log, &params, mode)
    } else {
        q65beacon::decode(&src, &mut store, &mut q65beacon::NullLog, &params, mode)
    };
    let dt = t0.elapsed();

    #[cfg(feature = "meter")]
    {
        let base = meter::cur();
        let peak = meter::peak();
        eprintln!("meter: PEAK live heap bytes during decode = {peak}");
        eprintln!("meter: decode-attributable peak (peak - residual baseline) approx = {}", peak.saturating_sub(0));
        let _ = base;
    }

    match result {
        Some(d) => println!(
            "{{\"text\": \"{}\", \"mode\": \"{}\", \"tone0_hz\": {:.2}, \"dt_s\": {:.3}, \"snr_db\": {:.1}, \"iterations\": {}, \"llh\": {:.1}, \"decode_s\": {:.3}}}",
            d.text, mode_name, d.freq_hz, d.dt_s, d.snr_db, d.iterations, d.llh, dt.as_secs_f64()
        ),
        None => {
            println!("{{\"text\": null, \"decode_s\": {:.3}}}", dt.as_secs_f64());
            std::process::exit(1);
        }
    }
}
