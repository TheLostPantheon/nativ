#!/usr/bin/env python3
"""The GXM object map: the layouts of Sony's own GXM objects, as libgxm's code writes and reads
them, for the host's boundary (Draw/BeginScene/EndScene) that must decode them.

  gxmobjects.py <libgxm_es4.suprx.elf> <psp2/gxm.h> [out.md] [--no-unicorn]

Three sources, in order of authority:
  1. The firmware, run: every texture/surface init and setter is executed in Unicorn with one
     argument varied at a time; the words that change give each field's bit range empirically.
  2. The firmware, read: every getter/setter is disassembled and its loads/stores through its
     object argument reported with the masks/shifts applied to them; the context setters are
     scanned with alias tracking (r0 copied to another register, or offset by an index) so the
     context table covers every field a setter touches, not only the straight-line stores.
  3. The author's reading of the larger functions (CreateContext, Draw, BeginScene, the shader
     patcher, the precomputed state): the layouts in LAYOUTS below, each field tagged with the
     function and address it was read from, so it can be re-checked against the disassembly.

Vita3K (GPL) was used only as a cross-check and is cited where it disagrees. The firmware is the
authority; the output derives from Sony code and stays out of the repository (dump/ is gitignored).
"""
import os, re, struct, sys
from collections import defaultdict
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, "..", "..", "recompiler"))   # (platmap beside this; analyze in the recompiler)
from analyze import Module, Analyser
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
from capstone import arm as A

DATA = 0x81024000           # libgxm's data segment (link address); +0x3a0 holds the sceGxmInitialize flags


def nid_names():
    names = {}
    for l in open(os.path.join(HERE, "..", "..", "decrypt", "nidmap.tsv")):
        p = l.rstrip("\n").split("\t")
        if len(p) >= 3:
            names[(p[0], int(p[1], 16))] = p[2]
    return names


