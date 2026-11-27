"""PE32 image loader: sections, relocations, imports, exports as a flat virtual image."""
import struct
import pefile


class Image:
    def __init__(self, path, base=None, scan_relocs=False):
        pe = pefile.PE(path)
        if base is not None and base != pe.OPTIONAL_HEADER.ImageBase:
            pe.relocate_image(base)  # applies .reloc fixups and updates ImageBase
        self.pe = pe
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.size = pe.OPTIONAL_HEADER.SizeOfImage
        self.entry = self.base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
        self.mem = bytearray(self.size)
        hdr = pe.OPTIONAL_HEADER.SizeOfHeaders
        self.mem[:hdr] = pe.__data__[:hdr]
        self.sections = []
        for s in pe.sections:
            name = s.Name.rstrip(b"\0").decode()
            va = self.base + s.VirtualAddress
            data = s.get_data()[: max(s.Misc_VirtualSize, 0) or s.SizeOfRawData]
            off = s.VirtualAddress
            self.mem[off : off + len(data)] = data
            self.sections.append((name, va, max(s.Misc_VirtualSize, s.SizeOfRawData), s.Characteristics))
        code = [s for s in self.sections if s[3] & 0x20000000]  # IMAGE_SCN_MEM_EXECUTE
        self.code_lo = min(s[1] for s in code)
        self.code_hi = max(s[1] + s[2] for s in code)

        # Relocations: VAs of 32-bit absolute pointers in the image.
        self.relocs = []
        if hasattr(pe, "DIRECTORY_ENTRY_BASERELOC") and not scan_relocs:
            for blk in pe.DIRECTORY_ENTRY_BASERELOC:
                for e in blk.entries:
                    if e.type == 3:  # HIGHLOW
                        self.relocs.append(self.base + e.rva)
        # No relocations (Verok's nfs2se.exe has them zeroed): stand-ins from a scan. Code: every
        # byte offset holding a pointer into code; data: aligned. Measured on NFS2SEA.EXE against
        # its real ones: all code pointers found in code, 682/690 in data (docs/verok-plan.md).
        # discover.py seeds only the data ones directly, and code ones that turn out to be
        # instruction operands.
        self.relocs_scanned = not self.relocs and (scan_relocs or not pe.FILE_HEADER.Characteristics & 0x2000)
        self.scanned_data = set()
        if self.relocs_scanned:
            raw = {self.base + s.VirtualAddress: s.PointerToRawData for s in pe.sections}
            for name, va, size, ch in self.sections:
                if name == ".reloc" or not size or ch & 0x80 or not raw[va]:
                    continue                 # .bss (NFS2SEA's claims file bytes from offset 0)
                o0 = va - self.base
                end = min(o0 + size, len(self.mem)) - 3
                code = ch & 0x20000000
                hits = [o for o in range(o0, end) if self.code_lo <= struct.unpack_from("<I", self.mem, o)[0] < self.code_hi]
                hitset = set(hits)
                for o in hits:
                    if code:
                        self.relocs.append(self.base + o)
                    # unaligned (packed structs): only what starts like a function, or a run of
                    # records (Watcom's startup/shutdown routine table has 6-byte ones)
                    elif o % 4 == 0 or self._prologue(struct.unpack_from("<I", self.mem, o)[0]) or \
                            any(o + k in hitset or o - k in hitset for k in (6, 8, 12, 16)):
                        self.relocs.append(self.base + o)
                        self.scanned_data.add(self.base + o)

        # Imports: IAT slot VA -> "DLL!name"
        self.imports = {}
        for e in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
            dll = e.dll.decode().upper()
            for i in e.imports:
                n = i.name.decode() if i.name else f"#{i.ordinal}"
                self.imports[i.address] = f"{dll}!{n}"

        self.exports = {}
        if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
            for s in pe.DIRECTORY_ENTRY_EXPORT.symbols:
                self.exports[self.base + s.address] = s.name.decode() if s.name else f"#{s.ordinal}"

    def _prologue(self, va):
        b = self.mem[va - self.base : va - self.base + 3]
        return len(b) == 3 and (0x50 <= b[0] <= 0x57 or b[0] in (0x81, 0x83) and b[1] == 0xEC)

    def is_code(self, va):
        return self.code_lo <= va < self.code_hi

    def in_image(self, va):
        return self.base <= va < self.base + self.size

    def u32(self, va):
        o = va - self.base
        return struct.unpack_from("<I", self.mem, o)[0]

    def bytes(self, va, n):
        o = va - self.base
        return bytes(self.mem[o : o + n])
