"""Build the game files for Verok's patch v1.0.5 with SilentPatch on top (docs/verok-plan.md).

usage: python -I tools/verok-patch.py <unpacked patch folder> <game folder> [--p-base HEX] [--bpp 16|32]

Verok's nfs2se.p patches nfs2se.exe's code while the game runs (DllMain, then at startup and when
the window opens). A static recompile only sees the file on disk, so this writes the code patches
into <game>/nfs2se.exe beforehand. nfs2se.p's data writes still happen at run time when its own
lifted code runs. The patch list below was taken from an emulation of nfs2se.p with
Verok's default ini and ClassicTextures=0 (SE textures). Every patched site's original bytes
are checked first. Pointers into nfs2se.p are stored relative to its base, because the runtime
loads it at --p-base (its own base, 0x10000000, is inside our guest heap).

SilentPatch (CookiePLMonster/SilentPatchNFS90s) is applied after Verok's patches, found by the
same byte patterns it uses. Each pattern must match exactly once.

Not baked in: NoMovies (0x408202, 0x40d73b). Those go to <game>/../build/verok-lift/variants.json,
the lifter emits both versions, and the runtime picks one from NoMovies in nfs2se.ini.

Also writes build/verok-lift/seeds.txt: the exe code addresses nfs2se.p refers to (the return
points of its hooks), which the lifter must treat as function starts. It also installs the
patch's data files: text.*, the new GameData/Tracks/pc/*.HRZ, the Glide renderer as
drivers/glide3x/voodoo2a.dll, and a default nfs2se.ini.
"""
import json
import os
import re
import shutil
import struct
import sys

import pefile

P_ORIG = 0x10000000          # nfs2se.p's own base: the RVAs below are relative to it

# (kind, exe address, value, original bytes): jmp = E9 to nfs2se.p+rva, ptr = dword nfs2se.p+rva,
# nop = n x 90. In the order nfs2se.p writes them (0x4976b8's jmp overwrites the top byte of 0x4976b4's).
VEROK = [
    ("jmp", 0x405cae, 0x1860, "8b84240001"),
    ("ptr", 0x42480a, 0x6040, "18375600"),
    ("jmp", 0x428acf, 0x4d20, "ff15743756"),
    ("jmp", 0x42a170, 0x4fb0, "535152833d"),
    ("jmp", 0x42a270, 0x4dd0, "56c1e00505"),
    ("jmp", 0x42a2a0, 0x5020, "535152a140"),
    ("ptr", 0x42a779, 0x6050, "34375600"),
    ("ptr", 0x42aa9e, 0x6040, "18375600"),
    ("ptr", 0x42abc0, 0x6040, "18375600"),
    ("ptr", 0x42ad46, 0x6040, "18375600"),
    ("ptr", 0x42aebd, 0x6040, "18375600"),
    ("ptr", 0x42c0a0, 0x6040, "18375600"),
    ("ptr", 0x430206, 0x1830, "44b24c00"),
    ("nop", 0x43020f, 5, "e88c030000"),
    ("nop", 0x430216, 5, "e865020000"),
    ("nop", 0x430275, 5, "e8a6060000"),
    ("nop", 0x430298, 5, "e8337f0000"),
    ("nop", 0x4302e1, 5, "e80a400300"),
    ("jmp", 0x433910, 0x4e80, "525589e583"),
    ("jmp", 0x4355b0, 0x2cc0, "5156575589"),
    ("jmp", 0x435bcb, 0x2c20, "88da81e2ff"),
    ("nop", 0x463432, 5, "e8c9c9fcff"),
    ("ptr", 0x47bb58, 0x6040, "18375600"),
    ("jmp", 0x480760, 0x4df0, "528b506c83"),
    ("jmp", 0x481399, 0x47e0, "ff15383756"),
    ("ptr", 0x488706, 0x601c, "98b64d00"),
    ("ptr", 0x4888bb, 0x601c, "98b64d00"),
    ("ptr", 0x48895c, 0x601c, "98b64d00"),
    ("jmp", 0x497160, 0x3b20, "5383ec50e8"),
    ("jmp", 0x497220, 0x3b60, "8b4c24048b"),
    ("jmp", 0x497244, 0x3b10, "83ec0431c9"),
    ("jmp", 0x4972bc, 0x3b40, "538b15e4cf"),
    ("jmp", 0x497318, 0x3b70, "538b542410"),
    ("jmp", 0x497464, 0x3bf0, "535657558b"),
    ("jmp", 0x49755c, 0x3c10, "53568b5c24"),
    ("jmp", 0x4975b0, 0x3c20, "6a00ff1590"),
    ("jmp", 0x4975e8, 0x3c00, "5356558b44"),
    ("jmp", 0x497680, 0x3ba0, "8b44240448"),
    ("jmp", 0x49769c, 0x3b80, "31c066a1ec"),
    ("jmp", 0x4976b4, 0x3b90, "c38d40008b"),
    ("jmp", 0x4976b8, 0x3b30, "8b15f4cf4d"),
    ("jmp", 0x4976c8, 0x3b50, "538b442408"),
    ("jmp", 0x497714, 0x3b00, "53568b5424"),
    ("jmp", 0x497738, 0x3be0, "535683ec48"),
    ("jmp", 0x497a20, 0x3bb0, "5383ec1489"),
    ("jmp", 0x497a84, 0x3bc0, "53568b7424"),
    ("jmp", 0x497ab4, 0x3bd0, "535657558b"),
    ("jmp", 0x497b24, 0x3c50, "5381ecf000"),
    ("jmp", 0x497d6c, 0x3c60, "5356575581"),
    ("jmp", 0x4983b0, 0x3c40, "5381ecb400"),
    ("jmp", 0x498ee0, 0x3c30, "5383ec788b"),
    ("jmp", 0x4a15d0, 0x3af0, "53565755bb"),
    ("jmp", 0x4b2d98, 0x28f0, "5351525589"),
    # when the window opens (0x10004800): the hook below, and the colour depth from
    # _THRASH_about's mode list (16 or 32) in four immediates; --bpp, checked at run time
    # by VirtualProtect (kernel32.c)
    ("jmp", 0x47bb90, 0x1da0, None),
]
BPP_SITES = [0x407e6e, 0x40802c, 0x430232, 0x49afb7]
# NoMovies=1 (0x10002180): whole instructions, both versions lifted (variants.json)
NOMOVIES = [(0x408202, 5, "e8e9c00100", "9090909090"), (0x40d73b, 6, None, None)]


