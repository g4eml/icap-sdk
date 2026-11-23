#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the PC test tools for Q65.APP into host/build/ (x86-64 Linux; needs gcc and the Rust toolchain of
# ../decoder/q65beacon/rust-toolchain.toml, and ../decoder/mfsk-core from ../decoder/build_decoder.sh):
#   build/harness  the real q65_705.c + the decoder built with its radio feature, against a simulated radio
#   build/q65gen   one T/R period of Q65 test audio (8 kHz WAV) from the app's own encoder
# Run from this directory.
set -e
cd "$(dirname "$0")"
A=..; C=../../digi-common; SDK=../../..; B=build
[ -d ../decoder/mfsk-core ] || { echo "missing ../decoder/mfsk-core - run ../decoder/build_decoder.sh first"; exit 1; }
mkdir -p $B
# the decoder exactly as on the radio (callback ABI, region allocator), compiled for the PC
(cd ../decoder/q65beacon && cargo build --release --lib --no-default-features --features radio \
   --target x86_64-unknown-linux-gnu --target-dir "$OLDPWD/$B/rust")
LIB=$B/rust/x86_64-unknown-linux-gnu/release/libq65beacon.a
ENC="$A/q65enc.c $C/txsynth.c $C/msg77.c"
# -Dtarget(x)= drops the Thumb attributes of the radio build; -ffp-contract=off as the radio's encoders
gcc -O1 -g -ffp-contract=off -DQ65_HOST_TRACE '-Dtarget(x)=' -I$SDK -I$A -I$C \
    harness.c $A/q65_705.c $ENC "$LIB" -lm -lpthread -ldl -o $B/harness
gcc -O2 -ffp-contract=off -I$A -I$C q65gen.c $ENC -lm -o $B/q65gen
echo "built $B/harness $B/q65gen"