def enums(header):
    """SCE_GXM_* constant -> value, from the header (plain numeric initialisers only)"""
    src = re.sub(r"/\*.*?\*/", "", open(header).read(), flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    out = {}
    for m in re.finditer(r"\b(SCE_GXM_[A-Z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)[uU]?", src):
        out[m.group(1)] = int(m.group(2), 0)
    return out


class Lib:
    def __init__(self, elf):
        self.m = m = Module(elf)
        self.text = m.loads[0]
        self.base = self.text[2]
        self.code = m.mem[self.base]
        self.an = Analyser(m, self.base, self.base + self.text[4])
        names = nid_names()
        self.stub = {}
        for lib, fns, _ in m.imports():
            for nid, a in fns:
                self.stub[a & ~1] = names.get((lib, nid), f"{lib}_{nid:08X}")
        self.ex = {}
        for lib, nid, a, var in m.export_table():
            if lib and not var:
                self.ex[names.get((lib, nid), f"{lib}_{nid:08X}")] = a
        self.entries = {a & ~1 for a in self.ex.values()} | set(self.stub)
        self.md = Cs(CS_ARCH_ARM, CS_MODE_ARM); self.md.detail = True
        self.mdt = Cs(CS_ARCH_ARM, CS_MODE_THUMB); self.mdt.detail = True

    def insns(self, name):
        """the instructions of an exported function, in address order"""
        a = self.ex[name]
        arm = not a & 1
        r = self.an.function(a & ~1, arm, self.entries)
        if r is None:
            return []
        out = []
        for at in sorted(r[0]):
            o = at - self.base
            i = next((self.md if arm else self.mdt).disasm(bytes(self.code[o:o + 4]), at, 1), None)
            if i is not None:
                out.append(i)
        return out


# ---------------------------------------------------------------- 2. accessor evidence from disassembly
def accessor_evidence(lib, name):
    """loads/stores through r0 (the object) with the bit operations applied to the loaded values:
    a compact, mechanical reading of a getter or setter"""
    ev = []
    for i in lib.insns(name):
        mem = [o for o in i.operands if o.type == A.ARM_OP_MEM]
        if mem and i.reg_name(mem[0].mem.base) == "r0":
            ev.append(f"{i.mnemonic} {'+%#x' % mem[0].mem.disp}")
        elif i.mnemonic in ("ubfx", "sbfx"):
            ops = i.operands
            ev.append(f"ubfx bits {ops[2].imm}..{ops[2].imm + ops[3].imm - 1}")
        elif i.mnemonic in ("and", "bic", "orr", "eor", "tst") and i.operands[-1].type == A.ARM_OP_IMM:
            ev.append(f"{i.mnemonic} {i.operands[-1].imm:#x}")
        elif i.mnemonic in ("lsl", "lsr") and i.operands[-1].type == A.ARM_OP_IMM:
            ev.append(f"{i.mnemonic} {i.operands[-1].imm}")
        elif i.mnemonic in ("cmp",) and i.operands[-1].type == A.ARM_OP_IMM and i.operands[-1].imm >= 0x100:
            ev.append(f"cmp {i.operands[-1].imm:#x}")
    return ev


# ---------------------------------------------------------------- 2. context stores with alias tracking
def context_stores(lib, name):
    """(offset, width, indexed) for every store through a register known to hold the context
    (r0 at entry) or the context plus a constant, following mov/subs/add copies; an add with a
    shifted index register marks an indexed array store"""
    alias = {"r0": (0, None)}   # reg -> (constant offset, index description or None)
    out = []
    for i in lib.insns(name):
        ops = i.operands
        rn = lambda k: i.reg_name(ops[k].reg) if ops[k].type == A.ARM_OP_REG else None
        mem = [o for o in ops if o.type == A.ARM_OP_MEM]
        if i.mnemonic.startswith(("str", "vstr", "stm")) and mem:
            b = i.reg_name(mem[0].mem.base)
            if b in alias and not (i.mnemonic.startswith("str") and len(ops) >= 2 and rn(0) in alias and rn(0) == b and False):
                off, idx = alias[b]
                w = 1 if i.mnemonic in ("strb",) else 2 if i.mnemonic in ("strh",) else 8 if i.mnemonic == "strd" else 4
                out.append((off + mem[0].mem.disp, w, idx))
            continue
        if i.mnemonic == "stmib" and ops and rn(0) in alias:
            off, idx = alias[rn(0)]
            for k, o in enumerate(ops[1:]):
                out.append((off + 4 * (k + 1), 4, idx))
            continue
        # aliasing
        dst = rn(0) if ops else None
        if dst is None or dst in ("sp", "pc"):
            continue
        if i.mnemonic in ("mov", "subs", "sub", "add", "adds") and len(ops) >= 2 and rn(1) in alias:
            off, idx = alias[rn(1)]
            if i.mnemonic == "mov" and len(ops) == 2:
                alias[dst] = (off, idx); continue
            if len(ops) == 3 and ops[2].type == A.ARM_OP_IMM:
                d = ops[2].imm if i.mnemonic.startswith("add") else -ops[2].imm
                alias[dst] = (off + d, idx); continue
            if len(ops) == 3 and ops[2].type == A.ARM_OP_REG and i.mnemonic.startswith("add"):
                sh = ops[2].shift.value if ops[2].shift.type else 0
                alias[dst] = (off, f"{rn(2)} * {1 << sh}"); continue
        if dst in alias and not (i.mnemonic.startswith("ldr") and False):
            # the register is overwritten with something else
            if i.mnemonic not in ("cmp", "tst", "str", "strh", "strb", "stm"):
                alias.pop(dst, None)
    return out


# ---------------------------------------------------------------- 1. running the firmware in Unicorn
class Runner:
    """libgxm's two segments mapped at their link addresses; a function called with register and
    stack arguments; returns r0 and lets the caller read object memory"""
    SCRATCH, STACK, RET = 0x60000000, 0x70000000, 0x50000000

    def __init__(self, lib):
        from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_PROT_ALL
        from unicorn import arm_const as C
        self.C = C
        self.mu = mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        for s in lib.m.loads:
            lo = s[2] & ~0xfff
            size = (s[2] + s[5] - lo + 0xfff) & ~0xfff
            mu.mem_map(lo, size, UC_PROT_ALL)
            mu.mem_write(s[2], bytes(lib.m.mem[s[2]]))
        mu.mem_map(0, 0x1000)                   # the stack-check guard: an imported variable left at 0 in the dump
        mu.mem_map(self.SCRATCH, 0x10000)
        mu.mem_map(self.STACK, 0x10000)
        mu.mem_map(self.RET, 0x1000)
        mu.reg_write(C.UC_ARM_REG_C1_C0_2, mu.reg_read(C.UC_ARM_REG_C1_C0_2) | (0xf << 20))   # VFP/NEON for the
        mu.reg_write(C.UC_ARM_REG_FPEXC, 0x40000000)                                           # float setters
        self.lib = lib

    def call(self, name, regs, stack=(), floats=()):
        C, mu = self.C, self.mu
        a = self.lib.ex[name] & ~1
        sp = self.STACK + 0x8000
        for k, v in enumerate(stack):
            mu.mem_write(sp + 4 * k, struct.pack("<I", v & 0xffffffff))
        for k, v in enumerate(regs):
            mu.reg_write(getattr(C, f"UC_ARM_REG_R{k}"), v & 0xffffffff)
        for k, v in enumerate(floats):
            mu.reg_write(getattr(C, f"UC_ARM_REG_S{k}"), struct.unpack("<I", struct.pack("<f", v))[0])
        mu.reg_write(C.UC_ARM_REG_SP, sp)
        mu.reg_write(C.UC_ARM_REG_LR, self.RET)
        try:
            mu.emu_start(a, self.RET, count=200000)
        except Exception as e:   # a fault (an import stub reached, an unmapped address): reported in the row
            self.fault = f"{e} at pc {mu.reg_read(C.UC_ARM_REG_PC):#x}"
            return 0xeeeeeeee
        return mu.reg_read(C.UC_ARM_REG_R0)

    def words(self, at, n):
        return list(struct.unpack(f"<{n}I", self.mu.mem_read(at, 4 * n)))

    def zero(self, at, n):
        self.mu.mem_write(at, b"\0" * n)


def bit_ranges(diff_words):
    """'w1[12..23]' for each word whose bits changed"""
    out = []
    for k, d in enumerate(diff_words):
        if d:
            bits = [b for b in range(32) if d >> b & 1]
            out.append(f"w{k}[{bits[0]}..{bits[-1]}]" if bits[-1] > bits[0] else f"w{k}[{bits[0]}]")
    return " ".join(out) or "-"


def experiments(run, E):
    """each: (function, object words, baseline (regs, stack, floats), [(parameter name, which,
    index, alternative values)]). 'which' is 'r' (register), 's' (stack slot) or 'f' (VFP s-reg).
    Returns rows for the map."""
    T = run.SCRATCH
    F = lambda n: E[n]
    LIN = F("SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8")   # | SWIZZLE4_ABGR (0)
    rows = []

    def tex_init(name, regs, stack, params, note=""):
        run.zero(T, 16)
        rc = run.call(name, regs, stack)
        base = run.words(T, 4)
        line = [f"`{name}`", note, f"rc={rc:#x}", " ".join(f"{w:08x}" for w in base)]
        rows.append(("## " + name, line))
        for pname, which, idx, alts in params:
            for v in alts:
                r2, s2 = list(regs), list(stack)
                if which == "r": r2[idx] = v
                else: s2[idx] = v
                run.zero(T, 16)
                rc2 = run.call(name, r2, s2)
                w = run.words(T, 4)
                rows.append(("row", [pname, f"{v:#x}", f"rc={rc2:#x}", " ".join(f"{x:08x}" for x in w), bit_ranges([a ^ b for a, b in zip(base, w)])]))

    # --- textures: (texture, data, format, width, height, mipCount)
    tex_init("sceGxmTextureInitLinear", [T, 0x01000000, LIN, 64], [32, 1],
             [("data", "r", 1, [0x01001000]), ("format base", "r", 2, [F("SCE_GXM_TEXTURE_BASE_FORMAT_S8"), F("SCE_GXM_TEXTURE_BASE_FORMAT_P8") | 0]),
              ("format swizzle", "r", 2, [LIN | F("SCE_GXM_TEXTURE_SWIZZLE4_ARGB"), LIN | F("SCE_GXM_TEXTURE_SWIZZLE4_BGRA")]),
              ("width", "r", 3, [65, 2048]), ("height", "s", 0, [33, 2048]), ("mipCount", "s", 1, [2, 9])],
             "data 0x01000000, U8U8U8U8_ABGR, 64x32, 1 mip")
    tex_init("sceGxmTextureInitSwizzled", [T, 0x01000000, LIN, 64], [32, 1],
             [("width", "r", 3, [128, 1024]), ("height", "s", 0, [64, 1024]), ("mipCount", "s", 1, [2, 9])], "64x32 (log2 encoded)")
    tex_init("sceGxmTextureInitTiled", [T, 0x01000000, LIN, 64], [32, 1],
             [("width", "r", 3, [96, 2048]), ("height", "s", 0, [64, 2048]), ("mipCount", "s", 1, [2])], "64x32")
    tex_init("sceGxmTextureInitCube", [T, 0x01000000, LIN, 64], [64, 1],
             [("width/height", "r", 3, [128]), ("mipCount", "s", 1, [2])], "64x64 (log2 encoded)")
    tex_init("sceGxmTextureInitSwizzledArbitrary", [T, 0x01000000, LIN, 100], [60, 1],
             [("width", "r", 3, [101]), ("height", "s", 0, [61]), ("mipCount", "s", 1, [2])], "100x60")
    tex_init("sceGxmTextureInitCubeArbitrary", [T, 0x01000000, LIN, 100], [100, 1],
             [("width/height", "r", 3, [101]), ("mipCount", "s", 1, [2])], "100x100")
    # (texture, data, format, width, height, byteStride)
    tex_init("sceGxmTextureInitLinearStrided", [T, 0x01000000, LIN, 64], [32, 256],
             [("width", "r", 3, [65]), ("height", "s", 0, [33]), ("byteStride", "s", 1, [260, 512, 0x1000, 0x8000])], "64x32, stride 256")

    # --- texture setters on a linear 64x32 baseline
    def setter(name, args, note, floats=()):
        run.zero(T, 16)
        run.call("sceGxmTextureInitLinear", [T, 0x01000000, LIN, 64], [32, 1])
        base = run.words(T, 4)
        rc = run.call(name, [T] + list(args), floats=floats)
        w = run.words(T, 4)
        rows.append(("row", [f"`{name}`", ", ".join(f"{a:#x}" for a in args) + note, "void" if T <= rc < T + 0x40 else f"rc={rc:#x}", " ".join(f"{x:08x}" for x in w), bit_ranges([a ^ b for a, b in zip(base, w)])]))
    rows.append(("## texture setters", ["function", "arguments", "rc", "words after", "bits changed vs the linear baseline"]))
    setter("sceGxmTextureSetMinFilter", [F("SCE_GXM_TEXTURE_FILTER_MIPMAP_POINT")], " (MIPMAP_POINT = 3)")
    setter("sceGxmTextureSetMagFilter", [F("SCE_GXM_TEXTURE_FILTER_MIPMAP_POINT")], " (3)")
    setter("sceGxmTextureSetMipFilter", [F("SCE_GXM_TEXTURE_MIP_FILTER_ENABLED")], " (ENABLED)")
    setter("sceGxmTextureSetUAddrMode", [F("SCE_GXM_TEXTURE_ADDR_REPEAT")], " (REPEAT = 0; init leaves CLAMP = 2 in bits 6-8)")
    setter("sceGxmTextureSetVAddrMode", [F("SCE_GXM_TEXTURE_ADDR_REPEAT")], " (0; init CLAMP in bits 3-5)")
    setter("sceGxmTextureSetUAddrMode", [F("SCE_GXM_TEXTURE_ADDR_MIRROR")], " (MIRROR = 1: refused with 0x805b0009 unless the texture is SWIZZLED, 0x8101a2b4-0x8101a2c0)")
    setter("sceGxmTextureSetUAddrMode", [F("SCE_GXM_TEXTURE_ADDR_CLAMP_HALF_BORDER")], " (7: modes 4-7 need a border-capable format and a swizzled type, else 0x805b0009)")
    setter("sceGxmTextureSetLodBias", [0], " (init sets 31 = SCE_GXM_LOD_BIAS default)")
    setter("sceGxmTextureSetLodBias", [63], "")
    setter("sceGxmTextureSetLodMin", [12], "")
    setter("sceGxmTextureSetMipmapCount", [13], "")
    run.zero(T, 16)
    run.call("sceGxmTextureInitLinear", [T, 0x01000000, F("SCE_GXM_TEXTURE_BASE_FORMAT_P8"), 64], [32, 1])
    base = run.words(T, 4)
    rc = run.call("sceGxmTextureSetPalette", [T, 0x02000040])
    w = run.words(T, 4)
    rows.append(("row", ["`sceGxmTextureSetPalette`", "0x02000040 on a P8 texture (64-byte aligned; refused with 0x805b0004 on a non-palette format)", f"rc={rc:#x}", " ".join(f"{x:08x}" for x in w), bit_ranges([a ^ b for a, b in zip(base, w)])]))
    setter("sceGxmTextureSetGammaMode", [F("SCE_GXM_TEXTURE_GAMMA_GR")], " (GR = 0x18000000)")
    setter("sceGxmTextureSetNormalizeMode", [F("SCE_GXM_TEXTURE_NORMALIZE_ENABLED")], " (init sets it)")
    setter("sceGxmTextureSetNormalizeMode", [0], "")
    setter("sceGxmTextureSetData", [0x03000000], "")
    setter("sceGxmTextureSetFormat", [F("SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8") | F("SCE_GXM_TEXTURE_SWIZZLE4_RGBA")], " (RGBA swizzle)")

    # --- colour surface: (surface, colorFormat, surfaceType, scaleMode, outputRegisterSize, width, height, strideInPixels, data)
    CF = F("SCE_GXM_COLOR_BASE_FORMAT_U8U8U8U8") | F("SCE_GXM_COLOR_SWIZZLE4_ABGR")
    def surf(name, regs, stack, params, note, nwords):
        run.zero(T, 4 * nwords)
        rc = run.call(name, regs, stack)
        base = run.words(T, nwords)
        rows.append(("## " + name, [f"`{name}`", note, f"rc={rc:#x}", " ".join(f"{w:08x}" for w in base)]))
        for pname, which, idx, alts in params:
            for v in alts:
                r2, s2 = list(regs), list(stack)
                if which == "r": r2[idx] = v
                else: s2[idx] = v
                run.zero(T, 4 * nwords)
                rc2 = run.call(name, r2, s2)
                w = run.words(T, nwords)
                rows.append(("row", [pname, f"{v:#x}", f"rc={rc2:#x}", " ".join(f"{x:08x}" for x in w), bit_ranges([a ^ b for a, b in zip(base, w)])]))
    surf("sceGxmColorSurfaceInit", [T, CF, F("SCE_GXM_COLOR_SURFACE_LINEAR"), 0], [0, 960, 544, 960, 0x02000000],
         [("colorFormat", "r", 1, [F("SCE_GXM_COLOR_BASE_FORMAT_U8U8U8U8") | F("SCE_GXM_COLOR_SWIZZLE4_ARGB"), F("SCE_GXM_COLOR_BASE_FORMAT_U5U6U5") | F("SCE_GXM_COLOR_SWIZZLE3_BGR"), F("SCE_GXM_COLOR_BASE_FORMAT_F16F16F16F16") | 0]),
          ("surfaceType", "r", 2, [F("SCE_GXM_COLOR_SURFACE_TILED")]),
          ("scaleMode", "r", 3, [1]), ("outputRegisterSize", "s", 0, [1]),
          ("width", "s", 1, [959, 512]), ("height", "s", 2, [543, 256]), ("strideInPixels", "s", 3, [1024]), ("data", "s", 4, [0x02001000])],
         "U8U8U8U8_ABGR linear 960x544 stride 960 data 0x02000000 (12 words: 8 + the 16-byte background texture at +0x20)", 12)
    surf("sceGxmColorSurfaceInit", [T, CF, F("SCE_GXM_COLOR_SURFACE_SWIZZLED"), 0], [0, 512, 256, 512, 0x02000000],
         [("width", "s", 1, [1024]), ("height", "s", 2, [512])],
         "U8U8U8U8_ABGR SWIZZLED 512x256 stride 512 (power-of-two sizes required: 960x544 is refused with 0x805b0003)", 12)
    # --- depth/stencil surface: (surface, format, type, strideInSamples, depthData, stencilData)
    surf("sceGxmDepthStencilSurfaceInit", [T, F("SCE_GXM_DEPTH_STENCIL_FORMAT_DF32_S8"), F("SCE_GXM_DEPTH_STENCIL_SURFACE_LINEAR"), 960], [0x03000000, 0x03100000],
         [("format", "r", 1, [F("SCE_GXM_DEPTH_STENCIL_FORMAT_DF32"), F("SCE_GXM_DEPTH_STENCIL_FORMAT_S8"), F("SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24"), F("SCE_GXM_DEPTH_STENCIL_FORMAT_D16"), F("SCE_GXM_DEPTH_STENCIL_FORMAT_DF32M_S8")]),
          ("type", "r", 2, [F("SCE_GXM_DEPTH_STENCIL_SURFACE_TILED")]), ("strideInSamples", "r", 3, [992, 4096]),
          ("depthData", "s", 0, [0x03001000]), ("stencilData", "s", 1, [0x03101000])],
         "DF32_S8 linear stride 960 depth 0x03000000 stencil 0x03100000 (5 words)", 5)
    # depth/stencil setters
    rows.append(("## depth/stencil surface setters", ["function", "arguments", "rc", "words after", "bits changed vs the DF32_S8 baseline"]))
    def ds_setter(name, args, note, floats=()):
        run.zero(T, 20)
        run.call("sceGxmDepthStencilSurfaceInit", [T, F("SCE_GXM_DEPTH_STENCIL_FORMAT_DF32_S8"), 0, 960], [0x03000000, 0x03100000])
        base = run.words(T, 5)
        rc = run.call(name, [T] + list(args), floats=floats)
        w = run.words(T, 5)
        rows.append(("row", [f"`{name}`", ", ".join(f"{a:#x}" for a in args) + note, "void" if T <= rc < T + 0x40 else f"rc={rc:#x}", " ".join(f"{x:08x}" for x in w), bit_ranges([a ^ b for a, b in zip(base, w)])]))
    ds_setter("sceGxmDepthStencilSurfaceSetForceLoadMode", [F("SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED")], " (ENABLED = 2)")
    ds_setter("sceGxmDepthStencilSurfaceSetForceStoreMode", [F("SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED")], " (ENABLED = 4)")
    ds_setter("sceGxmDepthStencilSurfaceSetBackgroundStencil", [0xa5], "")
    ds_setter("sceGxmDepthStencilSurfaceSetBackgroundMask", [1], " (init sets it)")
    ds_setter("sceGxmDepthStencilSurfaceSetBackgroundMask", [0], "")
    ds_setter("sceGxmDepthStencilSurfaceSetBackgroundDepth", [], " s0 = 0.5f", floats=(0.5,))
    # colour surface setters
    rows.append(("## colour surface setters", ["function", "arguments", "rc", "words after", "bits changed vs the linear baseline"]))
    def cs_setter(name, args, note):
        run.zero(T, 48)
        run.call("sceGxmColorSurfaceInit", [T, CF, 0, 0], [0, 960, 544, 960, 0x02000000])
        base = run.words(T, 12)
        rc = run.call(name, [T] + list(args))
        w = run.words(T, 12)
        rows.append(("row", [f"`{name}`", ", ".join(f"{a:#x}" for a in args) + note, "void" if T <= rc < T + 0x40 else f"rc={rc:#x}", " ".join(f"{x:08x}" for x in w), bit_ranges([a ^ b for a, b in zip(base, w)])]))
    cs_setter("sceGxmColorSurfaceSetScaleMode", [1], " (MSAA_DOWNSCALE)")
    cs_setter("sceGxmColorSurfaceSetGammaMode", [F("SCE_GXM_COLOR_SURFACE_GAMMA_R")], " (R = 0x1000: also sets the background texture's gamma)")
    cs_setter("sceGxmColorSurfaceSetDitherMode", [F("SCE_GXM_COLOR_SURFACE_DITHER_ENABLED")], " (8)")
    cs_setter("sceGxmColorSurfaceSetData", [0x02100000], "")
    cs_setter("sceGxmColorSurfaceSetClip", [8, 16, 100, 200], " (xMin, yMin, xMax, yMax)")
    cs_setter("sceGxmColorSurfaceSetFormat", [F("SCE_GXM_COLOR_BASE_FORMAT_U8U8U8U8") | F("SCE_GXM_COLOR_SWIZZLE4_RGBA")], " (RGBA)")
    return rows


# ---------------------------------------------------------------- 3. the author's reading
# Each layout: (title, intro, [(offset, size, meaning, evidence)]). Offsets are from the object.
LAYOUTS = []

LAYOUTS.append(("SceGxmTexture (16 bytes)", """
Four control words. The format argument is split three ways: its bit 31 lands in word 0, bits
24-28 (the base format) in word 1, bits 12-14 (the swizzle) in word 3 bits 28-30; `sceGxmTextureGetFormat`
reassembles exactly those (`ldr +0 & 0x80000000 | ldr +4 & 0x1f000000 | ldrh +0xe & 0x7000`).
The type (SceGxmTextureType, pre-shifted) is word 1 bits 29-31. Size encoding depends on the type:
swizzled (0) and cube (0x40000000) store log2 sizes (`tst w1, 0xa0000000` is zero for exactly those
two), every other type stores width-1 / height-1. LINEAR_STRIDED (0xc0000000) has no mip chain and
reuses word 0's mip/lod bits for the stride; its min filter moves to bits 12-13.
""", [
    ("+0x0", "4", "word 0, non-strided: bits 0-2 unused; 3-5 V address mode; 6-8 U address mode (SceGxmTextureAddrMode, init = CLAMP = 2 in both: the 0x90 the inits or in); 9 mip filter (0x200 = ENABLED); 10-11 min filter; 12-13 mag filter (SceGxmTextureFilter); 14-16 unused; 17-20 mipCount-1 (`GetMipmapCount` = (ubfx 17,4 + 1) & 15); 21-26 LOD bias (init 31 = `SCE_GXM_LOD_BIAS` no bias; `SetLodBias` takes 0-63); 27-28 gamma mode (0x18000000: SCE_GXM_TEXTURE_GAMMA_R 0x08000000, GR 0x18000000); 29-30 cleared when copied into a context slot (`bic 0x60000000` in SetFragmentTexture / _sceGxmSetVertexTexture); 31 format bit 31", "InitLinear 0x81019bb0-0x81019c1c; Set*Filter 0x8101a108-0x8101a1f0; Set*AddrMode 0x8101a294/0x8101a478; SetLodBias 0x8101a890; SetMipmapCount 0x8101ad84; SetGammaMode 0x8101af80"),
    ("+0x0", "4", "word 0, LINEAR_STRIDED: stride in words minus one S = byteStride/4 - 1, scattered: bits 9-11 = S[0..2], bits 17-26 = S[3..12], bits 1-2 = S[13..14] (`GetStride` = ((w0>>1&3)<<13 | (w0>>17&0x3ff)<<3 | (w0>>9&7)) + 1) * 4); bits 3-8 address modes as above; 12-13 min filter (and mag filter: `GetMinFilter` reads 12-13 for this type, `GetMagFilter` always reads 12-13); no mip count / lod bias (getters return 0); 31 format bit 31", "InitLinearStrided 0x8101a05c-0x8101a0c4; GetStride 0x8101aa3c"),
    ("+0x4", "4", "word 1: bits 29-31 type (SceGxmTextureType >> 0: SWIZZLED 0x00000000, CUBE 0x40000000, LINEAR 0x60000000, TILED 0x80000000, SWIZZLED_ARBITRARY 0xa0000000, LINEAR_STRIDED 0xc0000000, CUBE_ARBITRARY 0xe0000000); bits 24-28 base format (format & 0x1f000000); swizzled/cube: bits 16-19 log2 width, bits 0-3 log2 height; all others: bits 12-23 width-1, bits 0-11 height-1", "GetWidth 0x8101abf8 / GetHeight 0x8101ad3c; InitSwizzled 0x810199cc-0x81019a00"),
    ("+0x8", "4", "word 2: bits 2-31 data address (`GetData` = w2 & ~3; the inits store data & ~3, SetData keeps the low 2 bits); bits 0-1 = LOD min bits 2-3 (SetLodMin stores lodMin >> 2 here)", "SetData 0x8101ae84; SetLodMin 0x8101a90c-0x8101a934"),
    ("+0xc", "4", "word 3: bits 0-25 palette address >> 6 (`GetPalette` = w3 << 6, so the field is 26 bits of a 64-byte-aligned address: SetPalette masks with 0xfc000000 and ors palette >> 6); bits 26-27 LOD min bits 0-1; bits 28-30 swizzle (format & 0x7000) << 16; bit 31 normalize mode (0x80000000 = ENABLED; every init sets it)", "SetPalette 0x8101aeec; GetLodMin 0x8101a96c; SetNormalizeMode 0x8101affc"),
]))

LAYOUTS.append(("SceGxmColorSurface (0x30 bytes)", """
`sceGxmColorSurfaceInit(surface, colorFormat, surfaceType, scaleMode, outputRegisterSize, width,
height, strideInPixels, data)`. Vita3K's struct for this object is its own invention (it says so);
the real one is below. The last 16 bytes are a SceGxmTexture of the surface (`backgroundTex` in the
headers), built by calling sceGxmTextureInitSwizzled/Tiled/Linear/LinearStrided on it with the
colour format mapped to a texture format (sub_8100bd34) -- so the surface's width, height and data
can also be read with the texture getters at +0x20.
""", [
    ("+0x0", "4", "bits 0-1 scale mode (SceGxmColorSurfaceScaleMode: 0 none, 1 MSAA downscale); bit 2 set by Init (surface valid); bit 3 dither (SCE_GXM_COLOR_SURFACE_DITHER_ENABLED = 8); bits 12-13 gamma (SCE_GXM_COLOR_SURFACE_GAMMA_R 0x1000, GR 0x3000). InitDisabled zeroes words 0-7.", "Init 0x81018cc4/0x81018cdc; Get/SetScaleMode 0x81019028; Get/SetGammaMode 0x81019498; Get/SetDitherMode 0x8101958c"),
    ("+0x4", "4", "colour format (colorFormat & 0xf1b00000: base format bits 20-21,23,28-31 and swizzle bits 20-21 -- `GetFormat` masks exactly that) | surface type (0x0c000000: LINEAR 0, TILED 0x04000000, SWIZZLED 0x08000000; `GetType`) | 0x10000 | the PBE pack mode sub_8100bd80(format, outputRegisterSize) computes into bits 8-15 (`IsEnabled` = (w1 & 0x1ff00) != 0 unless init flag 0x8000)", "Init 0x81018c88-0x81018ce8; GetFormat 0x810190e8; GetType 0x81019218; IsEnabled 0x81018f50"),
    ("+0x8", "4", "data address (bits 2-31; `GetData` = w2 & ~3; SetData keeps the low 2 bits and also calls sceGxmTextureSetData on +0x20)", "Init 0x81018ccc/0x81018ce8; SetData 0x810190a0-0x810190b8"),
    ("+0xc", "4", "bits 0-13: strideInPixels/2 - 1 (`GetStrideInPixels` = ((w3 & 0x3fff) + 1) * 2)", "Init 0x81018c94-0x81018cec; GetStrideInPixels 0x81019224"),
    ("+0x10", "4", "0 at Init (the result of the pack-mode helper's status)", "Init 0x81018c80"),
    ("+0x14", "4", "clip: bits 0-11 xMin, 12-23 yMin (Init: 0); bits 24-27 log2 of the width in 16-pixel tiles, 28-31 log2 of the height in tiles (Init computes clz of (w+15)>>4 and (h+15)>>4)", "Init 0x81018c64-0x81018ce0; GetClip 0x81018f80; SetClip 0x81018fd0"),
    ("+0x18", "4", "clip: bits 0-11 xMax (Init: width-1), bits 12-23 yMax (Init: height-1) -- so width/height are recoverable here unless the game called SetClip", "Init 0x81018c70/0x81018c98"),
    ("+0x1c", "4", "outputRegisterSize (SceGxmOutputRegisterSize: 0 = 32-bit, 1 = 64-bit)", "Init 0x81018d00"),
    ("+0x20", "16", "SceGxmTexture of the surface: linear type for LINEAR when stride == (width+7)&~7, else LINEAR_STRIDED with byteStride = stride * bytesPerPixel; TILED -> tiled; SWIZZLED -> swizzled; 1 mip; gamma copied from +0x0 bits 12-13 (R -> SCE_GXM_TEXTURE_GAMMA_R, GR -> 0x18000000). width-1/height-1 (or log2) live in its word 1 as for any texture", "Init 0x81018d04-0x81018eb8"),
]))

LAYOUTS.append(("SceGxmDepthStencilSurface (0x14 bytes)", """
`sceGxmDepthStencilSurfaceInit(surface, depthStencilFormat, surfaceType, strideInSamples, depthData,
stencilData)`. Vita3K's bit split of word 0 (`_stride` at bits 3-10, `_type_and_format` 20 bits) is
consistent with this; it does not name the force load/store bits' meaning for BeginSceneEx.
""", [
    ("+0x0", "4", "zlsControl: bit 1 force load (SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED = 2); bit 2 force store (= 4); bits 3-10 strideInSamples/32 - 1 (`GetStrideInSamples` = (ubfx 3,8 + 1) << 5; stride must be a multiple of 32); surface type bits 12 and 16 (SCE_GXM_DEPTH_STENCIL_SURFACE_TILED = 0x11000; LINEAR = 0); bit 20 (0x100000) set by Init; format bits (format & 0x07eee000: bits 13-15, 17-19, 21-26 -- `GetFormat`/`IsEnabled` mask exactly that; DF32 0x44000, S8 0x22000, DF32_S8 0x66000, DF32M 0xcc000, DF32M_S8 0xee000, S8D24 0x1266000, D16 0x2444000). InitDisabled writes 0.", "Init 0x810192b4-0x810192f4; GetFormat 0x81019470; GetStrideInSamples 0x81019484; Set/GetForceLoadMode 0x81019428; Set/GetForceStoreMode 0x8101944c"),
    ("+0x4", "4", "depthData", "Init 0x810192c0"),
    ("+0x8", "4", "stencilData", "Init 0x810192d8"),
    ("+0xc", "4", "backgroundDepth, float (Init: 1.0f; Set/GetBackgroundDepth are a bare vstr/vldr s0)", "0x810192fc; 0x810193e8"),
    ("+0x10", "4", "backgroundControl: bits 0-7 background stencil (Init 0); bit 8 background mask (Init 1); bit 9 set by Init (0x300 total)", "Init 0x81019300; Set/GetBackgroundStencil 0x810193f8; Set/GetBackgroundMask 0x810193c0"),
]))

LAYOUTS.append(("SceGxmContext (0x554 bytes of params->hostMem, memset to 0 by CreateContext)", """
`sceGxmCreateContext` requires hostMemSize >= 0x800, clears 0x554 bytes and builds the context in
place (`*context = hostMem`). Offsets below are from the context. The dirty word at +4 is the
driver's own bookkeeping: bits 0x7c0 are the vertex program's flags (vprog+0x1c: 0x700/0x740/0x780/
0x7c0 depending on whether the GXP has SA blocks), 0x3e the fragment program's (fprog+0x1c); the
setters OR in 0x300/0x380 (vertex uniform buffer changed, SA block A/B), 0x400 (vertex stream
changed and used), 0x28/0x2c (fragment uniform buffer), 0x20/0xc/0x10 (fragment texture, by the
GXP's per-texture usage nibble), 0x800 (ISP state). `sceGxmDraw` flushes these into the ring
buffers (sub_81006eb4, sub_81006e54) -- see "What Draw does" below.
""", [
    ("+0x0", "4", "context type: 0 immediate, 1 deferred (`sceGxmGetContextType`)", "CreateContext 0x81008630; GetContextType 0x81003d9c"),
    ("+0x4", "4", "dirty flags (see intro); 1 at creation", "0x810089b8 and every setter"),
    ("+0x8", "0x14", "VDM ring cursor: +8 current, +0xc end, +0x10 end offset, +0x14 refill callback (0x81015290), +0x18 -> ring descriptor (+0x3e0). Draw commands (sub_8101f738, 6 words) and vertex state words (sub_8101f680, 2 words) are allocated here 4-aligned", "CreateContext 0x810088c8-0x810088e4; sub_810153b4"),
    ("+0x1c", "0x14", "vertex ring cursor (+0x1c current, +0x20 end, +0x24, +0x28 callback, +0x2c -> descriptor +0x404). **Default vertex uniform buffers come from here**: `sceGxmReserveVertexDefaultUniformBuffer` rounds +0x1c up to 4, hands it out if 4*gxp[+0x64] bytes fit before +0x20 (else the refill callback), stores it at +0x264 and +0x27c and sets +0x228 = 1 -- but does NOT advance the cursor. `sceGxmDraw`/`EndScene`/`MidSceneFlush` commit it: when +0x228 == 1 they set +0x1c = +0x27c + 4*gxp[+0x64] and +0x228 = 2. The same ring holds the vertex SA blocks (+0x268, +0x278), PDS vertex-fetch instances (+0x3d0) and uniform-buffer DMA tables (+0x3c8) that the flush writes per draw", "Reserve 0x81003f08-0x81003f50; Draw 0x81004900-0x81004918; CreateContext 0x810088ec-0x81008914"),
    ("+0x30", "0x14", "fragment ring cursor (+0x30 current, +0x34 end, +0x3c callback, +0x40 -> descriptor +0x428). `sceGxmReserveFragmentDefaultUniformBuffer` allocates here (stores to +0x84 and +0x9c, state +0x48 = 1); Draw commits +0x30 = +0x9c + 4*gxp[+0x64], +0x48 = 2. Also the fragment SA blocks (+0x88, +0x98), PDS fragment/texture block (+0x1b0) and DMA table (+0x1a8)", "Reserve 0x8100401c-0x81004064; Draw 0x81004870-0x81004888"),
    ("+0x44", "4", "current SceGxmFragmentProgram*", "SetFragmentProgram 0x81003e3c; Draw 0x8100484c"),
    ("+0x48", "4", "fragment default uniform buffer state: 0 none, 1 reserved (uncommitted), 2 committed by a draw, 3 set by sceGxmSetFragmentDefaultUniformBuffer. SetFragmentProgram resets it to 0 unless 3", "0x81004064; 0x81004198; 0x81003e80-0x81003e8c"),
    ("+0x4c", "4*14", "fragment uniform buffers 0-13 (sceGxmSetFragmentUniformBuffer(ctx, i, p) stores p at +0x4c+4i). Initialised to the sentinel 0x3e000000 | i<<16; the GXP's tables address the default buffer as index 14 = +0x84", "SetFragmentUniformBuffer 0x810045dc; CreateContext 0x81008778"),
    ("+0x84", "4", "fragment default uniform buffer (index 14); set by Reserve or SetFragmentDefaultUniformBuffer", "0x81004050; 0x81004160"),
    ("+0x88", "4", "last fragment SA block B (texture control words as secondary attributes), allocated in the fragment ring by the flush", "sub_81006eb4 0x810080b0"),
    ("+0x8c", "12", "copied from fprog+0x3c/+0x40/+0x44 by SetFragmentProgram (the registered program's three GPU allocations)", "0x81003e5c-0x81003e70"),
    ("+0x98", "4", "last fragment SA block A (uniform buffer addresses, literals, textures)", "sub_81006eb4 0x8100797c"),
    ("+0x9c", "4", "reserved fragment default uniform buffer (what Reserve returned; Draw commits from here)", "0x81004054"),
    ("+0xa0", "16*16", "fragment textures 0-15: the 16-byte SceGxmTexture copied by sceGxmSetFragmentTexture (word 0 with bits 29-30 cleared). The setter compares all four words first and skips the dirty marking when equal", "SetFragmentTexture 0x81004460-0x810044a4 (ip = ctx + 0x98 + 16i, words at ip+8..ip+0x14)"),
    ("+0x1a0", "4", "current SceGxmPrecomputedFragmentState* (0 = none); +0x1a4 the one last flushed", "SetPrecomputedFragmentState 0x8100464c; sub_81006eb4 0x81006f88"),
    ("+0x1a8", "4", "last fragment uniform-buffer DMA table block (ring); +0x1ac/+0x1ae its sizes", "sub_81006eb4 0x81007ba0"),
    ("+0x1b0", "4", "last fragment PDS block (textures + fprog template), +0x1b4h fprog+0x2c, +0x1b6h its size", "sub_81006eb4 0x81007ef8-0x81007f0c"),
    ("+0x1b8", "4", "front fragment program enable (SCE_GXM_FRAGMENT_PROGRAM_DISABLED = 0x200000 kept raw); +0x1bc back", "SetFrontFragmentProgramEnable 0x8100a630; SetBack 0x8100a650"),
    ("+0x1c0", "4", "dirty word for the ISP/viewport groups: bit 0 front word (+0x1c4), 1 front depth bias (+0x1c8), 2 front stencil (+0x1cc), 3 back word (+0x1d0), 4 back depth bias, 5 back stencil, 6 fragment state words (+0x1dc), 7 region clip (+0x1e8), 8 viewport (+0x1f0), 11 W clamp value (+0x210), 12 cull/viewport-enable word (+0x214)", "every state setter"),
    ("+0x1c4", "4", "**front ISP word**: bits 0-7 stencil ref (SetFrontStencilRef); bit 8 stencil test enabled (set by SetFrontStencilFunc when the stencil word's upper half != 0xe00, i.e. not (ALWAYS, KEEP, KEEP, KEEP)); bit 11 two-sided (SCE_GXM_TWO_SIDED_ENABLED = 0x800); bits 12-13 visibility test index bits 12-13 (the index's bits 0-11 go to +0x1c8); bit 14 visibility test enable (0x4000); bits 15-17 polygon mode (SceGxmPolygonMode pre-shifted, 0x38000); bit 18 visibility test op (0x40000 = SET); bit 19 line fill last pixel (0x80000); bit 20 depth write DISABLED (0x100000); bit 21 fragment program disabled (0x200000, from the setter OR'd with fprog+0x20's bit 21); bits 22-24 depth func (SceGxmDepthFunc pre-shifted: NEVER 0, LESS 0x400000 ... ALWAYS 0x1c00000); bits 25-27 pass type from fprog+0x20 (SceGxmPassType, OPAQUE 0 / TRANSLUCENT 0x2000000 / DISCARD 0x4000000 / MASK_UPDATE 0x6000000 / DEPTH_REPLACE 0xa000000); bits 28-31 point/line width - 1 (SetFrontPointLineWidth)", "0x8100a5a4 (depth func), 0x8100a68c (depth write), 0x8100a614 (fp enable), 0x8100a6dc, 0x8100a774, 0x8100a7c4, 0x8100a814-0x8100a8a8 (stencil), 0x8100a72c, 0x8100ad00, 0x8100ac10, 0x8100acb0, 0x8100a9c4 (two-sided)"),
    ("+0x1c8", "4", "front: bits 0-11 visibility test index (low 12 bits); bits 16-20 depth bias units; bits 21-25 depth bias factor (SetFrontDepthBias(ctx, factor, units))", "0x8100ac38-0x8100ac58; 0x8100a94c-0x8100a96c"),
    ("+0x1cc", "4", "**front stencil word** from SetFrontStencilFunc(ctx, func, stencilFail, depthFail, depthPass, compareMask, writeMask): bits 0-7 writeMask; 8-15 compareMask; 16-18 depthPass op; 19-21 depthFail op; 22-24 stencilFail op (SceGxmStencilOp 0-7: KEEP ZERO REPLACE INCR DECR INVERT INCR_WRAP DECR_WRAP); 25-27 func (SceGxmStencilFunc pre-shifted: NEVER 0 ... ALWAYS 0xe000000)", "0x8100a818-0x8100a884"),
    ("+0x1d0", "4", "back ISP word, same layout as +0x1c4 (SetBack*; sceGxmSetTwoSidedEnable rewrites bits 21,25-27 of it from fprog+0x20 and +0x1bc)", "0x8100a5dc, 0x8100a6b4, 0x8100a9f4-0x8100aa24"),
    ("+0x1d4", "4", "back depth bias / visibility index, as +0x1c8", "0x8100a988, 0x8100ac60"),
    ("+0x1d8", "4", "back stencil word, as +0x1cc", "0x8100a8b0"),
    ("+0x1dc", "12", "the three fragment state words of the last flush (PDS/USSE program addresses; same as a precomputed fragment state's +0x18..+0x20, copied from it by the flush)", "sub_81006eb4 0x81006f74-0x81006f8c, 0x81008020-0x81008030"),
    ("+0x1e8", "4", "region clip word A: bits 30-31 mode (SceGxmRegionClipMode pre-shifted: NONE 0, ALL 0x40000000, OUTSIDE 0x80000000, INSIDE 0xc0000000); bits 0-7 xMax>>5 (32-pixel tiles); bits 16-23 xMin>>5. BeginScene's default (SetDefaultRegionClipAndViewport) is OUTSIDE with xMax = validRegion xMax (or width-1)", "SetRegionClip 0x8100ab04-0x8100ab78; SetDefault 0x8100ad54-0x8100ad98"),
    ("+0x1ec", "4", "region clip word B: bits 0-7 yMax>>5, bits 16-23 yMin>>5 (immediate contexts clamp yMax to the scene's +0x4b4)", "0x8100ab2c-0x8100ab78"),
    ("+0x1f0", "24", "viewport floats: xOffset, xScale, yOffset, yScale, zOffset, zScale, exactly sceGxmSetViewport's s0-s5. Scene default: xOffset = xScale = (xMax+1)/2, yOffset = (yMax+1)/2, yScale = -(yMax+1)/2, zOffset = zScale = 0.5", "SetViewport 0x8100aa58-0x8100aa70; SetDefault 0x8100ada4-0x8100ae0c"),
    ("+0x210", "4", "W clamp value (float, sceGxmSetWClampValue)", "0x8100aacc"),
    ("+0x214", "4", "bits 0-1 cull mode (SceGxmCullMode: NONE 0, CW 1, CCW 2); bit 14 W buffer enable (0x4000); bit 15 W clamp enable (0x8000); bit 16 viewport DISABLED (0x10000)", "SetCullMode 0x8100ab98; SetWBufferEnable 0x8100abe8; SetWClampEnable 0x8100aadc; SetViewportEnable 0x8100abc0"),
    ("+0x224", "4", "current SceGxmVertexProgram*", "SetVertexProgram 0x81003dd4; Draw 0x81004840"),
    ("+0x228", "4", "vertex default uniform buffer state: 0 none, 1 reserved, 2 committed, 3 set explicitly (as +0x48)", "0x81003f50; 0x8100412c; 0x81003e18"),
    ("+0x22c", "4*14", "vertex uniform buffers 0-13 (sceGxmSetVertexUniformBuffer); sentinel 0x3e000000 | i<<16 at creation; index 14 = +0x264", "0x81004558; CreateContext 0x81008774"),
    ("+0x264", "4", "vertex default uniform buffer (index 14)", "0x81003f3c; 0x810040ec"),
    ("+0x268", "4", "last vertex SA block B (vertex textures as SAs)", "sub_81006eb4 0x810077e8"),
    ("+0x26c", "12", "copied from vprog+0x38/+0x3c/+0x40 by SetVertexProgram", "0x81003de8-0x81003dfc"),
    ("+0x278", "4", "last vertex SA block A", "sub_81006eb4 0x810071f4"),
    ("+0x27c", "4", "reserved vertex default uniform buffer (Draw commits from here)", "0x81003f40"),
    ("+0x280", "4*16", "vertex streams 0-15: the data pointer from sceGxmSetVertexStream (stride is not here: it is in the vertex program's stream table, vprog+0x60). Sentinel 0x3d000000 | i<<20 at creation. The setter marks dirty only if the vertex program uses the stream (vprog+0x2c bit i)", "SetVertexStream 0x810041dc-0x81004204; CreateContext 0x81008648"),
    ("+0x2c0", "16*16", "vertex textures 0-15 (sceGxmSetVertexTexture / _sceGxmSetVertexTexture, 16 bytes each, word 0 with bits 29-30 cleared)", "0x81004268-0x810042a8 (r2 = ctx + 0x2b8 + 16i, words at r2+8..)"),
    ("+0x3c0", "4", "current SceGxmPrecomputedVertexState* (0 = none); +0x3c4 the one last flushed", "SetPrecomputedVertexState 0x81004684; sub_81006eb4 0x8100706c"),
    ("+0x3c8", "4", "last vertex uniform-buffer DMA table block (ring); +0x3cc/+0x3ce sizes", "sub_81006eb4 0x81007424-0x81007450"),
    ("+0x3d0", "12", "last vertex PDS fetch instance: +0x3d0 address in the vertex ring, +0x3d4h vprog+0x24, +0x3d6h its size (vprog+0x26). The same shape as a precomputed draw's +4..+0xb. sceGxmDraw passes this to the VDM draw emitter", "sub_81006e54 0x81006e6c-0x81006e78; Draw 0x810048c0"),
    ("+0x3d8", "1", "within a scene (BeginScene sets 1, EndScene 0; Draw and Reserve fail with 0x805b0006 outside)", "BeginScene 0x81005284; EndScene 0x81005e0c; Draw 0x81004834"),
    ("+0x3dc", "4", "-> a 16-byte block of 4 GPU read pointers, one per ring (vdm +0, vertex +4, fragment +8, fragment USSE +0xc), allocated from a shared kernel-visible pool (sub_8100ee48); the ring refill callback waits (sceGxmWaitEvent) until the GPU's value frees space", "CreateContext 0x810088a8-0x810088b4; sub_810153b4 [+0x1c]"),
    ("+0x3e0", "0x24", "VDM ring descriptor: +0 base, +4 offset, +8 size, +0xc usable (size - 0x104 for the VDM ring), +0x10 half, +0x14b is-VDM, +0x18 kick threshold, +0x1c -> read pointer word, +0x20 kick offset", "sub_810153b4"),
    ("+0x404", "0x24", "vertex ring descriptor (vertexRingBufferMem/Size)", "CreateContext 0x810088ec"),
    ("+0x428", "0x24", "fragment ring descriptor", "0x81008918"),
    ("+0x44c", "0x14", "fragment USSE ring cursor; +0x460 its descriptor (fragmentUsseRingBufferMem/Size/Offset)", "0x81008948-0x81008974"),
    ("+0x484", "0x24", "scene parameters copied at BeginScene from the defaults at +0x530 (+0x530: 0x80, +0x534h 0x3e8, +0x536h 0, +0x538 0xe9, +0x53c 1, +0x540 0, +0x54c 0, +0x550 1)", "CreateContext 0x81008978-0x810089ac; BeginScene 0x81005288-0x81005298"),
    ("+0x4a8", "4", "scene flags (BeginScene's `flags` & 0xf)", "BeginScene 0x81005258"),
    ("+0x4ac", "4", "scene render target (the SceGxmRenderTarget handle)", "0x81005240"),
    ("+0x4b0", "4", "scene xMax (validRegion->xMax, or render target width - 1); +0x4b4 yMax", "0x8100527c-0x81005280"),
    ("+0x4b8", "4", "validRegion pointer (0 when the colour surface is disabled)", "0x81005278"),
    ("+0x4bc", "4", "colour surface enabled flag", "0x8100524c"),
    ("+0x4c0", "4", "fragment sync object (vertexSyncObject must be NULL)", "0x81005268"),
    ("+0x4c4", "0x30", "**the scene's colour surface**: a copy of the SceGxmColorSurface passed to BeginScene (InitDisabled'd copy when NULL)", "0x810050a0-0x810050bc; 0x81005494"),
    ("+0x4f4", "0x1c", "**the scene's depth/stencil**: +0x4f4 zlsControl (BeginSceneEx merges the load surface's format bits 0x00e0f002 with the store surface's), +0x4f8/+0x4fc depth/stencil data to load, +0x500/+0x504 depth/stencil data to store (same pointers for BeginScene), +0x508 background depth, +0x50c background control. Disabled: 0, zeros, 1.0f, 0x300", "BeginScene 0x81005398-0x810053d8; BeginSceneEx 0x810056e0-0x81005738; disabled 0x8100510c-0x81005134"),
    ("+0x510", "4", "samples per pixel (render target msaa x * y)", "0x81005270"),
]))

LAYOUTS.append(("SceGxmRenderTarget and SceGxmSyncObject (kernel handles)", """
Neither is a libgxm object. `sceGxmCreateRenderTarget` validates the params, allocates the driver
memblock when `driverMemBlock == -1` (sceKernelAllocMemBlock type 0x0c208060, size from
sub_810141d8 = sceGxmGetRenderTargetMemSize), maps it (SceGpuEs4User_0EA7458D -> bridge 0x1058),
then calls SceGpuEs4User_0162A405 with a 0x44-byte description; that is bridge call 0x1086 to the
kernel GPU driver (in 0x4c: {?, device handle, the 0x44 bytes}; out {error, handle}) and the
handle is what the game holds as SceGxmRenderTarget*. BeginScene queries it back with
SceGpuEs4User_83D972DF = bridge 0x108b (in {handle}, out {error, width, height, msaaX h | msaaY h
<< 16, exact-size flag}); DestroyRenderTarget uses 0x108c? (D90B0707) and 0x1089 (804BA13D, the
memblock lookup also used by sceGxmRenderTargetGetDriverMemBlock). In the recomp the kernel side is
runtime/gfx/gpu.c, so the render target's fields live wherever the host's 0x1086 handler keeps them.
A sync object is likewise a handle from bridge 0x1055 (SceGpuEs4User_94286660).
""", [
    ("+0x0", "4", "bridge 0x1086 description: flags: 2, or 3 unless params.flags & 0x10; | 0x10000 if params.flags & 1 (custom multisample locations); | 0x60000 if & 2; | 0x80000 if & 4; | 0x100000 if & 8", "CreateRenderTarget 0x81014bcc-0x81014cdc"),
    ("+0x4", "4", "scenesPerFrame * 2", "0x81014be0"),
    ("+0x8", "4", "scenesPerFrame * (3, or 2 with flag 0x10, or sub_8100b968(global)+1)", "0x81014bf4"),
    ("+0xc", "4", "global [0x81024000+0x1a0] (the GPU render context handle from sceGxmInitialize)", "0x81014c00"),
    ("+0x10", "4", "global [0x81024000+0xa8]", "0x81014c04"),
    ("+0x14", "4", "width; +0x18 height (uint16 params widened)", "0x81014c10-0x81014c14"),
    ("+0x1c", "2+2", "multisample: x samples (1, 2 for 2X/4X), y samples (1, 2 for 4X) from SceGxmMultisampleMode", "0x81014c20-0x81014c30, 0x81014d70"),
    ("+0x20", "16", "0, 0, 1.0f, 0", "0x81014c44-0x81014cd4"),
    ("+0x30", "4", "halfword 1; byte +0x32 = flags>>8 & 15; byte +0x33 = flags>>12 & 15", "0x81014c80-0x81014cb4"),
    ("+0x34", "4", "driver memblock UID; +0x38 1 if libgxm allocated it; +0x3c memblock base; +0x40 multisampleLocations", "0x81014c9c-0x81014cc0"),
]))

LAYOUTS.append(("Registered program (SceGxmShaderPatcherId, 0x24 bytes)", """
`sceGxmShaderPatcherRegisterProgram(patcher, gxp, &id)` allocates this through the patcher's host
allocator; `sceGxmShaderPatcherGetProgramFromId` returns its first word.
""", [
    ("+0x0", "4", "const SceGxmProgram* (the GXP)", "RegisterProgram 0x81015c9c"),
    ("+0x4", "4", "buffer-memory copy of the GXP's code (4*gxp[+0x58] bytes from gxp+0x68+gxp[+0x68]), 0 if none", "0x81015dd0-0x81015e3c"),
    ("+0x8", "4", "buffer allocation of 4*gxp[+0x50] bytes (0 if none); +0xc one of 4*gxp[+0x54]", "0x81015d5c-0x81015dc8"),
    ("+0x10", "4", "list head of vertex programs made from it (linked by vprog+0x74); +0x14 fragment programs (fprog+0x7c)", "0x81015cfc-0x81015d00; CreateVertexProgram 0x810164bc; CreateFragmentProgram 0x81016ed8"),
    ("+0x18", "4", "patcher; +0x1c/+0x20 prev/next in the patcher's registered list", "0x81015cf8-0x81015d18"),
]))

LAYOUTS.append(("SceGxmVertexProgram (0x78-byte header + attributes + streams + patch table + PDS template)", """
`sceGxmShaderPatcherCreateVertexProgram(patcher, id, attributes, attributeCount, streams,
streamCount, &vertexProgram)`. A cache hit (same id, identical attribute and stream arrays)
returns the existing object with +0x68 incremented. `sceGxmVertexProgramGetProgram` = `ldr [+0]`.
""", [
    ("+0x0", "4", "const SceGxmProgram* (GXP)", "0x810166a0; GetProgram 0x8101f9e0"),
    ("+0x10", "8", "two words written by sub_810139cc(gxp) (the trailer the flush appends after the uniform DMA table: {vprog+0x10, vprog+0x14, 0})", "0x81016894; sub_81006eb4 0x810076ec-0x81007720"),
    ("+0x1c", "4", "context dirty bits set when the program is bound: 0x700 | 0x40 if gxp[+0x60] != 0 | 0x80 if gxp[+0x5c] != 0", "0x81016900-0x81016938"),
    ("+0x20", "8", "PDS vertex-fetch template descriptor: +0x20 pointer to the template (inside this object, after the stream table), +0x24h, +0x26h size in bytes (`sceGxmGetPrecomputedDrawSize` returns +0x26)", "0x810168fc; sub_8100f8ac; GetPrecomputedDrawSize 0x81012e1c"),
    ("+0x28", "4", "bytes the uniform-buffer DMA table needs (sub_810105e4); `sceGxmGetPrecomputedVertexStateSize` = 4*(gxp[+0x5c]+gxp[+0x60]) + this", "0x810169d8; 0x81010ac0"),
    ("+0x2c", "4", "stream usage mask: bit i set if some attribute reads stream i (SetVertexStream's dirty test)", "0x81016a4c-0x81016c44"),
    ("+0x30", "4", "-> attribute patch table, attributeCount entries of 8 bytes {u16 streamIndex, u16 offset, u32 wordIndex}: the fetch instance word `wordIndex` = streamData[streamIndex] + offset. **This is how stream addresses enter a draw**: for the context, sub_8100ff24(instance, table, count, ctx+0x280); for a precomputed draw, sceGxmPrecomputedDrawSetVertexStream -> sub_810100dc. To read stream i back from an instance: instance[wordIndex] - offset for any entry with streamIndex i", "0x810169e0; sub_8100ff24; sub_810100dc 0x810100f4-0x81010110"),
    ("+0x34", "4", "attributeCount (the patch table's length)", "0x810169dc"),
    ("+0x38", "12", "copies of id+4, id+8, id+0xc (the GPU allocations), copied to ctx+0x26c on SetVertexProgram", "0x810169e4-0x810169fc"),
    ("+0x44", "8", "VDM vertex info from sub_8100b9a0(gxp byte +0x13, gxp[+0x14] bit 20, gxp halfwords +0x30/+0x32/+0x34): byte +0x44 = output vertex size in words clamped to 8 (-1 in the draw word 2 bits 28-31), byte +0x45 = min(16, that, 4)-ish count (-1 in word 4 bits 28-31), word +0x48 = (primary attribute size<<7) | ... (draw word 5)", "0x81016a00-0x81016a20; sub_8100b9a0; sub_8101f738 0x8101f7a8-0x8101f7e8"),
    ("+0x4c", "4", "host allocation size; +0x50 vertex USSE allocation (the code copy's GPU address); +0x54 its size", "0x81016698, 0x8101666c, 0x81016690"),
    ("+0x58", "4", "-> SceGxmVertexAttribute[attributeCount] (copy, at +0x78); +0x5c attributeCount", "0x81016558, 0x81016a2c, 0x81016a38"),
    ("+0x60", "4", "-> SceGxmVertexStream[streamCount] (copy: {u16 stride, u16 indexSource}); +0x64 streamCount. **The stride of stream i is +0x60[i].stride**", "0x81016580, 0x81016a30, 0x81016a44"),
    ("+0x68", "4", "reference count; +0x6c patcher; +0x70 registered program (id); +0x74 next vertex program of the same id", "0x81016828, 0x81016c4c, 0x81016c48, 0x81016c60"),
    ("+0x78", "8*n", "attributes copy, then streams copy (4*m), then the patch table (8*n), then the PDS template", "0x81016554-0x81016590"),
]))

LAYOUTS.append(("SceGxmFragmentProgram (0x80-byte header + sampler list + blend copy + PDS template)", """
`sceGxmShaderPatcherCreateFragmentProgram(patcher, id, outputFormat, multisampleMode, blendInfo,
vertexProgram (GXP), &fragmentProgram)`. Cache key: the 10-byte varying remap (+0x58, derived from
the vertex GXP's output mask at vertexGxp+0x2c-block+0x14), outputFormat, the 4 blend bytes,
multisampleMode (when the program is multisample-aware). `sceGxmFragmentProgramGetProgram` = `ldr [+0]`.
""", [
    ("+0x0", "4", "const SceGxmProgram* (GXP)", "0x8101738c; GetProgram 0x8100c230"),
    ("+0x4", "12", "fragment USSE/PDS program words written by sub_8101f04c (state built at sp+0x74 by sub_8101edd0 from the blend info and output format: the blend is compiled into the patched USSE code, not kept as state -- there is no per-draw blend word in the context)", "0x81017394"),
    ("+0x10", "8", "two words from sub_810139cc(gxp) (trailer after the DMA table, as the vertex program's)", "0x810175cc"),
    ("+0x1c", "4", "context dirty bits when bound: 0x38 | 2 if gxp[+0x60] | 4 if gxp[+0x5c]", "0x810176a4-0x810176c0"),
    ("+0x20", "4", "**ISP bits merged into the context's front/back words**: bits 25-27 pass type (SceGxmPassType: TRANSLUCENT 0x2000000 when blending/colour needs the previous value, DISCARD 0x4000000 if gxp[+0x14] & 8, DEPTH_REPLACE 0xa000000 if & 0x10); bit 21 SCE_GXM_FRAGMENT_PROGRAM_DISABLED when the program writes no colour (gxp[+0x14] & 0x4018 == 0). `sceGxmFragmentProgramGetPassType` = +0x20 & 0xe000000; `IsEnabled` = !(+0x20 & 0x200000)", "0x810176c4-0x81017708; 0x8100c238; 0x8100c244"),
    ("+0x24", "4", "sub_81013864(gxp, multisampleMode) result (passed to the state-word builder sub_8100f4f8)", "0x81017720"),
    ("+0x28", "4", "-> PDS fragment template (textures-then-code block the flush instantiates); +0x2ch, +0x2eh its size (`sceGxmGetPrecomputedFragmentStateSize` = 4*(gxp[+0x5c]+gxp[+0x60]) + fprog[+0x30] + align16(+0x2e))", "sub_81010240 writes +0x28..; 0x81011ea4; sub_81006eb4 0x81007ec0-0x8100800c"),
    ("+0x30", "4", "bytes of the uniform-buffer DMA table (sub_810105e4)", "0x81017788"),
    ("+0x34", "4", "sampler count (from the GXP's descriptor block, halfword +0xe); +0x38 -> list of texture unit indices in sampler order (count words, at +0x80). Textures are copied into a state in this order (16 bytes each)", "0x8101778c, 0x81017794; PrecomputedFragmentStateSetAllTextures 0x810125c8-0x810125d0"),
    ("+0x3c", "12", "copies of id+4, id+8, id+0xc; copied to ctx+0x8c on SetFragmentProgram", "0x810177a4-0x810177e0"),
    ("+0x48", "4", "a small output/multisample mode code: 1/2 (32/64-bit output register), 5/6, 9/0xa or 0x11/0x12 by multisample mode, | 0x20 when blending needs it", "0x810177a0-0x81017820"),
    ("+0x4c", "4", "host allocation size; +0x50 fragment USSE allocation; +0x54 its size", "0x8101737c, 0x81017360, 0x81017384"),
    ("+0x58", "10", "varying remap bytes (cache key)", "0x8101729c-0x810172a4, 0x8101761c-0x81017658"),
    ("+0x64", "4", "outputFormat (SceGxmOutputRegisterFormat)", "0x810172b4"),
    ("+0x68", "4", "-> 4-byte copy of the SceGxmBlendInfo {colorMask; colorFunc:4 alphaFunc:4; colorSrc:4 colorDst:4; alphaSrc:4 alphaDst:4} inside this object, or 0 when blendInfo was NULL (no blending)", "0x81017248-0x81017258; 0x810175bc"),
    ("+0x6c", "4", "multisampleMode (SceGxmMultisampleMode)", "0x81017288"),
    ("+0x70", "4", "reference count; +0x74 patcher; +0x78 id; +0x7c next fragment program of the same id", "0x8101780c-0x8101782c"),
    ("+0x80", "4*n", "texture unit index per sampler, then the blend copy, then the PDS template", "0x81017234-0x81017254"),
]))

LAYOUTS.append(("SceGxmProgram (GXP) fields libgxm reads to build state", """
Beyond the parameter table the host already parses (+0x24 count, +0x28 offset, +0x64 default
uniform buffer size in words), the state code uses these tables; all offsets are self-relative
(table = gxp + offsetField + *offsetField).
""", [
    ("+0x14", "4", "flags: bit 3 discard, bit 4 depth replace, bits 3-4 and 14 (0x4018) colour written; bit 20 used by the vertex info", "CreateFragmentProgram 0x810176c4-0x810176d4, 0x81016dbc"),
    ("+0x18", "4", "uniform buffer usage: 2 bits per buffer, bits 2i-2i+1 for buffer i, bits 28-29 for the default buffer (14): bit 1 of the pair -> SA block A (dirty 0x380/0x2c), else DMA table (0x300/0x28)", "SetVertexUniformBuffer 0x8100456c-0x8100458c; Reserve 0x81003ee4-0x81003f00"),
    ("+0x1c", "8", "texture usage: 4 bits per texture unit (unit i at bit 4i, 16 units over two words): 4 -> SA block A, 2, 1", "SetFragmentTexture 0x810044b4-0x810044f4"),
    ("+0x2c", "4", "offset to a descriptor block: +0xc h, +0xe h sampler count, +0x10 offset, +0x14 vertex output mask (3-bit fields per varying)", "CreateFragmentProgram 0x81016dec-0x81016e00, 0x810171a0-0x810171bc"),
    ("+0x30", "6", "halfwords for the VDM vertex info (sub_8100b9a0); +0x32 h also passed to the vertex state-word emitter", "0x81016a08-0x81016a18; 0x81007740"),
    ("+0x50/+0x54/+0x58", "12", "sizes in words of the three allocations RegisterProgram makes (+0x58 = code, copied from +0x68 + *+0x68)", "RegisterProgram 0x81015ca0-0x81015cf4"),
    ("+0x5c", "4", "SA block A size in words (uniform buffer addresses, literals, texture words); +0x60 SA block B size (texture words via the second table)", "GetPrecomputedVertexStateSize 0x81010ac8; flush 0x810071c4, 0x810077b4"),
    ("+0x70/+0x74", "8", "literal table: count / offset; entries 8 bytes {u16 saIndex, u16 -, u32 value}: SA_A[saIndex] = value", "flush 0x81007258-0x810072a0"),
    ("+0x78/+0x7c", "8", "uniform-buffer-to-SA table: {u16 bufferIndex (14 = default), u16 saIndex, u32 byteOffset}: SA_A[saIndex] = buffer[bufferIndex] + byteOffset", "flush 0x810071fc-0x81007250; PrecomputedVertexStateSetUniformBuffer 0x81011e14-0x81011e64"),
    ("+0x80/+0x84", "8", "texture-to-SA-A table: {u16 (unit << 2 | word), u16 saIndex}: SA_A[saIndex] = texture[unit].word[word]", "flush 0x810072ac-0x810073a0; PrecomputedFragmentStateSetTexture 0x81012b44-0x81012b98"),
    ("+0x88/+0x8c", "8", "texture-to-SA-B table, same entries, into SA block B", "flush 0x810077f4-0x810078ec"),
    ("+0x90/+0x94", "8", "uniform-buffer DMA table: count / offset; entries 8 bytes {u16 bufferIndex, u16 wordOffset, u16 sizeWords, u16 -}: DMA entry k word 0 = buffer[bufferIndex] + 4*wordOffset, word 1 = (sizeWords-1) | ... (flush 0x81007468-0x810076b0). **To read uniform buffer b back from a state or the ring: DMA[k].word0 - 4*wordOffset for the k with bufferIndex b**", "flush 0x810073ec-0x81007450; PrecomputedFragmentStateSetAllUniformBuffers 0x81012c4c-0x81012c94"),
]))

LAYOUTS.append(("SceGxmPrecomputedDraw (11 words)", """
`sceGxmPrecomputedDrawInit(draw, vertexProgram, memBlock)` (memBlock 16-aligned, size =
`sceGxmGetPrecomputedDrawSize` = vprog+0x26 = the PDS template size) copies the vertex program's
PDS fetch template into memBlock; SetVertexStream/SetAllVertexStreams patch stream addresses into
that instance through the vertex program's patch table; SetParams builds the six VDM draw words
in place. `sceGxmDrawPrecomputed` then copies those 24 bytes into the VDM ring (0x81004764) --
no other state is touched by the draw itself (textures/uniforms come from the bound precomputed
vertex/fragment states at flush time).
""", [
    ("+0x0", "4", "const SceGxmVertexProgram*", "Init 0x81012e70"),
    ("+0x4", "4", "PDS fetch instance address (= memBlock rounded up to 16); +0x8h vprog+0x24; +0xah instance size", "Init 0x81012e8c (sub_8100f8ac 0x8100f8fc-0x8100f908)"),
    ("+0xc", "8", "unused", ""),
    ("+0x14", "24", "VDM draw words (sub_8101f738): w0 = 0x81c00000 | primType (SceGxmPrimitiveType pre-shifted, bits 26-28) | indexCount; w1 = indexData | 1 (bit 0 = 'from a precomputed draw'); w2 = indexFormat (SCE_GXM_INDEX_FORMAT_U32 = 0x01000000) | (vprog byte +0x44 - 1) << 28; w3 = indexWrap & 0x3fffff (SetParamsInstanced; 0 otherwise); w4 = instance >> 4 | (vprog byte +0x45 - 1) << 28; w5 = vprog[+0x48] | 0x100 | instanceSize >> 4. **So primitive, index format, indices and count are recoverable from +0x14, +0x18 (& ~1), +0x1c (& 0x01000000), and the streams from the instance via vprog+0x30**", "SetParams 0x81013054-0x81013098; sub_8101f738 0x8101f77c-0x8101f7e8"),
]))

LAYOUTS.append(("SceGxmPrecomputedVertexState (7 words) and SceGxmPrecomputedFragmentState (9 words)", """
Init (state, program, memBlock) lays the program's per-draw GPU data out in memBlock and keeps
pointers to the pieces; the Set* calls patch the pieces. Both states are mirror images of what
the Draw-time flush writes into the rings for the immediate path (same tables, same word
formats), which is why `sceGxmSetPrecomputed*State` just swaps the pointer and the flush copies
the state's final words. The uniform buffers and textures are therefore not stored as such: they
are already converted to SA words and DMA entries, recoverable through the GXP tables above.
""", [
    ("V +0x0", "4", "const SceGxmVertexProgram*", "VertexStateInit 0x81010b70"),
    ("V +0x4", "4", "-> uniform-buffer DMA table block (memBlock + 4*(gxp[+0x5c]+gxp[+0x60]), 16-aligned), 8-byte entries ordered as the GXP +0x94 table; SetUniformBuffer/SetAllUniformBuffers write entry.word0 = buffer + 4*wordOffset; SetDefaultUniformBuffer writes the entry whose bufferIndex is 14", "0x81010b80; 0x81011e7c-0x81011e84; 0x81011170-0x81011178"),
    ("V +0x8", "4", "default uniform buffer pointer as given to SetDefaultUniformBuffer (`GetDefaultUniformBuffer` reads it); 0 at Init", "0x810110a8; 0x81011088"),
    ("V +0xc", "4", "-> SA block A (memBlock, 4*gxp[+0x5c] bytes) or 0; receives uniform buffer addresses (+0x78 table), literals, vertex texture words (+0x80 table)", "0x81010b94; 0x81011158; 0x81011398"),
    ("V +0x10", "4", "-> SA block B (4*gxp[+0x60] bytes) or 0; vertex texture words via the +0x88 table", "0x81010b9c; 0x810113f0"),
    ("V +0x14", "8", "two VDM vertex-state words (sub_8101f680): w0 = DMA table >> 4 | 0x40000000; w1 = (table size >> 4) << 27 | 0xe100 | (gxp h +0x32 + 3) >> 2 | 0x4000000 if flag. The flush copies these 8 bytes into the VDM ring when the state changes (0x81007058-0x81007068)", "0x8101101c-0x81011034; sub_8101f680"),
    ("F +0x0", "4", "const SceGxmFragmentProgram*", "FragmentStateInit 0x81011f88"),
    ("F +0x4", "4", "-> PDS fragment block (memBlock): the program's textures first, 16 bytes per sampler in fprog+0x38 order (SetTexture/SetAllTextures copy them here with word 0 bits 29-30 cleared), then the template", "0x81011fa0; 0x810125f0-0x81012664; 0x81012b24-0x81012b38"),
    ("F +0x8", "4", "-> uniform-buffer DMA table block (after the texture block, 16-aligned), as V +0x4", "0x81011fa0; 0x81012dfc"),
    ("F +0xc", "4", "default uniform buffer pointer (`GetDefaultUniformBuffer`); 0 at Init", "0x810124c0; 0x810125a4"),
    ("F +0x10", "4", "-> SA block A or 0 (uniform buffers via +0x78, literals, textures via +0x80)", "0x81011fa8; 0x81012dd8; 0x81012b90"),
    ("F +0x14", "4", "-> SA block B or 0 (textures via +0x88)", "0x81011fb0; 0x81012be8"),
    ("F +0x18", "12", "three fragment state words (sub_8100f4f8(out, dmaDesc, fprog+0x24, pdsDesc)): w0 = DMA table >> 4 | (size >> 4) << 27; w1 = fprog[+0x24] | 0x4000 if the table address is 'negative' | 0x2000 if the PDS block's is; w2 = PDS block >> 4 | (size >> 4) << 27. The flush copies them to ctx+0x1dc when the state changes", "0x81012434-0x8101244c; sub_8100f4f8; flush 0x81006f74-0x81006f8c"),
]))

LAYOUTS.append(("SceGxmNotification and the notification region", """
`sceGxmGetNotificationRegion` returns [global +0x2b4]->+4 (a memblock base) + 0x20; the region's
size is at the same struct's +0x14. `sceGxmNotificationWait(n)` checks n->address lies in the
region (else 0x805b0003), then loops `while (*n->address != n->value) sceGpuSignalWait(tls, tls,
100000)`. So a notification is {volatile uint32 *address; uint32 value} as the header says, and the
GPU (host) writes `value` to `address` when the scene that carried it completes.
""", []))


HOST_NOTES = """
## What Draw, BeginScene and EndScene do that the boundary skips

`sceGxmDraw(ctx, prim, indexFormat, indices, count)` (0x81004810):

1. Fails unless within a scene (+0x3d8), a vertex (+0x224) and fragment (+0x44) program are bound.
2. **Commits the reserved default uniform buffers**: if +0x228 == 1, +0x1c = +0x27c + 4*vertexGxp[+0x64]
   and +0x228 = 2; if +0x48 == 1, +0x30 = +0x9c + 4*fragmentGxp[+0x64] and +0x48 = 2. Without this,
   the next `Reserve*DefaultUniformBuffer` returns the **same address** (the cursor never moved) --
   fine only if the host consumed the uniform data at the draw, catastrophic if it reads it later.
3. `sub_81006eb4(ctx)`: the state flush. Allocates, in the vertex and fragment rings, the SA blocks
   (uniform buffer addresses/literals/texture words as secondary attributes), the uniform-buffer DMA
   tables, the fragment PDS block (a copy of the bound fragment textures and the program's PDS
   template) and, in the VDM ring, the vertex state words; or copies a bound precomputed state's
   final words instead. Also validates: default uniform buffer not reserved though the program has
   one -> 0x805b0011; depth/stencil test on a scene without that surface -> 0x805b001f.
4. `sub_81006e54(ctx)`: if a vertex stream changed, instantiates the vertex program's PDS fetch
   template in the vertex ring (+0x3d0) and patches the 16 stream pointers in through vprog+0x30.
5. `sub_8101f738`: the six VDM draw words (see SceGxmPrecomputedDraw) into the VDM ring.
6. For an immediate context, `sub_81009a78(ctx)` when a kick is due.

None of this matters to a host renderer except (2), and the ring arithmetic it protects: the
rings are the game's `vertexRingBufferMem` etc. from SceGxmContextParams, so every default uniform
buffer the game fills is at a game address inside that memory. When a cursor reaches its end,
the refill callback (0x81015290) consults the GPU read pointer at [+0x3dc]+4/+8 (a word only the GPU
-- here the host -- writes) and calls `sceGxmWaitEvent` until it allows a wrap; the kick path
(`sub_81009a78` -> `sub_81015430`) does the same bookkeeping at scene end. With Draw and EndScene
intercepted, Sony's code never advances the read pointers nor runs the kick, so the first wrap
(after vertexRingBufferMemSize bytes of uniforms, 2 MB by default) spins in sceGxmWaitEvent. The
host must either write the read pointers as a GPU would, or, at its EndScene (everything consumed),
rewind the cursors to their ring bases: +0x1c/+0x20 from the descriptor at +0x404 (base, usable),
+0x30/+0x34 from +0x428, +0x8/+0xc from +0x3e0, +0x44c/+0x450 from +0x460.

`sceGxmBeginScene(ctx, flags, renderTarget, validRegion, vertexSync (must be NULL), fragmentSync,
colorSurface, depthStencil)` (0x81005020): copies the colour surface to +0x4c4 (48 bytes) and the
depth/stencil to +0x4f4 (7 words, see the context table), queries the render target's size and
multisample mode from the GPU driver (bridge 0x108b), validates the surfaces against it (the colour
surface's background texture width/height, the depth stride), records flags/target/region/sync at
+0x4a8..+0x4c0, sets +0x3d8 = 1, +0x4 |= 0x800, copies the scene defaults +0x530 -> +0x484, and calls
`sceGxmSetDefaultRegionClipAndViewport(ctx, xMax, yMax)`: region clip OUTSIDE 0..xMax/yMax and the
viewport xOffset = xScale = (xMax+1)/2, yOffset = (yMax+1)/2, yScale = -(yMax+1)/2, z 0.5/0.5.
**A host BeginScene that does not run Sony's code must at least set +0x3d8 (or the setters'
dependents misbehave: Draw and Reserve refuse outside a scene) and apply the default clip and
viewport to +0x1e8..+0x204, since games rely on them.** `sceGxmBeginSceneEx` takes a load and a
store depth/stencil surface: +0x4f8/+0x4fc from the load surface, +0x500/+0x504 from the store one.

`sceGxmEndScene(ctx, vertexNotification, fragmentNotification)` (0x81005d58): commits the default
uniform buffers as Draw does, runs the kick (`sub_81009050(ctx, 2, vn, fn)`), clears +0x3d8.
`sceGxmMidSceneFlush` is the same with flag 0x80 and keeps the scene open.

## The host's decode, in short

| what the renderer needs | where |
|---|---|
| vertex GXP, attributes, strides | vprog = ctx+0x224: GXP at +0; attributes at +0x58 (count +0x5c); streams {stride, indexSource} at +0x60 (count +0x64) |
| vertex stream data | ctx+0x280 + 4i (immediate); precomputed: instance [draw+4] word `wordIndex` - offset per vprog+0x30 entry |
| fragment GXP, blend, output format | fprog = ctx+0x44: GXP +0; blend info copy at [+0x68] (0 = none); output format +0x64; multisample +0x6c; pass type +0x20 |
| textures | fragment ctx+0xa0 + 16i, vertex ctx+0x2c0 + 16i; precomputed fragment: [state+4] + 16k for sampler k -> unit fprog[+0x38][k] |
| default uniform buffers | vertex ctx+0x264 (committed copy +0x27c), fragment ctx+0x84 (+0x9c); precomputed: state V+0x8 / F+0xc |
| uniform buffers 0-13 | vertex ctx+0x22c + 4i, fragment ctx+0x4c + 4i; precomputed: DMA table [V+4]/[F+8] entries via GXP +0x94, or SA block via GXP +0x7c |
| depth func / write, stencil, polygon, line width, two-sided, vis test | ctx+0x1c4 (front), +0x1d0 (back); stencil ops/masks +0x1cc / +0x1d8 |
| cull, viewport enable, W buffer/clamp | ctx+0x214; viewport floats +0x1f0; region clip +0x1e8/+0x1ec; W clamp +0x210 |
| visibility buffer | ctx+0x544 base, +0x548 stride per core (sceGxmSetVisibilityBuffer, outside a scene only) |
| precomputed states bound | ctx+0x3c0 (vertex), +0x1a0 (fragment) |
| scene surfaces | ctx+0x4c4 (SceGxmColorSurface copy), +0x4f4 (depth/stencil: control, load depth/stencil, store depth/stencil, bg depth, bg control); target handle +0x4ac; region xMax/yMax +0x4b0/+0x4b4; samples +0x510 |

## Where Vita3K disagrees

- Vita3K's `SceGxmColorSurface` is a hand-made struct (width/height/stride/data as plain words); the
  firmware's is the PBE word layout above (format at +4, data at +8, stride/2-1 at +0xc, clip at
  +0x14/+0x18, output register size at +0x1c). Vita3K notes this itself.
- Vita3K's `SceGxmTexture` word-0 bitfield agrees with the firmware (unk0:3 vaddr:3 uaddr:3 mip:1
  min:2 mag:2 unk1:3 mip_count:4 lod_bias:6 gamma:2 unk2:2 format0:1); its `unk1` bits 14-16 are
  unused by any libgxm accessor here, and for LINEAR_STRIDED the firmware puts the stride in bits
  1-2, 9-11 and 17-26 (Vita3K handles that type separately).
- Vita3K's precomputed draw/state structs are emulator-internal (stream_data, texture_count...); the
  firmware's are the pointers-into-memBlock layouts above, and the draw keeps its parameters only
  as the six VDM words.
- Vita3K keeps the uniform buffer and viewport state in its own GxmContextState; offsets above are
  the firmware's.
"""


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) < 2:
        print(__doc__); sys.exit(1)
    elf, header = args[0], args[1]
    out = open(args[2], "w") if len(args) > 2 else sys.stdout
    use_unicorn = "--no-unicorn" not in sys.argv
    lib = Lib(elf)
    E = enums(header)
    w = out.write
    w(f"# libgxm object map\n\nGenerated by `dev/research/gxmobjects.py` from `{os.path.basename(elf)}` and "
      f"`{header.split('/include/')[-1]}`. Derived from Sony code: local, never committed.\n")
    w(__doc__.split("\n\n", 1)[1].split("usage:")[0].replace("  1.", "\n1.").replace("  2.", "2.").replace("  3.", "3."))

    # ---- 3. the layouts
    for title, intro, fields in LAYOUTS:
        w(f"\n## {title}\n{intro}\n")
        if fields:
            w("| offset | size | meaning | read at |\n|---|---|---|---|\n")
            for off, size, meaning, ev in fields:
                w(f"| `{off}` | {size} | {meaning} | {ev} |\n")

    w(HOST_NOTES)

    # ---- 1. the firmware, run
    if use_unicorn:
        w("\n## Empirical check: the firmware run in Unicorn\n\nEach init/setter is run on zeroed memory with a "
          "baseline, then with one argument changed; 'bits changed' are the XOR of the object's words against the "
          "baseline (wN[lo..hi] = word N, bit range). rc is the function's return (0 = success).\n")
        try:
            run = Runner(lib)
            rows = experiments(run, E)
        except Exception as e:   # Unicorn missing or a crash: the map still carries the reading
            rows = []
            w(f"\n(unicorn run failed: {e!r})\n")
        for kind, cells in rows:
            if kind.startswith("## "):
                if kind.endswith("setters"):
                    w(f"\n### {kind[3:]}\n\n| " + " | ".join(cells) + " |\n|" + "---|" * len(cells) + "\n")
                else:
                    w(f"\n### {kind[3:]}\n\nbaseline: {cells[1]}: {cells[2]}, words `{cells[3]}`\n\n| argument | value | rc | words | bits changed |\n|---|---|---|---|---|\n")
            else:
                w("| " + " | ".join(cells) + " |\n")

    # ---- 2. the firmware, read: accessor evidence
    w("\n## Accessor evidence (disassembly)\n\nFor each small getter/setter: its memory accesses through the object "
      "(r0) and the bit operations around them, in program order; the offsets and masks above come from these.\n\n")
    groups = [("Texture", "sceGxmTexture"), ("Colour surface", "sceGxmColorSurface"), ("Depth/stencil surface", "sceGxmDepthStencilSurface"),
              ("Programs", "sceGxmVertexProgramGet"), ("Programs", "sceGxmFragmentProgramGet"), ("Programs", "sceGxmFragmentProgramIs"),
              ("Precomputed", "sceGxmPrecomputed"), ("Precomputed", "sceGxmGetPrecomputed")]
    done = set()
    for gname, prefix in groups:
        names = sorted(n for n in lib.ex if n.startswith(prefix) and n not in done and not n.endswith("Init") and "InitDisabled" not in n)
        if not names:
            continue
        w(f"\n### {gname}\n\n| function | accesses and bit operations |\n|---|---|\n")
        for n in names:
            ins = lib.insns(n)
            if len(ins) > 120:   # the big ones are read by hand above
                continue
            done.add(n)
            ev = accessor_evidence(lib, n)
            if ev:
                w(f"| `{n}` | {' ; '.join(ev)} |\n")

    # ---- 2. context stores with alias tracking
    w("\n## Context stores, every setter (alias-tracked)\n\nStores through any register holding the context (or the "
      "context plus an index): offset, width, and the functions. This is the mechanical complement of the context table "
      "above; `+off[reg*n]` is an indexed array store.\n\n")
    by_off = defaultdict(set)
    for n in sorted(lib.ex):
        if not (n.startswith("sceGxmSet") or n.startswith("sceGxmReserve") or n in ("sceGxmBeginScene", "sceGxmBeginSceneEx", "sceGxmEndScene", "sceGxmCreateContext", "sceGxmSetVisibilityBuffer")):
            continue
        if n.startswith("sceGxmSetUniformDataF") or "Deferred" in n or "Internal" in n:
            continue
        for off, width, idx in context_stores(lib, n):
            if 0 <= off < 0x600:
                by_off[(off, idx or "")].add(n)
    w("| offset | written by |\n|---|---|\n")
    for (off, idx), fns in sorted(by_off.items()):
        fl = sorted(fns)
        w(f"| `+{off:#05x}{'[' + idx + ']' if idx else ''}` | {', '.join(f'`{f}`' for f in fl[:10])}{' …' if len(fl) > 10 else ''} ({len(fl)}) |\n")


if __name__ == "__main__":
    main()
