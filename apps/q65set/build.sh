#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build Q65SET.APP (one relocatable image for the IC-705 and the IC-9700).  Run from this directory.
#   sh build.sh            compile + pack   (needs arm-none-eabi-gcc and python3)
#   sh build.sh compile    compile + link only (arm-none-eabi-gcc)
#   sh build.sh pack       pack only (python3)
set -e
T=../../tools; SDK=../..
GCC=${GCC:-arm-none-eabi-gcc}; PY=${PYTHON:-python3}
CF="-mcpu=cortex-a9 -marm -mfpu=neon-vfpv3 -mfloat-abi=hard -O2 -ffreestanding -nostdlib -fno-builtin -fno-pic -mword-relocations -fno-common -fno-tree-loop-distribute-patterns -fno-section-anchors -Wall -I$SDK"
LF="-nostdlib -T $T/app.ld -Wl,--emit-relocs -Wl,--no-dynamic-linker"
B=build; STEP=${1:-all}
if [ "$STEP" = all ] || [ "$STEP" = compile ]; then
  mkdir -p $B
  $GCC $CF -c $T/icap_start.S -o $B/icap_start.o
  $GCC $CF -c q65set.c -o $B/q65set.o
  $GCC $CF $LF $B/icap_start.o $B/q65set.o -lgcc -o $B/q65set.elf
fi
if [ "$STEP" = all ] || [ "$STEP" = pack ]; then
  $PY $T/mkapp.py $B/q65set.elf $B/Q65SET.APP --name Q65SET --stack 4096
fi