def pattern(img, base, pat, what):
    rx = b"".join(b"." if t == "?" else re.escape(bytes([int(t, 16)])) for t in pat.split())
    hits = [m.start() + base for m in re.finditer(rx, bytes(img), re.S)]
    if len(hits) != 1:
        raise SystemExit(f"SilentPatch {what}: pattern {pat!r} matches {len(hits)} times (expected 1)")
    return hits[0]


class Img:
    """A PE file's bytes, addressed by VA (only bytes that exist in the file)."""
    def __init__(self, path):
        self.pe = pefile.PE(path)
        self.data = bytearray(open(path, "rb").read())
        self.base = self.pe.OPTIONAL_HEADER.ImageBase

    def off(self, va, n=1):
        for s in self.pe.sections:
            lo = self.base + s.VirtualAddress
            if lo <= va and va + n <= lo + s.SizeOfRawData:
                return s.PointerToRawData + va - lo
        raise SystemExit(f"{va:#x}+{n} is not file-backed")

    def get(self, va, n):
        o = self.off(va, n); return bytes(self.data[o:o + n])

    def put(self, va, b):
        o = self.off(va, len(b)); self.data[o:o + len(b)] = b

    def u32(self, va):
        return struct.unpack("<I", self.get(va, 4))[0]

    def text(self):
        s = self.pe.sections[0]
        return self.base + s.VirtualAddress, bytes(self.data[s.PointerToRawData:s.PointerToRawData + s.SizeOfRawData])

    def iat(self, dll, name):
        for e in self.pe.DIRECTORY_ENTRY_IMPORT:
            if e.dll.decode().lower() == dll.lower():
                for i in e.imports:
                    if i.name and i.name.decode() == name:
                        return i.address
        raise SystemExit(f"no import {dll}!{name}")


def jmp(at, target):
    return b"\xe9" + struct.pack("<i", target - (at + 5))


def call(at, target):
    return b"\xe8" + struct.pack("<i", target - (at + 5))


