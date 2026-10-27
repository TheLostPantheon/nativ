#!/usr/bin/env python3
"""NEON/VFP lane semantics of the recompiler against a model of the ARM ARM (ARMv7-A).

Each case is a short Thumb-2 sequence: clang assembles it (its ARM backend, -target armv7a), capstone
decodes it the way recomp.py does, Fn.build() translates it to C, and a tiny harness runs the C over
initialised Cpu states (plus a 4 KiB window of guest memory) piped through stdin. The same states go
through the Python model below, written from the ARM ARM's pseudocode (not from the recompiler, and not
from Unicorn, whose NEON crashes on Apple Silicon). Inputs are random with edge values mixed in per
byte: saturation bounds, negative and oversize shift counts, out-of-range table indices, f32
denormals, NaNs, infinities and the two zeros.

usage: tests/neon_test.py [-n ITER] [-k PATTERN] [-v]      (from the repository root, with .venv's python)
"""
import os, random, struct, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "recompiler"))
import recomp
from recomp import md, Fn
from capstone.arm import ARM_OP_REG, ARM_OP_IMM, ARM_OP_MEM

MEMBASE, MEMSZ = 0x1000, 0x1000
CPU_FMT = "<16I6I64I2I"   # r[16]; n z c v q pad; vfp.s[64]; fpscr stop  (rt.h's Cpu)
assert struct.calcsize(CPU_FMT) == 352

# ---------------------------------------------------------------- assembling and translating
def assemble(asm):
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, "t.s"); obj = os.path.join(td, "t.o")
        open(src, "w").write(".syntax unified\n.thumb\n.text\n" + asm + "\n")
        subprocess.run(["clang", "-target", "armv7a-none-eabi", "-mfpu=neon", "-c", src, "-o", obj], check=True)
        d = open(obj, "rb").read()
    shoff = struct.unpack_from("<I", d, 0x20)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2e)
    secs = [struct.unpack_from("<10I", d, shoff + k * shentsize) for k in range(shnum)]
    stro = secs[shstrndx][4]
    for sh in secs:
        name = d[stro + sh[0]:d.index(b"\0", stro + sh[0])]
        if name == b".text":
            return d[sh[4]:sh[4] + sh[5]]
    raise RuntimeError("no .text")

