// SPDX-License-Identifier: GPL-3.0-or-later
//! Backing-store abstractions. On the radio these are the SD card;
//! on the host harness they are files on disk. The decoder core only
//! ever holds a few small blocks/rows resident — the bulk PCM and the
//! intermediate sync spectra live in these stores.

/// Streamed read-only access to the captured 12 kHz mono f32 PCM.
/// Models the SD-card WAV/PCM capture: the decoder reads short blocks
/// by sample offset instead of holding the whole 60 s recording
/// (2.88 MB) resident.
pub trait SampleSource {
    /// Total sample count available.
    fn len(&self) -> usize;
    fn is_empty(&self) -> bool {
        self.len() == 0
    }
    /// Fill `out` with samples `[start, start+out.len())`. Samples
    /// past the end read as 0.0. Returns how many real samples were
    /// read (out.len() unless the window runs off the end).
    fn read_block(&self, start: usize, out: &mut [f32]) -> usize;
}

/// Row-addressable scratch for the narrow tone-0 sync spectrogram.
/// Streamed to the SD card so the full (rows × narrow-bins) matrix is
/// never resident; the decoder appends rows during the spectra pass
/// and reads them back during the sync-scoring pass.
pub trait SpectraStore {
    /// Begin a fresh matrix with `ncols` columns per row.
    fn begin(&mut self, ncols: usize);
    /// Append one row (`row.len() == ncols`).
    fn append_row(&mut self, row: &[f32]);
    /// Read row `idx` into `out` (`out.len() == ncols`).
    fn read_row(&self, idx: usize, out: &mut [f32]);
    fn nrows(&self) -> usize;
    fn ncols(&self) -> usize;
}

// ───────────────────────── host (std) impls ─────────────────────────

#[cfg(feature = "std")]
mod host {
    use super::*;
    use std::cell::RefCell;
    use std::fs::File;
    use std::io::{Read, Seek, SeekFrom, Write};

    /// SD-capture model: raw little-endian f32 PCM on disk, read in
    /// blocks by seeking. Nothing but the caller's block buffer is
    /// held resident.
    pub struct FilePcmSource {
        file: RefCell<File>,
        len: usize,
    }

    impl FilePcmSource {
        pub fn open(path: &str) -> std::io::Result<Self> {
            let file = File::open(path)?;
            let len = file.metadata()?.len() as usize / 4;
            Ok(Self { file: RefCell::new(file), len })
        }
    }

    impl SampleSource for FilePcmSource {
        fn len(&self) -> usize {
            self.len
        }
        fn read_block(&self, start: usize, out: &mut [f32]) -> usize {
            if start >= self.len {
                for v in out.iter_mut() {
                    *v = 0.0;
                }
                return 0;
            }
            let n = out.len().min(self.len - start);
            let mut f = self.file.borrow_mut();
            f.seek(SeekFrom::Start((start * 4) as u64)).unwrap();
            let mut bytes = vec![0u8; n * 4];
            f.read_exact(&mut bytes).unwrap();
            for i in 0..n {
                let b = [bytes[4 * i], bytes[4 * i + 1], bytes[4 * i + 2], bytes[4 * i + 3]];
                out[i] = f32::from_le_bytes(b);
            }
            for v in out.iter_mut().skip(n) {
                *v = 0.0;
            }
            n
        }
    }

    /// SD-backed sync-spectra scratch: rows written to a temp file,
    /// read back by seeking. Resident cost is one row buffer.
    pub struct FileSpectraStore {
        file: RefCell<File>,
        ncols: usize,
        nrows: usize,
    }

    impl FileSpectraStore {
        pub fn create(path: &str) -> std::io::Result<Self> {
            let file = File::options()
                .create(true)
                .read(true)
                .write(true)
                .truncate(true)
                .open(path)?;
            Ok(Self { file: RefCell::new(file), ncols: 0, nrows: 0 })
        }
    }

    impl SpectraStore for FileSpectraStore {
        fn begin(&mut self, ncols: usize) {
            self.ncols = ncols;
            self.nrows = 0;
            let mut f = self.file.borrow_mut();
            f.seek(SeekFrom::Start(0)).unwrap();
            f.set_len(0).unwrap();
        }
        fn append_row(&mut self, row: &[f32]) {
            debug_assert_eq!(row.len(), self.ncols);
            let mut f = self.file.borrow_mut();
            f.seek(SeekFrom::End(0)).unwrap();
            let mut bytes = Vec::with_capacity(row.len() * 4);
            for &v in row {
                bytes.extend_from_slice(&v.to_le_bytes());
            }
            f.write_all(&bytes).unwrap();
            self.nrows += 1;
        }
        fn read_row(&self, idx: usize, out: &mut [f32]) {
            debug_assert_eq!(out.len(), self.ncols);
            let mut f = self.file.borrow_mut();
            f.seek(SeekFrom::Start((idx * self.ncols * 4) as u64)).unwrap();
            let mut bytes = vec![0u8; self.ncols * 4];
            f.read_exact(&mut bytes).unwrap();
            for i in 0..self.ncols {
                let b = [bytes[4 * i], bytes[4 * i + 1], bytes[4 * i + 2], bytes[4 * i + 3]];
                out[i] = f32::from_le_bytes(b);
            }
        }
        fn nrows(&self) -> usize {
            self.nrows
        }
        fn ncols(&self) -> usize {
            self.ncols
        }
    }
}

#[cfg(feature = "std")]
pub use host::{FilePcmSource, FileSpectraStore};
