#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build WSPR.APP (WSPR-2 decoder + beacon; one relocatable image for the IC-705 and the IC-9700).  Run from this
# directory.
#   sh build.sh            compile + pack   (needs arm-none-eabi-gcc and python3)
#   sh build.sh compile    compile + link only (arm-none-eabi-gcc)
#   sh build.sh pack       pack only (python3)
set -e
T=../../tools; SDK=../..; C=../digi-common
. $C/flags.sh
B=build; STEP=${1:-all}
if [ "$STEP" = all ] || [ "$STEP" = compile ]; then
  mkdir -p $B
  $GCC $CF -c $T/icap_start.S -o $B/icap_start.o
  for e in txsynth msg77; do $GCC $EF -c $C/$e.c -o $B/$e.o; done
  $GCC $EF -I. -c wsprenc.c -o $B/wsprenc.o
  $GCC $EF -I. -c wsprdec.c -o $B/wsprdec.o     # like the encoders: no FMA contraction (same result as on a PC)
  $GCC $DF -I. -c wsprapp.c -o $B/wsprapp.o
  $GCC $CF $LF $B/icap_start.o $B/wsprapp.o $B/wsprdec.o $B/wsprenc.o $B/txsynth.o $B/msg77.o -lgcc -o $B/wspr.elf
fi
if [ "$STEP" = all ] || [ "$STEP" = pack ]; then
  $PY $T/mkapp.py $B/wspr.elf $B/WSPR.APP --name WSPR --models 3 --stack 8192 --abi 3 --background --mode WSPR
fi
