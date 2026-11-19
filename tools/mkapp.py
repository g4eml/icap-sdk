#!/usr/bin/env python3
"""mkapp.py - turn an ICAP app ELF into a *.APP file for the ICAP loader.

The ELF must be linked at 0 with tools/app.ld (one relocatable image), built with
-mword-relocations -fno-section-anchors and linked with -Wl,--emit-relocs. The *.APP is a 64-byte
header (+ a 32-byte extension when --mode is used) followed by the image and its R_ARM_ABS32
relocation offsets, CRC32-protected.

    python3 mkapp.py app.elf HELLO.APP --name HELLO [--models 3] [--stack 8192] [--abi 3]
                     [--background] [--mode NAME[:usbd|keep]]

--models: bitmask 1 = IC-705, 2 = IC-9700, 3 = both.  --abi 3: the app uses v1rc services.
--background: EXIT hides the window instead of stopping the app (decoders, beacons, downloads).
--mode: register a decoder mode (up to 4, name up to 4 characters); the radio switches to USB-D
        when it is selected (":keep" keeps the current radio mode).
Pure python (no pyelftools). MIT license (see LICENSE).
"""
import argparse, struct, sys, zlib

MAGIC, ABI, HDR = 0x50414349, 1, 64
R_ARM_ABS32, R_ARM_TARGET1 = 2, 38
PCREL_OK = {0, 3, 10, 27, 28, 29, 30, 40, 42, 102, 103, 51, 52, 25}   # NONE, REL32, THM_CALL, PLT32, CALL, JUMP24, THM_JUMP24, V4BX, PREL31, THM_JUMP11/8, THM_JUMP19/6, BASE_PREL?
FORBIDDEN = {43, 44, 47, 48, 45, 46, 49, 50}                         # MOVW/MOVT absolute (use -mword-relocations)


def parse(elf):
    assert elf[:4] == b"\x7fELF" and elf[4] == 1 and elf[5] == 1, "need ELF32 LE"
    e_entry, e_phoff, e_shoff = struct.unpack_from("<III", elf, 24)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", elf, 46)
    secs = []
    for i in range(e_shnum):
        secs.append(struct.unpack_from("<10I", elf, e_shoff + i * e_shentsize))
    shstr = secs[e_shstrndx]
    name = lambda off: elf[shstr[4] + off:elf.index(b"\0", shstr[4] + off)].decode()
    S = [dict(name=name(s[0]), type=s[1], flags=s[2], addr=s[3], off=s[4], size=s[5], link=s[6], info=s[7],
              entsize=s[9], idx=i) for i, s in enumerate(secs)]
    return e_entry, S


def symbols(elf, S):
    st = next(s for s in S if s["type"] == 2)
    strs = S[st["link"]]
    out = []
    for i in range(st["size"] // 16):
        nm, val, size, info, other, shndx = struct.unpack_from("<IIIBBH", elf, st["off"] + 16 * i)
        n = elf[strs["off"] + nm:elf.index(b"\0", strs["off"] + nm)].decode()
        out.append(dict(name=n, value=val, shndx=shndx))
    return out, st["idx"]


def build(elf, name, models, fw, stack, abi=ABI, flags=0):
    entry, S = parse(elf)
    syms, symidx = symbols(elf, S)
    sv = {s["name"]: s["value"] for s in syms}
    img_end, bss_s, bss_e = sv["_icap_image_end"], sv["_icap_bss_start"], sv["_icap_bss_end"]
    img = bytearray(img_end)
    for s in S:
        if s["flags"] & 2 and s["type"] == 1 and s["size"]:           # ALLOC + PROGBITS
            assert s["addr"] + s["size"] <= img_end, s["name"]
            img[s["addr"]:s["addr"] + s["size"]] = elf[s["off"]:s["off"] + s["size"]]
    img += bytes(-len(img) % 4)
    relocs = []
    for s in S:
        if s["type"] != 9:
            continue
        tgt = S[s["info"]]
        if not tgt["flags"] & 2:
            continue
        assert s["link"] == symidx
        for i in range(s["size"] // 8):
            off, info = struct.unpack_from("<II", elf, s["off"] + 8 * i)
            typ, si = info & 0xFF, info >> 8
            if typ in FORBIDDEN:
                raise SystemExit("absolute MOVW/MOVT relocation at %#x: compile with -mword-relocations" % off)
            if typ in (R_ARM_ABS32, R_ARM_TARGET1):
                if syms[si]["shndx"] == 0xFFF1:                      # absolute symbol: a real address
                    continue
                if tgt["type"] == 8:                                 # .bss words are zeroed anyway
                    continue
                assert off % 4 == 0 and off + 4 <= len(img)
                v = struct.unpack_from("<I", img, off)[0]            # the loader enforces the same rule
                if v > bss_e:
                    raise SystemExit("ABS32 at %#x = %#x points past the app (%#x): compile with -fno-section-anchors"
                                     % (off, v, bss_e))
                relocs.append(off)
            elif typ not in PCREL_OK:
                raise SystemExit("unsupported relocation type %d at %#x" % (typ, off))
    relocs = sorted(set(relocs))
    body = bytes(img) + b"".join(struct.pack("<I", r) for r in relocs)
    crc = zlib.crc32(body) & 0xFFFFFFFF
    nm = name.encode()[:15].ljust(16, b"\0")
    hdr = struct.pack("<IHHIIIIIIII16sII", MAGIC, abi, HDR, models, fw, len(img), bss_e - len(img) if bss_e > len(img) else 0,
                      entry, len(relocs), stack, crc, nm, flags, 0)
    assert len(hdr) == HDR
    return hdr + body, dict(load_size=len(img), bss=bss_e - len(img), entry=entry, relocs=len(relocs), crc=crc)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("elf"); ap.add_argument("out")
    ap.add_argument("--name", required=True)
    ap.add_argument("--models", type=lambda x: int(x, 0), default=3)
    ap.add_argument("--fw", type=lambda x: int(x, 0), default=0)
    ap.add_argument("--stack", type=lambda x: int(x, 0), default=8192)
    ap.add_argument("--abi", type=int, default=ABI, choices=(1, 3), help="header ABI (3 = v1rc services)")
    ap.add_argument("--autostart", action="store_true", help="set ICAP_FLAG_AUTOSTART (still needs AUTORUN.CFG)")
    ap.add_argument("--background", action="store_true", help="set ICAP_FLAG_BACKGROUND: EXIT hides the window, app keeps running")
    ap.add_argument("--mode", action="append", default=[], metavar="NAME[:usbd|keep]",
                    help="register a decoder mode (ABI 3, up to 4, name up to 4 chars); default radio mode usbd")
    a = ap.parse_args()
    raw = open(a.elf, "rb").read()
    fl = (0x2 if a.autostart else 0) | (0x4 if a.background else 0)
    data, info = build(raw, a.name, a.models, a.fw, a.stack, a.abi, fl)
    if a.mode:                                           # ABI 3 header extension: mode registration
        # the loader reads hdr_size 96: 64-byte header, 32-byte extension, then the image + relocs;
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
