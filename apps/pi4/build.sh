#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build PI4.APP for the IC-705 (build/705/) and the IC-9700 (build/9700/).  Run from this directory.
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
  $GCC $EF -I. -c pi4enc.c -o $B/pi4enc.o
  $GCC $DF -I. -c pi4dec.c -o $B/pi4dec.o
  $GCC $DF -I. -DDIGI_PI4 -c $C/digiapp.c -o $B/705/digi_pi4.o
  $GCC $CF $LF $B/icap_start.o $B/705/digi_pi4.o $B/pi4dec.o $B/txsynth.o $B/pi4enc.o -lgcc -o $B/705/pi4.elf
  $GCC $DF -I. -DDIGI_PI4 $SD9700 -c $C/digiapp.c -o $B/9700/digi_pi4.o
  $GCC $CF $LF $B/icap_start.o $B/9700/digi_pi4.o $B/pi4dec.o $B/txsynth.o $B/pi4enc.o -lgcc -o $B/9700/pi4.elf
fi
if [ "$STEP" = all ] || [ "$STEP" = pack ]; then
  $PY $T/mkapp.py $B/705/pi4.elf  $B/705/PI4.APP  --name PI4 --stack 8192 --abi 3 --background --mode PI4
  $PY $T/mkapp.py $B/9700/pi4.elf $B/9700/PI4.APP --name PI4 --models 2 --stack 8192 --abi 3 --background --mode PI4
fi
