"""Recursive-descent function discovery over a PE32 image.

Seeds: entry point, exports, relocated pointers that land in code (vtables,
callbacks, jump tables), and every direct call target found while decoding.
Output: functions -> basic blocks -> decoded instructions.
"""
import capstone as cs
from capstone import x86 as X

JCC = {
    "ja", "jae", "jb", "jbe", "je", "jne", "jg", "jge", "jl", "jle", "jo", "jno",
    "jp", "jnp", "js", "jns", "jcxz", "jecxz", "loop", "loope", "loopne",
}


# never in the game's code: a guessed seed that decodes to one of these is data
JUNK = {"in", "out", "insb", "insw", "insd", "outsb", "outsw", "outsd", "arpl", "bound", "into", "int1",
        "aaa", "aas", "daa", "das", "aam", "aad", "lcall", "ljmp", "hlt", "cli", "sti", "les", "lds", "salc"}


class Insn:
    __slots__ = ("addr", "size", "mnem", "ops", "raw", "text")

    def __init__(self, i):
        self.addr, self.size, self.mnem = i.address, i.size, i.mnemonic
        if self.mnem.startswith("notrack "):     # a 3E (ds:) prefix on call/jmp: no effect
            self.mnem = self.mnem[8:]
        self.ops = list(i.operands)
        self.raw = i
        self.text = f"{i.mnemonic} {i.op_str}".strip()


class Func:
    def __init__(self, start):
        self.start = start
        self.blocks = {}  # start va -> [Insn]
        self.calls = set()
        self.indirect_jumps = []  # unresolved jmp addrs
        self.jump_tables = {}  # jmp insn addr -> [targets]
        self.bad = None


