#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Dawid SQ6EMM (split-image variant of tools/mkapp.py; MIT, see ../../LICENSE)
"""mkapp_split.py - pack a split-image ICAP app ELF (Q65) into a *.APP file.

tools/mkapp.py packs the normal kind of app: one image linked at 0 that the loader relocates. A split
image is different: it is fully linked at its final addresses by a per-radio link script
(q65_split705.ld / q65_split9700.ld) and carries no relocations:

    segment 1  .text/.data (+ .bss)   at the ICAP app window        (MEMORY region WIN of the link script)
    segment 2  .rodata (+ NOBITS)     at the ICAP app data region   (MEMORY region REG)

The *.APP is the 64-byte header (+ the 32-byte mode extension), segment 1, then segment 2; the CRC32
covers both segments. Both base addresses are read from the ELF (the .text section and the
_icap_seg2_start symbol), so they are written down in one place only: the link script.

    python3 mkapp_split.py q65.elf Q65.APP --name Q65 --models 1 --stack 8192 --background --mode Q65

Needs the link-script symbols _icap_image_end, _icap_bss_start/_end, _icap_seg2_start/_end.
Uses the ELF reader of tools/mkapp.py. Pure python.
"""
import argparse, os, struct, sys, zlib

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
from mkapp import FORBIDDEN, HDR, MAGIC, parse, symbols  # noqa: E402

ABI_SPLIT_MIN, FLAG_SPLIT = 2, 0x1


def build_split(elf, name, models, fw, stack, abi, flags=0):
    entry, S = parse(elf)
    syms, _ = symbols(elf, S)
    sv = {s["name"]: s["value"] for s in syms}
    img_end, bss_e = sv["_icap_image_end"], sv["_icap_bss_end"]
    region_base, s2e = sv["_icap_seg2_start"], sv["_icap_seg2_end"]
    win_base = next(s["addr"] for s in S if s["name"] == ".text")
    assert win_base <= entry < img_end, "entry not in segment 1"
    seg1 = bytearray(img_end - win_base)
    seg2 = bytearray(s2e - region_base)
    for s in S:
        if not (s["flags"] & 2 and s["type"] == 1 and s["size"]):      # ALLOC + PROGBITS
            continue
        a, n = s["addr"], s["size"]
        if win_base <= a and a + n <= img_end:
            seg1[a - win_base:a - win_base + n] = elf[s["off"]:s["off"] + n]
        elif region_base <= a and a + n <= s2e:
            seg2[a - region_base:a - region_base + n] = elf[s["off"]:s["off"] + n]
        else:
            raise SystemExit("section %s @%#x outside segment 1 (%#x) or 2 (%#x)" % (s["name"], a, win_base, region_base))
    seg1 += bytes(-len(seg1) % 4); seg2 += bytes(-len(seg2) % 4)
    # no relocations in a split image; if the ELF kept any (--emit-relocs), reject absolute MOVW/MOVT
    for s in S:
        if s["type"] != 9 or not S[s["info"]]["flags"] & 2:
            continue
        for i in range(s["size"] // 8):
            off, info = struct.unpack_from("<II", elf, s["off"] + 8 * i)
            if info & 0xFF in FORBIDDEN:
                raise SystemExit("absolute MOVW/MOVT relocation at %#x: compile with -mword-relocations" % off)
    body = bytes(seg1) + bytes(seg2)
    crc = zlib.crc32(body) & 0xFFFFFFFF
    nm = name.encode()[:15].ljust(16, b"\0")
    hdr = struct.pack("<IHHIIIIIIII16sII", MAGIC, abi, HDR, models, fw, len(seg1), bss_e - img_end,
                      entry - win_base, 0, stack, crc, nm, FLAG_SPLIT | flags, len(seg2))
    assert len(hdr) == HDR
    return hdr + body, dict(load_size=len(seg1), bss=bss_e - img_end, entry_off=entry - win_base,
                            seg2_size=len(seg2), crc=crc, abi=abi, win_base=win_base, region_base=region_base,
                            seg1_plus_bss=bss_e - win_base)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="pack a split-image ICAP app (see the module docstring)")
    ap.add_argument("elf"); ap.add_argument("out")
    ap.add_argument("--name", required=True)
    ap.add_argument("--models", type=lambda x: int(x, 0), required=True, help="1 = IC-705, 2 = IC-9700 (a split image fits one radio)")
    ap.add_argument("--fw", type=lambda x: int(x, 0), default=0, help="id of the loader build the image is linked for (header field; default 0)")
    ap.add_argument("--stack", type=lambda x: int(x, 0), default=8192)
    ap.add_argument("--abi", type=int, default=3, choices=(2, 3), help="header ABI (3 = v1rc services)")
    ap.add_argument("--autostart", action="store_true", help="set ICAP_FLAG_AUTOSTART (still needs AUTORUN.CFG)")
    ap.add_argument("--background", action="store_true", help="set ICAP_FLAG_BACKGROUND: EXIT hides the window, app keeps running")
    ap.add_argument("--mode", action="append", default=[], metavar="NAME[:usbd|keep]",
                    help="register a decoder mode (ABI 3, up to 4, name up to 4 chars); default radio mode usbd")
    a = ap.parse_args()
    raw = open(a.elf, "rb").read()
    fl = (0x2 if a.autostart else 0) | (0x4 if a.background else 0)
    data, info = build_split(raw, a.name, a.models, a.fw, a.stack, max(a.abi, ABI_SPLIT_MIN), fl)
    if a.mode:                                           # ABI 3 header extension: mode registration
        # the loader reads hdr_size 96: 64-byte header, 32-byte extension, then segment 1 + segment 2;
        # the CRC covers only what follows the extension.
        assert a.abi == 3 and len(a.mode) <= 4, "--mode needs --abi 3, at most 4 modes"
        ext = b""
        for md in a.mode:
            nm, _, req = md.partition(":")
            assert 1 <= len(nm) <= 4 and nm.isascii() and " " not in nm, "mode name: 1..4 ASCII chars"
            ext += nm.encode().ljust(6, b"\0") + bytes([{"": 1, "usbd": 1, "keep": 0}[req.lower()], 0])
        ext = ext.ljust(32, b"\0")
        data = data[:6] + struct.pack("<H", 96) + data[8:64] + ext + data[64:]
    open(a.out, "wb").write(data)
    print(a.out, len(data), {k: (hex(v) if isinstance(v, int) else v) for k, v in info.items()})
