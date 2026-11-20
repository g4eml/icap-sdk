#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build FT8.APP for the IC-705 (build/705/) and the IC-9700 (build/9700/).  Run from this directory.
#   sh build.sh            compile + pack   (needs arm-none-eabi-gcc and python3)
#   sh build.sh compile    compile + link only (arm-none-eabi-gcc)
#   sh build.sh pack       pack only (python3)
# The vendored ft8_lib (ft8/, MIT) is compiled freestanding through ft8/ft8_port.h.
# IC-705: 16 KiB stack.  IC-9700: 8 KiB stack and -DFT8_ARENA_SCRATCH (the decoder's BP/OSD working arrays
# live in the decoder arena instead of .bss, so image + bss fit the smaller 9700 app window); the 9700 app shell
# is compiled for size (-Os) with a 4 KiB decode history (-DDIGI_HIST=4096) to make room for the QSO helper.
set -e
T=../../tools; SDK=../..; C=../digi-common
. $C/flags.sh
B=build; STEP=${1:-all}
if [ "$STEP" = all ] || [ "$STEP" = compile ]; then
  mkdir -p $B/705 $B/9700
  $GCC $CF -c $T/icap_start.S -o $B/icap_start.o
  for e in txsynth msg77; do $GCC $EF -c $C/$e.c -o $B/$e.o; done
  $GCC $EF -I. -Ift8 -include ft8/ft8_port.h -c ft8enc.c -o $B/ft8enc.o
  FO=""
  for f in constants crc message text encode; do
    $GCC $DF -Ift8 -Wno-unused-variable -Wno-unused-const-variable -Wno-unused-function -include ft8/ft8_port.h -c ft8/ft8/$f.c -o $B/f_$f.o
    FO="$FO $B/f_$f.o"
  done
  $GCC $DF -Ift8 -include ft8/ft8_port.h -c ft8/ft8_port.c -o $B/f_port.o
  $GCC $DF -c $C/digi_util.c -o $B/digi_util.o
  ENC="$B/txsynth.o $B/msg77.o $B/ft8enc.o"
  # IC-705
  $GCC $DF -I. -Ift8 -c ft8dec.c -o $B/705/ft8dec.o
  $GCC $DF -I. -Ift8 -DDIGI_FT8 -c $C/digiapp.c -o $B/705/digi_ft8.o
  $GCC $CF $LF $B/icap_start.o $B/705/digi_ft8.o $B/705/ft8dec.o $B/digi_util.o $B/f_port.o $FO $ENC -lgcc -o $B/705/ft8.elf
  # IC-9700
  $GCC $DF -I. -Ift8 -DFT8_ARENA_SCRATCH -c ft8dec.c -o $B/9700/ft8dec.o
  $GCC $DF -Os -I. -Ift8 -DDIGI_FT8 -DDIGI_HIST=4096 $SD9700 -c $C/digiapp.c -o $B/9700/digi_ft8.o
  $GCC $CF $LF $B/icap_start.o $B/9700/digi_ft8.o $B/9700/ft8dec.o $B/digi_util.o $B/f_port.o $FO $ENC -lgcc -o $B/9700/ft8.elf
fi
if [ "$STEP" = all ] || [ "$STEP" = pack ]; then
  $PY $T/mkapp.py $B/705/ft8.elf  $B/705/FT8.APP  --name FT8 --models 1 --stack 16384 --abi 3 --background --mode FT8
  $PY $T/mkapp.py $B/9700/ft8.elf $B/9700/FT8.APP --name FT8 --models 2 --stack 8192 --abi 3 --background --mode FT8
fi