def main():
    args = sys.argv[1:]
    def opt(name, default):
        if name in args:
            i = args.index(name); v = args[i + 1]; del args[i:i + 2]; return v
        return default
    p_base = int(opt("--p-base", "c00000"), 16)
    bpp = int(opt("--bpp", "16"))
    src, game = args
    lift_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "verok-lift")
    os.makedirs(lift_dir, exist_ok=True)

    exe = Img(os.path.join(src, "nfs2se.exe"))
    dll = Img(os.path.join(src, "nfs2se.p"))
    orig = Img(os.path.join(src, "nfs2se.exe"))

    # ---- Verok: check every original byte against the file first, then patch in order
    for kind, at, val, old in VEROK:
        if old is not None and orig.get(at, len(old) // 2).hex() != old:
            raise SystemExit(f"nfs2se.exe at {at:#x}: {orig.get(at, len(old)//2).hex()} is not the expected {old} (another exe?)")
    for kind, at, val, old in VEROK:
        if kind == "jmp": exe.put(at, jmp(at, p_base + val))
        elif kind == "ptr": exe.put(at, struct.pack("<I", p_base + val))
        elif kind == "nop": exe.put(at, b"\x90" * val)
    for at in BPP_SITES:
        if orig.u32(at) not in (16, 32): raise SystemExit(f"bpp site {at:#x} holds {orig.u32(at)}")
        exe.put(at, struct.pack("<I", bpp))

    # ---- SilentPatch on top (patterns from SilentPatchNFS90s.cpp, NFS2SE parts)
    def sp(pat, what, off=0):
        lo, img = exe.text()
        return pattern(img, lo, pat, what) + off
    notes = []
    # Verok's SetViewports hook takes every race mode but 0 for split screen (online = mode 4):
    # rebuild the function's start and apply the 1px seam fix in the stock code instead
    sv = sp("E9 ? ? ? ? ? ? ? ? ? 75 6B 68 F0 00 00 00", "SetViewports")
    exe.put(sv, bytes([0x53, 0x51, 0x52, 0x83, 0x3D])); exe.put(sv + 0x1B, struct.pack("<I", 241))
    notes.append(f"SetViewports {sv:#x}: Verok's hook undone, seam 241")
    plo = dll.base + dll.pe.sections[0].VirtualAddress
    pimg = dll.data[dll.pe.sections[0].PointerToRawData:][:dll.pe.sections[0].SizeOfRawData]
    gm = pattern(pimg, plo, "83 39 00 74 07 FE C0", "nfs2se.p gameMode") + 2
    dll.put(gm, bytes([0x01, 0x75]))
    notes.append(f"nfs2se.p {gm:#x} (+{gm - P_ORIG:#x}): gameMode != 0 -> == 1")
    # button bitmask used as a count: shl edi,cl / or edx,edi -> inc edx / nop
    a = sp("8B 90 ? ? ? ? D3 E7 09 FA 89 90", "button bitmask", 6); exe.put(a, bytes([0x42, 0x90, 0x90, 0x90])); notes.append(f"buttons {a:#x}")
    # a controller disconnect: the acquire-error handler becomes `return code >= 0`
    a = sp("85 C0 74 16 83 F8 01 74 10 3D 57 00 07 80", "pad disconnect"); exe.put(a, bytes([0xF7, 0xD0, 0xC1, 0xE8, 0x1F, 0xC3])); notes.append(f"disconnect {a:#x}")
    # Poll(): success codes are not errors (je -> jge)
    a = sp("FF 52 64 85 C0 74 ? 3D 1E 00 07 80", "poll", 5); exe.put(a, b"\x7d"); notes.append(f"poll {a:#x}")

    tlo, timg = exe.text()
    # code cave: the free bytes at the end of .text (checked: zero, and nothing in the exe points there)
    cave_lo = (tlo + len(timg.rstrip(b"\0")) + 15) & ~15
    cave_hi = tlo + len(timg)
    for o in range(0, len(timg) - 3):
        v = struct.unpack_from("<I", timg, o)[0]
        if cave_lo <= v < cave_hi: raise SystemExit(f"something at {tlo + o:#x} points into the code cave ({v:#x})")
    cave = [cave_lo]
    def emit(code):
        at = cave[0]; exe.put(at, code); cave[0] += (len(code) + 15) & ~15
        if cave[0] > cave_hi: raise SystemExit("code cave full")
        return at
    prolog = bytes([0x55, 0x89, 0xE5, 0x53, 0x51, 0x52, 0x56, 0x57])          # push ebp / mov ebp,esp / push ebx ecx edx esi edi
    epilog = bytes([0x5F, 0x5E, 0x5A, 0x59, 0x5B, 0x89, 0xEC, 0x5D, 0xC3])    # pop edi esi edx ecx ebx / mov esp,ebp / pop ebp / ret
    def callmem(iat): return b"\xff\x15" + struct.pack("<I", iat)
    def pushmem(va): return b"\xff\x35" + struct.pack("<I", va)

    # mouse jitter: GetDeviceData's jne -> js, and zero eax/edx where the buffer is read
    a = sp("8B 10 50 FF 52 28 89 C2 85 C0 0F 85", "mouse GetDeviceData", 11); exe.put(a, b"\x88")
    z = sp("89 CB C1 E3 04", "mouse zero eax")
    f = emit(bytes([0x89, 0xCB, 0xC1, 0xE3, 0x04, 0x31, 0xC0, 0x31, 0xD2, 0xC3]))
    exe.put(z, call(z, f)); notes.append(f"mouse {a:#x}, {z:#x} -> {f:#x}")

    # movie-thread start race: an event made with the stream mutex, set when the thread is up,
    # waited for (and closed) by the starter
    ce = sp("89 86 A4 00 00 00 E8 ? ? ? ? 89 86 AC 00 00 00", "movie create_event", 6)
    we = sp("8B 86 A4 00 00 00 E8 ? ? ? ? 89 F0", "movie wait_on_event", 6)
    se = sp("31 C0 E8 ? ? ? ? BF 01 00 00 00 31 ED", "movie signal_event", 2)
    var = emit(b"\0" * 4)
    org = (ce + 5 + struct.unpack("<i", exe.get(ce + 1, 4))[0]) & 0xFFFFFFFF
    ev_create, ev_wait = exe.iat("KERNEL32.dll", "CreateEventA"), exe.iat("KERNEL32.dll", "WaitForSingleObject")
    ev_set, ev_close = exe.iat("KERNEL32.dll", "SetEvent"), exe.iat("KERNEL32.dll", "CloseHandle")
    at = cave[0]
    body = prolog + bytes([0x6A, 0x00, 0x6A, 0x01, 0x6A, 0x00, 0x6A, 0x00]) + callmem(ev_create) + b"\xa3" + struct.pack("<I", var)
    body += call(at + len(body), org) + epilog
    f1 = emit(body)
    f2 = emit(prolog + bytes([0x6A, 0xFF]) + pushmem(var) + callmem(ev_wait) + pushmem(var) + callmem(ev_close) + epilog)
    f3 = emit(prolog + pushmem(var) + callmem(ev_set) + epilog)
    exe.put(ce, call(ce, f1)); exe.put(we, call(we, f2)); exe.put(se, call(se, f3))
    notes.append(f"movie race {ce:#x}/{we:#x}/{se:#x} -> {f1:#x}/{f2:#x}/{f3:#x} (event at {var:#x})")
    # not applied: thread affinity (the runtime runs game threads one at a time), Alt+F4 (the
    # browser's), Ctrl+V paste (names come from the page)

    # ---- NoMovies: both versions of whole instructions, for the lifter
    variants = []
    for at, n, old, new in NOMOVIES:
        o = exe.get(at, n)
        if at == 0x40d73b:      # jle rel32 -> nop / jmp rel32 (same target)
            if o[:2] != b"\x0f\x8e": raise SystemExit(f"{at:#x}: {o.hex()} is not jle rel32")
            new = "90e9" + o[2:].hex()
        elif o.hex() != old:
            raise SystemExit(f"{at:#x}: {o.hex()} is not {old}")
        variants.append({"addr": f"{at:#x}", "len": n, "flag": "nomovies", "orig": o.hex(), "new": new})

    # ---- seeds: exe code addresses inside nfs2se.p (hook return points, direct calls)
    seeds = set()
    lo, hi = tlo, tlo + len(timg)
    for s in dll.pe.sections:      # its .data: the jump-back slots of the hooks (.text has the patch
        if s.Name.rstrip(b"\0") != b".data": continue    # sites as arguments, mid-instruction)
        d = s.get_data()
        for o in range(0, len(d) - 3, 4):
            v = struct.unpack_from("<I", d, o)[0]
            if lo <= v < hi: seeds.add(v)
    # direct E8/E9 from nfs2se.p into the exe
    t = dll.pe.sections[0]; d = t.get_data(); tva = dll.base + t.VirtualAddress
    for o in range(len(d) - 4):
        if d[o] in (0xE8, 0xE9):
            v = (tva + o + 5 + struct.unpack_from("<i", d, o + 1)[0]) & 0xFFFFFFFF
            if lo <= v < hi: seeds.add(v)
    with open(os.path.join(lift_dir, "seeds.txt"), "w") as fp:
        fp.write("".join(f"{v:08x}\n" for v in sorted(seeds)))
    json.dump({"p_base": f"{p_base:#x}", "bpp": bpp, "variants": variants, "silentpatch": notes},
              open(os.path.join(lift_dir, "variants.json"), "w"), indent=1)

    # ---- write the game folder
    open(os.path.join(game, "nfs2se.exe"), "wb").write(exe.data)
    open(os.path.join(game, "nfs2se.p"), "wb").write(dll.data)

    def find_ci(root, rel):
        """the existing path for rel under root, matching case-insensitively (or a new one)"""
        cur = root
        for part in rel.split("/"):
            hit = next((e for e in (os.listdir(cur) if os.path.isdir(cur) else []) if e.lower() == part.lower()), None)
            cur = os.path.join(cur, hit or part.upper())
        return cur
    copied = []
    for rel in sorted(os.path.relpath(os.path.join(dp, f), src).replace(os.sep, "/")
                      for dp, _, fs in os.walk(src) for f in fs):
        if re.match(r"(?i)fedata/pc/text/text\.\w+$|gamedata/tracks/pc/3tr\d+\.hrz$", rel):
            dst = find_ci(game, rel); os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(os.path.join(src, rel), dst); copied.append(os.path.relpath(dst, game))
    # the network and modem lobbies' art: this exe asks for it with the language letter in
    # front (enetwork.qfs, emodem.qfs; the retail CD has NETWORK.QFS and MODEM.QFS). Without
    # it, "create game" never draws its lobby.
    art = find_ci(game, "fedata/pc/art")
    for name in ("NETWORK.QFS", "MODEM.QFS"):
        s = find_ci(game, "fedata/pc/art/" + name)
        if os.path.exists(s):
            shutil.copyfile(s, os.path.join(art, "E" + name)); copied.append(os.path.relpath(os.path.join(art, "E" + name), game))
    drv = find_ci(game, "drivers/glide3x"); os.makedirs(drv, exist_ok=True)
    shutil.copyfile(os.path.join(src, "Drivers", "nglide", "voodoo2a.dll"), os.path.join(drv, "voodoo2a.dll"))
    with open(os.path.join(drv, "thrash.ini"), "w", newline="\r\n") as fp:
        fp.write("[THRASH]\nFile=voodoo2a.dll\nType=voodoo\nFogSupport=1\n")
    ini = os.path.join(game, "nfs2se.ini")
    with open(ini, "w", newline="\r\n") as fp:     # the page rewrites ThrashDriver and NoMovies
        fp.write("[NFS2SE]\nLanguage=english\nThrashDriver=glide3x\nWindowedMode=1\nSingleProcAffinity=0\n"
                 "NoErrorReporting=1\nNoMovies=1\nResolution=2\nClassicTextures=0\nFog=1\nShading=1\nFiltering=1\n")
    print(f"nfs2se.exe: {len(VEROK)} Verok patches (nfs2se.p at {p_base:#x}), bpp {bpp}; SilentPatch:")
    for n in notes: print("  " + n)
    print(f"variants: {len(variants)} (NoMovies), seeds: {len(seeds)} -> build/verok-lift/")
    print(f"installed: nfs2se.exe nfs2se.p nfs2se.ini {os.path.relpath(drv, game)}/voodoo2a.dll+thrash.ini, " + ", ".join(copied))


if __name__ == "__main__":
    main()