def translate(entry, code):
    insns = list(md.disasm(code, entry))
    assert sum(i.size for i in insns) == len(code), "undecodable bytes"
    f = Fn(entry, insns, {entry})
    lines = f.build()
    return insns, lines, f.unimpl

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include "rt.h"
#define JUMP(t) do { c->r[15] = (t); goto jump; } while (0)
uint8_t *recomp_mem;
__thread uint32_t recomp_excl_addr, recomp_excl_val; __thread int recomp_excl_valid;
__thread uint64_t recomp_excl_val64; __thread uint32_t recomp_tpidruro;
void recomp_call_external(Cpu *c, uint32_t a) { (void)a; c->stop = 2; }
void recomp_unimplemented(Cpu *c, uint32_t a) { (void)a; c->stop = 1; }
GuestFn recomp_lookup(uint32_t a) { (void)a; return 0; }
const char *recomp_import_name(uint32_t a) { (void)a; return 0; }
_Static_assert(sizeof(Cpu) == 352, "Cpu layout");
%s
static GuestFn fns[] = { %s };
int main(void) {
	recomp_mem = calloc(1, 1 << 16);
	uint32_t idx; Cpu c; static uint8_t m[%d];
	while (fread(&idx, 4, 1, stdin) == 1) {
		if (fread(&c, sizeof c, 1, stdin) != 1 || fread(m, sizeof m, 1, stdin) != 1) return 1;
		memcpy(recomp_mem + %d, m, sizeof m);
		fns[idx](&c);
		fwrite(&c, sizeof c, 1, stdout); fwrite(recomp_mem + %d, sizeof m, 1, stdout);
	}
	return 0;
}
'''

# ---------------------------------------------------------------- the model (ARM ARM semantics)
M32 = 0xffffffff
def sx(v, bits): v &= (1 << bits) - 1; return v - (1 << bits) if v >> (bits - 1) else v
def ux(v, bits): return v & ((1 << bits) - 1)

class State:
    def __init__(self, r, s, mem):
        self.r, self.s, self.mem, self.pc = list(r), list(s), bytearray(mem), None
        self.n = self.z = self.c = self.v = 0
    # register files as integers (little-endian lane order: element 0 is the low bits)
    def reg(self, name):
        if name[0] == "q": n = 4 * int(name[1:]); return sum(self.s[n + k] << (32 * k) for k in range(4))
        if name[0] == "d": n = 2 * int(name[1:]); return self.s[n] | self.s[n + 1] << 32
        return self.s[int(name[1:])]
    def setreg(self, name, v):
        if name[0] == "q": n, w = 4 * int(name[1:]), 4
        elif name[0] == "d": n, w = 2 * int(name[1:]), 2
        else: n, w = int(name[1:]), 1
        for k in range(w): self.s[n + k] = (v >> (32 * k)) & M32
    def rd(self, a, n): return int.from_bytes(self.mem[a - MEMBASE:a - MEMBASE + n], "little")
    def wr(self, a, n, v): self.mem[a - MEMBASE:a - MEMBASE + n] = ux(v, 8 * n).to_bytes(n, "little")

def elems(v, width, es, signed=False):
    return [(sx if signed else ux)(v >> (es * k), es) for k in range(width // es)]
def pack(vals, es): return sum(ux(x, es) << (es * k) for k, x in enumerate(vals))
def width(name): return 128 if name[0] == "q" else 64

# f32 as bits: FZF flushes denormals to a signed zero and NaNs to the default NaN
def fzf(b):
    if (b & 0x7f800000) == 0: return b & 0x80000000
    if (b & 0x7f800000) == 0x7f800000 and (b & 0x7fffff): return 0x7fc00000
    return b
def f32(b): return struct.unpack("<f", struct.pack("<I", b))[0]
def fmax(a, b):
    a, b = fzf(a), fzf(b)
    if (a & 0x7fffffff) > 0x7f800000 or (b & 0x7fffffff) > 0x7f800000: return 0x7fc00000
    x, y = f32(a), f32(b)
    if x == y: return a & b   # FPMax: +0 wins over -0
    return a if x > y else b
def fmin(a, b):
    a, b = fzf(a), fzf(b)
    if (a & 0x7fffffff) > 0x7f800000 or (b & 0x7fffffff) > 0x7f800000: return 0x7fc00000
    x, y = f32(a), f32(b)
    if x == y: return a | b   # FPMin: -0 wins
    return a if x < y else b
def fadd(a, b):
    a, b = fzf(a), fzf(b)
    r = struct.unpack("<I", struct.pack("<f", f32(a) + f32(b)))[0] if not (f32(a) != f32(a) or f32(b) != f32(b)) else 0x7fc00000
    return fzf(r)

def sat(v, bits, signed):
    if signed: return max(-(1 << (bits - 1)), min((1 << (bits - 1)) - 1, v))
    return max(0, min((1 << bits) - 1, v))

def model(st, insns):
    """runs the sequence on the state: straight line, with 'add pc, rN' as a computed jump"""
    by_addr = {i.address: i for i in insns}
    pc = insns[0].address
    while pc in by_addr:
        i = by_addr[pc]
        pc = i.address + i.size
        mn = i.mnemonic.split(".")[0]
        dt = i.mnemonic.split(".")[1] if "." in i.mnemonic else ""
        ops = list(i.operands)
        names = [md.reg_name(o.reg) if o.type == ARM_OP_REG else None for o in ops]
        imm = [o.imm for o in ops if o.type == ARM_OP_IMM]
        em = dt and dt[0] in "usi" and dt[1:].isdigit()
        sg = dt[:1] == "s"
        bits = int(dt[1:]) if em else (int(dt) if dt.isdigit() else 0)
        if mn in ("movs", "adds") and ops[1].type == ARM_OP_IMM:
            d = int(names[0][1:])
            if mn == "movs": r = imm[0] & M32; st.n, st.z = r >> 31, int(r == 0)
            else:
                a, b = st.r[d], imm[0] & M32; u = a + b; r = u & M32
                st.n, st.z, st.c = r >> 31, int(r == 0), u >> 32
                st.v = int(((a ^ r) & (b ^ r)) >> 31 != 0)
            st.r[d] = r; continue
        if mn == "add" and names[0] == "pc":
            pc = (i.address + 4 + st.r[recomp.GPR[names[1]]]) & M32 & ~1; continue
        if mn == "rbit":
            v = st.r[recomp.GPR[names[1]]]; st.r[recomp.GPR[names[0]]] = int(f"{v:032b}"[::-1], 2); continue
        if mn == "ubfx":
            st.r[recomp.GPR[names[0]]] = (st.r[recomp.GPR[names[1]]] >> imm[0]) & ((1 << imm[1]) - 1); continue
        D = names[0]
        if mn in ("vld2", "vst2", "vld3", "vst3", "vld4", "vst4"):
            ds = [n for n in names if n and n[0] == "d"]
            es = bits // 8
            mem_op = [o for o in ops if o.type == ARM_OP_MEM][0]
            base = recomp.GPR[md.reg_name(mem_op.mem.base)]
            post = [n for k, n in enumerate(names) if n in recomp.GPR and k > 0]
            a = ea = st.r[base]
            lane = i.op_str.split("[")[1].split("]")[0] if i.op_str.startswith("{d") and "[" in i.op_str.split("}")[0] else None
            nst = int(mn[-1])
            if lane is not None:
                moves = [(int(lane), d) for d in ds]
            else:
                groups = [(ds[0], ds[2]), (ds[1], ds[3])] if nst == 2 and len(ds) == 4 else [tuple(ds)]
                moves = [(e, d) for g in groups for e in range(8 // es) for d in g]
            for e, d in moves:
                v = st.reg(d)
                if mn.startswith("vld"):
                    x = st.rd(a, es); v = (v & ~(((1 << bits) - 1) << (bits * e))) | (x << (bits * e)); st.setreg(d, v)
                else:
                    st.wr(a, es, (v >> (bits * e)) & ((1 << bits) - 1))
                a += es
            if post: st.r[base] = (ea + st.r[recomp.GPR[post[-1]]]) & M32
            elif i.writeback: st.r[base] = (ea + es * len(moves)) & M32
            continue
        if mn in ("vtbl", "vtbx"):
            tab = b"".join(st.reg(n).to_bytes(8, "little") for n in names[1:-1])
            ix = st.reg(names[-1]).to_bytes(8, "little"); old = st.reg(D).to_bytes(8, "little")
            st.setreg(D, int.from_bytes(bytes(tab[x] if x < len(tab) else (old[k] if mn == "vtbx" else 0) for k, x in enumerate(ix)), "little"))
            continue
        if mn == "vdup":   # element x of dM, or the low element of a core register, everywhere
            e = ux(st.reg(names[1]) >> (bits * ops[1].vector_index), bits) if names[1][0] == "d" else ux(st.r[recomp.GPR[names[1]]], bits)
            st.setreg(D, pack([e] * (width(D) // bits), bits)); continue
        W = width(D)
        if mn in ("vraddhn", "vaddhn", "vrsubhn", "vsubhn"):
            a, b = elems(st.reg(names[1]), 128, bits), elems(st.reg(names[2]), 128, bits)
            rnd = (1 << (bits // 2 - 1)) if mn[1] == "r" else 0
            op = (lambda x, y: x + y) if "add" in mn else (lambda x, y: x - y)
            st.setreg(D, pack([(op(x, y) + rnd) >> (bits // 2) for x, y in zip(a, b)], bits // 2)); continue
        if mn in ("addw", "subw", "add", "sub") and len(names) == 3 and names[1] == "pc" and imm:   # Thumb: Align(pc+4, 4) +/- imm
            v = ((i.address + 4) & ~3) + (imm[0] if mn in ("add", "addw") else -imm[0])
            st.r[recomp.GPR[names[0]]] = v & M32; continue
        if mn == "vmull":
            a, b = elems(st.reg(names[1]), 64, bits, sg), elems(st.reg(names[2]), 64, bits, sg)
            st.setreg(D, pack([x * y for x, y in zip(a, b)], 2 * bits)); continue
        if mn == "vshrn":
            a = elems(st.reg(names[1]), 128, bits)
            st.setreg(D, pack([x >> imm[0] for x in a], bits // 2)); continue
        if mn == "vmov" and names[0] and names[0][0] == "r" and names[1] and names[1][0] == "d" and em:
            e = elems(st.reg(names[1]), 64, bits, sg)[ops[1].vector_index]
            st.r[recomp.GPR[names[0]]] = e & M32; continue
        if mn in ("vaddl", "vsubl", "vaddw", "vsubw"):
            a = elems(st.reg(names[1]), 64, bits, sg) if mn[-1] == "l" else elems(st.reg(names[1]), 128, 2 * bits, sg)
            b = elems(st.reg(names[2]), 64, bits, sg)
            op = (lambda x, y: x + y) if "add" in mn else (lambda x, y: x - y)
            st.setreg(D, pack([op(x, y) for x, y in zip(a, b)], 2 * bits)); continue
        if dt == "f32":
            a = elems(st.reg(names[1]), W, 32)
            b = elems(st.reg(names[2]), W, 32) if len(names) > 2 and names[2] else None
            if mn in ("vmax", "vmin"): r = [(fmax if mn == "vmax" else fmin)(x, y) for x, y in zip(a, b)]
            elif mn in ("vpmax", "vpmin", "vpadd"):
                f = {"vpmax": fmax, "vpmin": fmin, "vpadd": fadd}[mn]
                r = [f(a[0], a[1]), f(b[0], b[1])]
            elif mn in ("vcgt", "vcge", "vclt", "vcle", "vceq"):
                cmpf = {"vcgt": lambda x, y: x > y, "vcge": lambda x, y: x >= y, "vclt": lambda x, y: x < y, "vcle": lambda x, y: x <= y, "vceq": lambda x, y: x == y}[mn]
                r = [M32 if cmpf(f32(fzf(x)), f32(fzf(y))) else 0 for x, y in zip(a, b)]
            else: raise NotImplementedError(i.mnemonic)
            st.setreg(D, pack(r, 32)); continue
        # integer elements
        if bits == 64 and mn in ("vshr", "vsra", "vrshr", "vrsra", "vshl"):
            n = imm[0]; a = elems(st.reg(names[1]), W, 64, sg); d = elems(st.reg(D), W, 64)
            def shr(x):
                if mn == "vshl": return x << n
                if mn in ("vshr", "vsra"): return x >> n
                return (x + (1 << (n - 1))) >> n
            r = [shr(x) + (dk if mn in ("vsra", "vrsra") else 0) for x, dk in zip(a, d)]
            st.setreg(D, pack(r, 64)); continue
        a = elems(st.reg(names[1]), W, bits, sg)
        b = elems(st.reg(names[2]), W, bits, sg) if len(names) > 2 and names[2] and names[2][0] in "dq" else None
        d = elems(st.reg(D), W, bits, sg)
        mask = (1 << bits) - 1
        cmps = {"vcgt": lambda x, y: x > y, "vcge": lambda x, y: x >= y, "vclt": lambda x, y: x < y, "vcle": lambda x, y: x <= y, "vceq": lambda x, y: x == y}
        if mn in cmps:
            if b is None: b = [0] * len(a)
            r = [mask if cmps[mn](x, y) else 0 for x, y in zip(a, b)]
        elif mn in ("vmax", "vmin"): r = [max(x, y) if mn == "vmax" else min(x, y) for x, y in zip(a, b)]
        elif mn in ("vpmax", "vpmin", "vpadd"):
            f = {"vpmax": max, "vpmin": min, "vpadd": lambda x, y: x + y}[mn]
            r = [f(a[2 * k], a[2 * k + 1]) for k in range(len(a) // 2)] + [f(b[2 * k], b[2 * k + 1]) for k in range(len(b) // 2)]
        elif mn == "vadd": r = [x + y for x, y in zip(a, b)]
        elif mn == "vsub": r = [x - y for x, y in zip(a, b)]
        elif mn == "vmul": r = [x * y for x, y in zip(a, b)]
        elif mn == "vmla": r = [dk + x * y for x, y, dk in zip(a, b, d)]
        elif mn == "vmls": r = [dk - x * y for x, y, dk in zip(a, b, d)]
        elif mn == "vhadd": r = [(x + y) >> 1 for x, y in zip(a, b)]
        elif mn == "vhsub": r = [(x - y) >> 1 for x, y in zip(a, b)]
        elif mn == "vqadd": r = [sat(x + y, bits, sg) for x, y in zip(a, b)]
        elif mn == "vqsub": r = [sat(x - y, bits, sg) for x, y in zip(a, b)]
        elif mn == "vtst": r = [mask if (x & y) & mask else 0 for x, y in zip(a, b)]
        elif mn == "vneg": r = [-x for x in a]
        elif mn == "vabs": r = [abs(x) for x in a]
        elif mn in ("vshl", "vqshl") and b is not None:   # by register: SInt(low byte), negative = right shift
            r = []
            for x, y in zip(a, b):
                sh = sx(y, 8)
                v = (x << sh) if sh >= 0 else (x >> -sh)   # python shifts are arithmetic on ints
                r.append(sat(v, bits, sg) if mn == "vqshl" else v)
        elif mn in ("vshl", "vshr", "vsra", "vrshr", "vrsra", "vqshl"):
            n = imm[0]
            def shr(x):
                if mn == "vshl": return x << n
                if mn == "vqshl": return sat(x << n, bits, sg)
                if mn in ("vshr", "vsra"): return x >> n
                return (x + (1 << (n - 1))) >> n
            r = [shr(x) + (dk if mn in ("vsra", "vrsra") else 0) for x, dk in zip(a, d)]
        else: raise NotImplementedError(i.mnemonic)
        st.setreg(D, pack(r, bits))

# ---------------------------------------------------------------- inputs
EDGE_BYTES = [0, 1, 2, 3, 7, 8, 15, 16, 31, 32, 63, 64, 0x7f, 0x80, 0x81, 0xc0, 0xe0, 0xf0, 0xf8, 0xf9, 0xfe, 0xff]
EDGE_WORDS = [0, 1, 0x7fffffff, 0x80000000, 0xffffffff, 0x7fff7fff, 0x80008000, 0x7f800000, 0xff800000,
              0x7fc00000, 0x7f812345, 0x00000001, 0x00400000, 0x807fffff, 0x3f800000, 0xbf800000, 0x00800000, 0x0000ffff]
def rword(rng):
    p = rng.random()
    if p < 0.25: return rng.choice(EDGE_WORDS)
    if p < 0.7: return sum(rng.choice(EDGE_BYTES) << (8 * k) for k in range(4))
    return rng.getrandbits(32)
def inputs(rng):
    s = [rword(rng) for _ in range(64)]
    r = [rng.getrandbits(32) for _ in range(16)]
    for k in (0, 1, 9): r[k] = MEMBASE + 8 * rng.randrange(0, 0x180)   # bases into the memory window
    for k in (2, 7): r[k] = rng.randrange(0, 64)                       # post-index amounts
    r[13] = MEMBASE + 0x800
    mem = bytes(rng.getrandbits(8) for _ in range(MEMSZ))
    return r, s, mem

# ---------------------------------------------------------------- cases
CASES = [
    ("movs r3, #0; addw ip, pc, #7", None), ("addw ip, pc, #0x4f", None), ("movs r3, #0; subw r0, pc, #9", None), ("movs r3, #1; add r1, pc, #8", None),
    ("vmull.u8 q0, d1, d2", None), ("vmull.s8 q3, d4, d5", None), ("vmull.u16 q0, d1, d2", None), ("vmull.s16 q0, d1, d2", None),
    ("vmull.u32 q0, d1, d2", None), ("vmull.s32 q2, d6, d7", None),
    ("vshrn.i16 d0, q1, #3", None), ("vshrn.i16 d0, q1, #8", None), ("vshrn.i32 d0, q1, #16", None), ("vshrn.i32 d5, q3, #1", None), ("vshrn.i64 d0, q1, #32", None),
    ("vmov.u16 r0, d3[2]", None), ("vmov.s16 r1, d3[1]", None), ("vmov.u8 r2, d4[0]", None), ("vmov.s8 r3, d4[7]", None), ("vmov.u16 r4, d9[3]", None),
    # the forms the games hit (Uncharted, Unit 13) and their siblings
    ("vmin.u32 d4, d4, d5", None), ("vmax.u32 d4, d4, d27", None), ("vmin.s32 q0, q1, q2", None), ("vmax.s16 d0, d1, d2", None),
    ("vmin.u8 q3, q4, q5", None), ("vmax.s8 d9, d10, d11", None), ("vmin.u16 d0, d0, d0", None),
    ("vcgt.u8 d15, d16, d8", None), ("vcge.u8 d15, d16, d4", None), ("vcgt.s8 q0, q1, q2", None), ("vcge.s16 d0, d1, d2", None),
    ("vcgt.u32 d0, d1, d2", None), ("vcge.s32 q0, q1, q2", None), ("vclt.s8 d0, d1, #0", None), ("vcle.s16 q0, q1, #0", None),
    ("vcgt.s32 d0, d1, #0", None), ("vceq.i8 d0, d1, d2", None), ("vceq.i16 q0, q1, q2", None), ("vceq.i32 d0, d1, #0", None),
    ("vcgt.f32 d0, d1, d2", None), ("vcge.f32 q0, q1, q2", None),
    ("vshr.u8 q4, q3, #2", None), ("vshr.u8 d0, d1, #8", None), ("vshr.s8 d0, d1, #8", None), ("vshr.s16 d0, d24, #8", None),
    ("vshr.s16 d0, d24, #4", None), ("vshr.u16 q0, q1, #16", None), ("vshr.s32 d0, d1, #32", None), ("vshr.u32 d0, d1, #32", None),
    ("vshl.i8 q5, q3, #2", None), ("vshl.i8 d0, d1, #7", None), ("vshl.i16 d0, d1, #15", None), ("vshl.i32 q0, q1, #31", None),
    ("vsra.s32 d24, d26, #9", None), ("vsra.s16 d8, d26, #7", None), ("vsra.u32 d17, d0, #1", None), ("vsra.u8 q0, q1, #3", None),
    ("vsra.s8 d0, d1, #8", None), ("vsra.u16 d0, d1, #16", None), ("vsra.s32 q0, q1, #32", None),
    ("vrshr.u8 d0, d1, #3", None), ("vrshr.s8 d0, d1, #8", None), ("vrshr.u16 q0, q1, #16", None), ("vrshr.s32 d0, d1, #32", None),
    ("vrshr.u32 d0, d1, #32", None), ("vrshr.s16 d0, d1, #1", None),
    ("vrsra.u64 d15, d16, #49", None), ("vrsra.u64 d15, d16, #64", None), ("vrsra.s64 d0, d1, #64", None), ("vrsra.s64 q0, q1, #1", None),
    ("vrsra.u64 d0, d1, #1", None), ("vrsra.u32 d0, d1, #32", None), ("vrsra.s8 d0, d1, #8", None), ("vrsra.u16 q0, q1, #5", None),
    ("vsra.u64 d0, d1, #64", None), ("vsra.s64 d0, d1, #3", None), ("vrshr.u64 d0, d1, #64", None), ("vrshr.s64 q0, q1, #63", None),
    ("vqshl.u16 d31, d4, d5", None), ("vqshl.s8 q1, q2, q3", None), ("vqshl.u8 d0, d1, d2", None), ("vqshl.s16 d0, d1, d2", None),
    ("vqshl.u32 d0, d1, d2", None), ("vqshl.s32 q0, q1, q2", None),
    ("vqshl.u16 d1, d2, #3", None), ("vqshl.s8 d0, d1, #7", None), ("vqshl.u32 q0, q1, #31", None), ("vqshl.s32 d0, d1, #0", None),
    ("vshl.u8 d0, d1, d2", None), ("vshl.s16 q0, q1, q2", None), ("vshl.s8 d0, d1, d2", None),
    ("vqadd.s8 d16, d14, d24", None), ("vqadd.s16 d16, d2, d24", None), ("vqadd.u8 q0, q1, q2", None), ("vqadd.u16 d0, d1, d2", None),
    ("vqadd.s32 d0, d1, d2", None), ("vqadd.u32 q0, q1, q2", None), ("vqsub.s8 d0, d1, d2", None), ("vqsub.u16 q0, q1, q2", None),
    ("vqsub.s32 d0, d1, d2", None), ("vqsub.u32 d0, d1, d2", None),
    ("vhadd.u16 d16, d12, d7", None), ("vhadd.u32 d16, d0, d7", None), ("vhadd.s8 q0, q1, q2", None), ("vhadd.s32 d0, d1, d2", None),
    ("vhsub.u8 d0, d1, d2", None), ("vhsub.s16 q0, q1, q2", None), ("vhsub.s32 d0, d1, d2", None), ("vhsub.u32 d0, d1, d2", None),
    ("vpmin.f32 d15, d31, d29", None), ("vpmax.f32 d15, d0, d0", None), ("vpadd.f32 d0, d1, d2", None),
    ("vmax.f32 d0, d1, d2", None), ("vmin.f32 q0, q1, q2", None),
    ("vpmax.u8 d0, d1, d2", None), ("vpmin.s16 d0, d1, d2", None), ("vpmax.s32 d0, d1, d2", None), ("vpmin.u32 d0, d1, d2", None),
    ("vpadd.i8 d0, d1, d2", None), ("vpadd.i16 d0, d1, d2", None), ("vpadd.i32 d0, d1, d2", None),
    ("vraddhn.i32 d15, q5, q10", None), ("vraddhn.i16 d0, q1, q2", None), ("vraddhn.i64 d0, q1, q2", None),
    ("vaddhn.i32 d0, q1, q2", None), ("vrsubhn.i16 d0, q1, q2", None), ("vsubhn.i64 d0, q1, q2", None), ("vrsubhn.i32 d0, q0, q1", None),
    ("vaddl.u8 q8, d0, d0", None), ("vaddl.s8 q0, d1, d2", None), ("vaddl.u16 q0, d1, d2", None), ("vaddl.s16 q0, d1, d2", None),
    ("vaddl.u32 q0, d1, d2", None), ("vaddl.s32 q0, d1, d2", None), ("vsubl.u8 q0, d1, d2", None), ("vsubl.s32 q0, d1, d2", None),
    ("vaddw.u8 q0, q1, d2", None), ("vaddw.s16 q0, q1, d2", None), ("vsubw.u32 q0, q1, d2", None), ("vaddw.s8 q0, q0, d1", None),
    ("vdup.8 d0, r3", None), ("vdup.16 q0, r4", None), ("vdup.32 d0, r5", None), ("vdup.32 q0, d1[1]", None),
    ("vdup.8 d2, d2[0]", None), ("vdup.8 d7, d17[5]", None), ("vdup.16 q1, d3[2]", None), ("vdup.8 q0, d1[7]", None), ("vdup.16 d0, d1[3]", None),
    ("vtbx.8 d31, {d15, d16, d17, d18}, d24", None), ("vtbx.8 d0, {d1}, d2", None), ("vtbx.8 d0, {d1, d2}, d0", None),
    ("vtbl.8 d0, {d1, d2, d3}, d4", None), ("vtbl.8 d6, {d8, d9}, d4", None),
    ("vadd.i16 d0, d1, d2", None), ("vsub.i16 q0, q1, q2", None), ("vmul.i8 d0, d1, d2", None), ("vmul.i16 q0, q1, q2", None),
    ("vmla.i32 q5, q8, q10", None), ("vmla.i16 d0, d1, d2", None), ("vmls.i8 q0, q1, q2", None), ("vmls.i32 d0, d1, d2", None),
    ("vtst.8 d0, d1, d2", None), ("vtst.16 q0, q1, q2", None), ("vneg.s8 d0, d1", None), ("vabs.s16 q0, q1", None), ("vneg.s32 d0, d1", None),
    ("vst4.32 {d24, d26, d28, d30}, [r0]", None), ("vst4.32 {d0, d1, d2, d3}, [r1]!", None), ("vst4.8 {d0, d1, d2, d3}, [r1], r2", None),
    ("vst4.16 {d4, d6, d8, d10}, [r9]", None), ("vld4.16 {d0, d1, d2, d3}, [r1], r2", None), ("vld4.32 {d0, d2, d4, d6}, [r0]!", None),
    ("vld4.8 {d0, d1, d2, d3}, [r0]", None), ("vst3.16 {d0, d1, d2}, [r0]!", None), ("vld3.32 {d0, d2, d4}, [r1], r7", None),
    ("vst2.32 {d0, d1}, [r0]!", None), ("vld2.8 {d0, d1, d2, d3}, [r1]", None), ("vst2.16 {d0, d2}, [r9], r7", None),
    ("vst2.8 {d8[6], d9[6]}, [r9], r7", None), ("vld2.16 {d8[2], d9[2]}, [r9]!", None), ("vst2.32 {d0[1], d1[1]}, [r0]", None),
    ("vld2.8 {d0[0], d1[0]}, [r1]", None), ("vst3.8 {d0[3], d1[3], d2[3]}, [r0]!", None), ("vld4.16 {d0[1], d1[1], d2[1], d3[1]}, [r1], r2", None),
    ("vst4.32 {d0[0], d2[0], d4[0], d6[0]}, [r9]", None), ("vld3.32 {d1[1], d2[1], d3[1]}, [r0]", None),
    ("rbit r7, r3", None), ("rbit r3, r3", None), ("ubfx r1, r2, #3, #5", None),
    # Thumb "add pc, rN": lands on one of the adds (r0 in {-2, 0, 2, 4, 6}: the next instruction up to the last)
    ("movs r1, #0\nadd pc, r0\nadds r1, #1\nadds r1, #2\nadds r1, #4\nadds r1, #8\nadds r1, #16", "jump"),
]

def main():
    args = sys.argv[1:]
    iters = int(args[args.index("-n") + 1]) if "-n" in args else 200
    pat = args[args.index("-k") + 1] if "-k" in args else ""
    verbose = "-v" in args
    rng = random.Random(1)
    cases = [c for c in CASES if pat in c[0]]
    fns, bodies, prog = [], [], []
    for k, (asm, kind) in enumerate(cases):
        entry = 0x10000 + 0x100 * k
        insns, lines, unimpl = translate(entry, assemble(asm))
        if unimpl:
            print(f"UNIMPL in {asm!r}: {unimpl}"); sys.exit(1)
        fns.append(f"fn_{entry:08x}"); bodies += lines; prog.append((asm, kind, insns))
        if verbose: print("\n".join(l for l in lines if l.startswith("\t") and "TR(" not in l and "ENTER" not in l))
    src = HARNESS % ("\n".join(bodies), ", ".join(fns), MEMSZ, MEMBASE, MEMBASE)
    td = tempfile.mkdtemp(prefix="neon_test_")
    open(os.path.join(td, "h.c"), "w").write(src)
    subprocess.run(["cc", "-O1", "-ffp-contract=off", "-w", "-I", os.path.join(HERE, "..", "..", "recompiler"), os.path.join(td, "h.c"), "-o", os.path.join(td, "h")], check=True)
    # all vectors in one run of the harness
    vectors = []
    for k, (asm, kind, insns) in enumerate(prog):
        for _ in range(iters):
            r, s, mem = inputs(rng)
            if kind == "jump": r[0] = rng.choice([0xfffffffe, 0, 2, 4, 6])
            vectors.append((k, r, s, mem))
    blob = b"".join(struct.pack("<I", k) + struct.pack(CPU_FMT, *r, 0, 0, 0, 0, 0, 0, *s, 0, 0) + mem for k, r, s, mem in vectors)
    out = subprocess.run([os.path.join(td, "h")], input=blob, capture_output=True, check=True).stdout
    rec = struct.calcsize(CPU_FMT) + MEMSZ
    assert len(out) == rec * len(vectors), f"harness wrote {len(out)} bytes for {len(vectors)} vectors"
    fails, failed_cases = 0, set()
    for n, (k, r, s, mem) in enumerate(vectors):
        got = struct.unpack(CPU_FMT, out[n * rec:n * rec + struct.calcsize(CPU_FMT)]); gmem = out[n * rec + struct.calcsize(CPU_FMT):(n + 1) * rec]
        asm, kind, insns = prog[k]
        st = State(r, s, mem); model(st, insns)
        gr, gflags, gs, gstop = got[:16], got[16:20], got[22:86], got[87]
        ok = gstop == 0 and list(gr[:15]) == st.r[:15] and list(gs) == st.s and gmem == bytes(st.mem) and (kind != "jump" or gflags == (st.n, st.z, st.c, st.v))
        if not ok:
            fails += 1
            if asm not in failed_cases:
                failed_cases.add(asm)
                print(f"FAIL {asm!r}" + (f" (stop={gstop})" if gstop else ""))
                for j in range(64):
                    if gs[j] != st.s[j]: print(f"  s{j}: got {gs[j]:08x} want {st.s[j]:08x}  (in s{j}={s[j]:08x})")
                for j in range(15):
                    if gr[j] != st.r[j]: print(f"  r{j}: got {gr[j]:08x} want {st.r[j]:08x}")
                if gmem != bytes(st.mem): print("  memory differs")
    print(f"{len(cases)} cases x {iters} vectors: {len(vectors) - fails} passed, {fails} failed")
    sys.exit(1 if fails else 0)

if __name__ == "__main__":
    main()