class Program:
    def __init__(self, img):
        self.img = img
        self.md = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_32)
        self.md.detail = True
        self.insns = {}  # va -> Insn (global decode cache)
        self.funcs = {}
        self.reloc_set = set(img.relocs)
        self.dropped = 0
        self.junk_seeds = set()

    def decode(self, va):
        if va in self.insns:
            return self.insns[va]
        if not self.img.is_code(va):
            return None
        for i in self.md.disasm(self.img.bytes(va, 16), va, 1):
            ins = Insn(i)
            self.insns[va] = ins
            return ins
        return None

    def jump_table(self, ins):
        """jmp dword ptr [reg*4 + table]: read consecutive relocated entries."""
        op = ins.ops[0]
        if op.type != X.X86_OP_MEM:
            return None
        # jmp [reg*4 + table], or jmp [reg + table] with a byte offset in reg (Watcom's
        # math library: `or ebx, 4` / `jmp cs:[ebx + table]`)
        if not (op.mem.scale == 4 and op.mem.base == 0 or op.mem.index == 0 and op.mem.base != 0):
            return None
        tbl = op.mem.disp & 0xFFFFFFFF
        out = []
        a = tbl
        while a in self.reloc_set and self.img.is_code(self.img.u32(a)):
            out.append(self.img.u32(a))
            a += 4
        return out or None

    def explore(self, start):
        if start in self.funcs:
            return self.funcs[start]
        f = Func(start)
        self.funcs[start] = f
        work = [start]
        seen = set()
        while work:
            b = work.pop()
            if b in seen:
                continue
            seen.add(b)
            block, va = [], b
            while True:
                ins = self.decode(va)
                if ins is None:
                    f.bad = f"undecodable at {va:#x}"
                    break
                block.append(ins)
                m = ins.mnem
                nxt = va + ins.size
                op0 = ins.ops[0] if ins.ops else None
                if m == "call":
                    if op0.type == X.X86_OP_IMM:
                        f.calls.add(op0.imm & 0xFFFFFFFF)
                elif m == "jmp":
                    if op0.type == X.X86_OP_IMM:
                        t = op0.imm & 0xFFFFFFFF
                        if self.img.is_code(t):
                            work.append(t)
                        else:                # into another module (Verok's hooks): a tail call
                            f.calls.add(t)
                    else:
                        jt = self.jump_table(ins)
                        if jt:
                            f.jump_tables[ins.addr] = jt
                            work.extend(jt)
                        else:
                            f.indirect_jumps.append(ins.addr)
                    break
                elif m in JCC:
                    work.append(op0.imm & 0xFFFFFFFF)
                    work.append(nxt)
                    break
                elif m in ("ret", "retf", "iret", "iretd", "hlt", "int3", "ud2"):
                    break
                va = nxt
                if va in seen or va in f.blocks:  # fallthrough into existing block
                    break
            f.blocks[b] = block
        return f

    def looks_like_data(self, t):
        """an IAT slot or a C string (MSVC merges both into .text)"""
        if t in self.img.imports:
            return True
        b = self.img.bytes(t, 64)
        n = b.find(b"\0")
        return n >= 3 and all(32 <= c < 127 or c in (9, 10, 13) for c in b[:n])

    def junk(self, f):
        """decoded data: undecodable, or instructions game code never has"""
        return f.bad or any(i.mnem in JUNK for blk in f.blocks.values() for i in blk)

    def run(self, extra_seeds=()):
        img = self.img
        seeds = {img.entry, *img.exports, *extra_seeds}
        sure = set(seeds)
        # Relocated pointers into code that are not inside an instruction stream
        # are likely function pointers (vtables/callbacks) or jump-table entries.
        # Includes relocs inside code: immediates like `push offset callback`.
        for r in img.relocs:
            if img.relocs_scanned and r not in img.scanned_data:
                continue                 # scanned, in code: only once it is an instruction operand
            t = img.u32(r)
            if img.is_code(t) and t not in sure and not self.looks_like_data(t):
                seeds.add(t)
        self.explore_all(seeds, sure)
        if img.relocs_scanned:
            # code pointers that are immediates of decoded instructions (push offset callback,
            # mov [x], offset fn), until no new ones. Not displacements: those are data kept in
            # the code section (tables, constants).
            while True:
                new = set()
                for ins in (i for f in list(self.funcs.values()) for blk in f.blocks.values() for i in blk):
                    for op in ins.ops:
                        v = (op.imm if op.type == X.X86_OP_IMM else 0) & 0xFFFFFFFF
                        if img.is_code(v) and v not in self.funcs and v not in self.junk_seeds and ins.mnem not in JCC and ins.mnem not in ("jmp", "call"):
                            new.add(v)
                if not new:
                    # pointer tables kept in the code section: aligned code pointers that are
                    # not inside any decoded instruction
                    inside = {a for f in self.funcs.values() for blk in f.blocks.values() for i in blk for a in range(i.addr, i.addr + i.size)}
                    for r in img.relocs:
                        if r % 4 == 0 and r not in img.scanned_data and r not in inside:
                            t = img.u32(r)
                            if img.is_code(t) and t not in self.funcs and t not in self.junk_seeds and not self.looks_like_data(t):
                                new.add(t)
                    if not new:
                        break
                self.explore_all(new, set())
        self.split_overlaps()
        return self.funcs

    def explore_all(self, seeds, sure):
        """seeds not in `sure` are guesses (pointer-shaped values): dropped if they decode as junk.
        Their callees, and everything reached by a direct call, are kept."""
        img = self.img
        pending = [(s, s in sure) for s in seeds]
        while pending:
            s, certain = pending.pop()
            if s in self.funcs:
                continue
            f = self.explore(s)
            if not certain and self.junk(f):
                del self.funcs[s]
                self.junk_seeds.add(s)
                self.dropped += 1
                continue
            pending.extend((c, True) for c in f.calls if c not in self.funcs and img.is_code(c))

    def split_overlaps(self):
        """A jump table target or tail-jump may land inside another function's
        block; both views stay valid in C (each function owns its own copy)."""
        pass
