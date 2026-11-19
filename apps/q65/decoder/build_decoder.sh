#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the Q65 decoder staticlib libq65beacon.a (ARM hard-float, no_std) for Q65.APP.
#   1. fetch mfsk-core 0.10.1 from crates.io (checksum-pinned), unpack it next to the q65beacon crate
#      and apply mfsk-core-0.10.1-icap.patch;
#   2. build q65beacon (feature "radio") for armv7a-none-eabihf, size-optimised, with build-std.
# Needs curl, tar, patch, sha256sum and rustup (the pinned nightly of q65beacon/rust-toolchain.toml is
# installed by rustup on first use).  Run from this directory; e.g. in the official Rust container image:
#   docker run --rm -v "$PWD":/work -w /work rust:1 sh build_decoder.sh
# (mounted at /work as above, the result links into a Q65.APP that is byte-identical to the release build;
#  the absolute path of the crate goes into Rust symbol hashes, so another path reorders the code)
# Output: q65beacon/target/armv7a-none-eabihf/release/libq65beacon.a
set -e
cd "$(dirname "$0")"
V=0.10.1
SUM=84326ff4728ae29c4af37065106dd1d6d754791560eaa2d0669aa8ff7c1a25e3
if [ ! -d mfsk-core ]; then
  [ -f mfsk-core-$V.crate ] || curl -sSfL -o mfsk-core-$V.crate https://static.crates.io/crates/mfsk-core/mfsk-core-$V.crate
  echo "$SUM  mfsk-core-$V.crate" | sha256sum -c -
  tar -xzf mfsk-core-$V.crate
  mv mfsk-core-$V mfsk-core
  (cd mfsk-core && patch -p1 < ../mfsk-core-$V-icap.patch)
fi
cd q65beacon
rustc --version
CARGO_PROFILE_RELEASE_OPT_LEVEL=z CARGO_PROFILE_RELEASE_LTO=true CARGO_PROFILE_RELEASE_CODEGEN_UNITS=1 \
RUSTFLAGS="-Zunstable-options -Cpanic=immediate-abort -Ctarget-cpu=cortex-a9" \
cargo build --release --locked --target armv7a-none-eabihf --no-default-features --features radio \
   -Z build-std=core,alloc,compiler_builtins -Z build-std-features=optimize_for_size
ls -l target/armv7a-none-eabihf/release/libq65beacon.a
