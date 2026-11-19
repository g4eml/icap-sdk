#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build JT4.APP for the IC-705 (build/705/) and the IC-9700 (build/9700/).  Run from this directory.
#   sh build.sh            compile + pack   (needs arm-none-eabi-gcc and python3)
#   sh build.sh compile    compile + link only (arm-none-eabi-gcc)
#   sh build.sh pack       pack only (python3)
set -e
T=../../tools; SDK=../..; C=../digi-common
. $C/flags.sh
B=build; STEP=${1:-all}
if [ "$STEP" = all ] || [ "$STEP" = compile ]; then
  mkdir -p $B/705 $B/9700
  $GCC $CF -c $T/icap_start.S -o $B/icap_start.o
  $GCC $EF -c $C/txsynth.c -o $B/txsynth.o
  $GCC $EF -I. -c jt4enc.c -o $B/jt4enc.o
  JO=""
  for f in jt4dec fano232 jt72msg; do $GCC $DF -I. -c $f.c -o $B/$f.o; JO="$JO $B/$f.o"; done
  $GCC $DF -c $C/digi_util.c -o $B/digi_util.o; JO="$JO $B/digi_util.o"
  $GCC $DF -I. -DDIGI_JT4 -c $C/digiapp.c -o $B/705/digi_jt4.o
  $GCC $CF $LF $B/icap_start.o $B/705/digi_jt4.o $JO $B/txsynth.o $B/jt4enc.o -lgcc -o $B/705/jt4.elf
  $GCC $DF -I. -DDIGI_JT4 $SD9700 -c $C/digiapp.c -o $B/9700/digi_jt4.o
  $GCC $CF $LF $B/icap_start.o $B/9700/digi_jt4.o $JO $B/txsynth.o $B/jt4enc.o -lgcc -o $B/9700/jt4.elf
fi
if [ "$STEP" = all ] || [ "$STEP" = pack ]; then
  $PY $T/mkapp.py $B/705/jt4.elf  $B/705/JT4.APP  --name JT4 --stack 8192 --abi 3 --background --mode JT4
  $PY $T/mkapp.py $B/9700/jt4.elf $B/9700/JT4.APP --name JT4 --models 2 --stack 8192 --abi 3 --background --mode JT4
fi
