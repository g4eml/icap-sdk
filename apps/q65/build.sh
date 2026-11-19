#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build Q65.APP for the IC-705 (build/705/) and the IC-9700 (build/9700/).  Run from this directory.
#   sh build.sh            compile + pack   (needs arm-none-eabi-gcc and python3)
#   sh build.sh compile    compile + link only (arm-none-eabi-gcc)
#   sh build.sh pack       pack only (python3)
# Needs the decoder staticlib first (Rust, see decoder/build_decoder.sh); override its path with LIBQ65=...
# Q65.APP is a split image: linked at the app-window / data-region addresses of each radio by
# q65_split705.ld / q65_split9700.ld and packed with mkapp_split.py (see README.md).
set -e
T=../../tools; SDK=../..; C=../digi-common
GCC=${GCC:-arm-none-eabi-gcc}; PY=${PYTHON:-python3}
LIBQ65=${LIBQ65:-decoder/q65beacon/target/armv7a-none-eabihf/release/libq65beacon.a}
CF="-mcpu=cortex-a9 -marm -mfpu=neon-vfpv3 -mfloat-abi=hard -O2 -ffreestanding -nostdlib -fno-builtin -fno-pic -mword-relocations -fno-common -fno-tree-loop-distribute-patterns -ffunction-sections -fdata-sections -fno-section-anchors -Wall -Wextra -Wno-unused-parameter -I. -I$SDK -I$C"
EF="$CF -fbuiltin -fno-math-errno -ffp-contract=off"   # beacon encoders: bit-exact floating point
B=build; STEP=${1:-all}
if [ "$STEP" = all ] || [ "$STEP" = compile ]; then
  [ -f "$LIBQ65" ] || { echo "missing $LIBQ65 - build the decoder first (decoder/build_decoder.sh)"; exit 1; }
  for R in 705 9700; do
    D=$B/$R; mkdir -p $D
    LF="-nostdlib -T q65_split$R.ld -Wl,--gc-sections -Wl,--no-dynamic-linker -Wl,-Map=$D/q65.map"
    $GCC $CF -c $T/icap_start.S -o $D/icap_start.o
    $GCC $CF -Os -c q65_705.c -o $D/q65_705.o      # the shim is not the hot path: size first
    $GCC $CF -Oz -c stubs.c -o $D/stubs.o
    for e in txsynth msg77; do $GCC $EF -c $C/$e.c -o $D/$e.o; done
    $GCC $EF -c q65enc.c -o $D/q65enc.o
    $GCC $CF $LF $D/icap_start.o $D/q65_705.o $D/stubs.o $D/txsynth.o $D/msg77.o $D/q65enc.o "$LIBQ65" -lgcc -o $D/q65.elf
    # a split image is fully linked: nothing may stay undefined
    if ${GCC%gcc}nm -u $D/q65.elf | grep -q .; then echo "UNDEFINED SYMBOLS ($R):"; ${GCC%gcc}nm -u $D/q65.elf; exit 1; fi
  done
fi
if [ "$STEP" = all ] || [ "$STEP" = pack ]; then
  $PY mkapp_split.py $B/705/q65.elf  $B/705/Q65.APP  --name Q65 --models 1 --abi 3 --background --mode Q65 --stack 8192
  # --fw: the id of the IC-9700 loader build this image is linked for
  $PY mkapp_split.py $B/9700/q65.elf $B/9700/Q65.APP --name Q65 --models 2 --abi 3 --background --mode Q65 --stack 8192 --fw 0x97001710
fi
