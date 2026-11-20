# SPDX-License-Identifier: GPL-3.0-or-later
# Compiler / linker flags shared by the FT8, JT4, PI4 and WSPR build scripts (sourced, POSIX sh).
# The including script sets T (tools/), SDK (directory of icap.h) and C (this directory) first.
CF="-mcpu=cortex-a9 -marm -mfpu=neon-vfpv3 -mfloat-abi=hard -O2 -ffreestanding -nostdlib -fno-builtin -fno-pic -mword-relocations -fno-common -fno-tree-loop-distribute-patterns -fno-section-anchors -Wall -I$SDK"
LF="-nostdlib -T $T/app.ld -Wl,--emit-relocs -Wl,--no-dynamic-linker"
DF="$CF -fbuiltin -fno-math-errno -I$C"      # decoders and the app shell
EF="$DF -ffp-contract=off"                   # beacon encoders: bit-exact floating point (no FMA contraction)
# IC-9700 builds: captures go to (possibly slow) SD cards in small writes, the decoder reads through a small cache;
# -DQSO_THUMB compiles the QSO helper (qso.h) as Thumb code (the 9700 app window is smaller)
SD9700="-DDIGI_CAPBUF=4096 -DDIGI_RDBUF=2048 -DQSO_THUMB"
GCC=${GCC:-arm-none-eabi-gcc}
PY=${PYTHON:-python3}
