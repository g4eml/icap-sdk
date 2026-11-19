#!/bin/sh
# Build HELLO.APP with the Arm GNU toolchain (arm-none-eabi-gcc) and python3.  Run from this directory.
set -e
T=../../tools
CF="-mcpu=cortex-a9 -marm -mfpu=neon-vfpv3 -mfloat-abi=hard -O2 -ffreestanding -nostdlib -fno-builtin \
    -fno-pic -mword-relocations -fno-common -fno-tree-loop-distribute-patterns -fno-section-anchors -Wall -I../.."
LF="-nostdlib -T $T/app.ld -Wl,--emit-relocs -Wl,--no-dynamic-linker"
arm-none-eabi-gcc $CF -c $T/icap_start.S -o icap_start.o
arm-none-eabi-gcc $CF -c hello.c -o hello.o
arm-none-eabi-gcc $CF $LF icap_start.o hello.o -lgcc -o hello.elf
python3 $T/mkapp.py hello.elf HELLO.APP --name HELLO --models 3 --stack 4096 --abi 3
