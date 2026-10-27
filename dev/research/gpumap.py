#!/usr/bin/env python3
"""The GPU bridge map: how Sony's user-mode GPU stack (libgxm -> libgpu_es4, the PowerVR services
client) reaches the kernel driver (SceGpuEs4) through PVRSRV_BridgeDispatchKM(id, package), and what the
host must return for each bridge id so the client's own code proceeds.

Four sources, all from the decrypted firmware:
  1. the kernel's dispatch table (its init function run under Unicorn), and a static scan of every
     handler: the input offsets it reads, the output offsets it writes, the kernel services it calls;
  2. every libgpu_es4 export run under Unicorn with pattern-tagged arguments and a recording bridge:
     which id it issues, which argument lands at which input offset, which output words it hands back
     to the caller (so the user-side protocol of each id is read off, not guessed);
  3. libgxm's call graph: which of its exports reach which libgpu exports;
  4. libgxm itself run under Unicorn over libgpu with a Python host (the recipe below) answering the
     bridge: sceGxmInitialize, MapMemory, Map*UsseMemory, CreateContext, CreateRenderTarget,
     SyncObjectCreate and ShaderPatcherCreate must return 0 -- the recipe is proven by libgxm's own
     success codes, and every host-returned word libgxm consumes is listed (taint on tagged words).

  gpumap.py [--out dump/decrypted/maps/gpu-bridge.md]

Derived from Sony code: the map stays local (under dump/), never committed.
"""
import collections, os, re, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, "..", "..", "recompiler"))   # (platmap beside this; analyze in the recompiler)
from analyze import Module, set_thumb_imm16
from platmap import Mod, nid_names, DEC
from unicorn import *
from unicorn.arm_const import *
from capstone import arm as A
from capstone.arm_const import *

N = nid_names()
KERNEL = os.path.join(DEC, "kd", "bootimage", "SceGpuEs4.skprx.elf")
LIBGPU = os.path.join(DEC, "us", "libgpu_es4.suprx.elf")
LIBGXM = os.path.join(DEC, "us", "libgxm_es4.suprx.elf")
TABLE, INIT, NOT_SUPPORTED = 0x81011c50, 0x81001c04, 0x81000dbd    # the kernel's table, its filler, the "not supported" stub
nm = lambda k: N.get(k, f"{k[0]}_{k[1]:08X}")


def fix(m):
    """apply a module's absolute relocations at its link address (Module.relocate skips a zero delta)"""
    for symseg, typ, datseg, addend, off in m.relocations():
        v = (m.loads[symseg][2] + addend) & 0xffffffff
        buf = m.mem[m.loads[datseg][2]]
        if typ in (2, 38): struct.pack_into("<I", buf, off, v)
        elif typ in (0x2f, 0x30):
            hw1, hw2 = struct.unpack_from("<HH", buf, off)
            struct.pack_into("<HH", buf, off, *set_thumb_imm16(hw1, hw2, v & 0xffff if typ == 0x2f else v >> 16))
        elif typ in (0x2b, 0x2c):
            w, = struct.unpack_from("<I", buf, off); h = v & 0xffff if typ == 0x2b else v >> 16
            struct.pack_into("<I", buf, off, (w & 0xfff0f000) | (h >> 12) << 16 | (h & 0xfff))


# ---------------------------------------------------------------- 1. the kernel side
def kernel_table():
    """id -> handler address, from running the table's init function"""
    m = Module(KERNEL)
    u = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    u.mem_map(0x81000000, 0x20000)
    for l in m.loads:
        b = l[2]; data = bytes(m.mem[b]); u.mem_write(b, data[:min(len(data), 0x81020000 - b)])
    u.mem_map(0x70000000, 0x10000); u.mem_write(0x70000000, b"\xfe\xe7")
    tab = {}
    def w(uc, acc, addr, size, val, ud):
        if TABLE <= addr < TABLE + 0x94 * 4: tab[(addr - TABLE) // 4] = val
    u.hook_add(UC_HOOK_MEM_WRITE, w)
    u.reg_write(UC_ARM_REG_SP, 0x7000f000); u.reg_write(UC_ARM_REG_LR, 0x70000001)
    try: u.emu_start(INIT | 1, 0x70000000, count=200000)
    except UcError: pass
    return {0x1000 + k: v for k, v in tab.items()}


def kernel_scan(km, names, entry):
    """static scan of a handler(index, in, out, pp): (in reads, out writes, pp accesses, calls, size);
    registers holding in/out/pp are followed through mov/add"""
    r = km.an.function(entry & ~1, False, km.entries)
    if not r: return None
    alias = {1: "in", 2: "out", 3: "pp"}
    ins, outs, pps, calls = {}, {}, set(), []
    for a in sorted(r[0]):
        i = km.an.insn(a, False)
        if i is None: continue
        ops, mn = i.operands, i.mnemonic
        if mn in ("bl", "blx") and ops and ops[0].type == ARM_OP_IMM:
            t = ops[0].imm & 0xffffffff
            args = [alias.get(k) for k in range(4)]
            calls.append((names.get(t & ~1, f"sub_{t & ~1:x}"), args))
            for k in (0, 1, 2, 3, 12): alias.pop(k, None)
            continue
        mem = [o for o in ops if o.type == ARM_OP_MEM]
        if mem and mn.startswith(("ldr", "str", "ldm", "stm")):
            base = mem[0].mem.base - ARM_REG_R0 if ARM_REG_R0 <= mem[0].mem.base <= ARM_REG_R12 else None
            if base in alias and not mem[0].mem.index:
                sz = {"b": 1, "h": 2, "d": 8}.get(mn[3:4] if mn.startswith(("ldr", "str")) else "", 4)
                tag, off = alias[base], mem[0].mem.disp
                if tag == "pp": pps.add(off)
                elif tag.startswith("in") and mn.startswith("ldr"): ins[off] = sz
                elif tag.startswith("out") and mn.startswith("str"): outs[off] = sz
        if mn == "mov" and len(ops) == 2 and ops[0].type == ARM_OP_REG and ops[1].type == ARM_OP_REG:
            d, s = ops[0].reg - ARM_REG_R0, ops[1].reg - ARM_REG_R0
            if s in alias: alias[d] = alias[s]
            else: alias.pop(d, None)
        elif mn in ("add", "add.w", "adds") and len(ops) == 3 and ops[0].type == ARM_OP_REG and ops[1].type == ARM_OP_REG and ops[2].type == ARM_OP_IMM:
            d, s = ops[0].reg - ARM_REG_R0, ops[1].reg - ARM_REG_R0
            alias.pop(d, None)   # (a sub-struct pointer: tracked no further)
        elif ops and ops[0].type == ARM_OP_REG and not mn.startswith(("str", "stm", "cmp", "tst", "push", "b", "cb", "it", "teq", "cmn")):
            d = ops[0].reg - ARM_REG_R0
            if 0 <= d <= 12: alias.pop(d, None)
    return ins, outs, pps, calls, sum(r[0].values())


# ---------------------------------------------------------------- 2. libgpu_es4: each export's protocol
ARG, P1, P2, OUT, HEAP, STK = 0x40000000, 0x50000000, 0x60000000, 0x70000000, 0xc0000000, 0x7f000000


class GpuEmu:
    """one libgpu export run with tagged arguments: arg k is the pointer ARG + k << 16; the words behind
    any argument pointer are themselves tagged pointers (P1 region, 0x100 apart so +offsets stay
    decodable; a second dereference lands in P2); the bridge records each call and fills the output
    with tagged words (OUT | call << 20 | offset << 8) so the words the export hands back to its caller
    say which output offset they came from"""
    ORACLE = {0x1000: {4: 1, 8: 7, 12: 0, 16: 0}}    # ENUM_DEVICES: one SGX, so the connect wrapper proceeds
    # argument values an export's own checks need (reserved pointers that must be null, counts, flags)
    ARGS = {0xAAD85460: {2: 0}, 0x5564C1BE: {1: 4}, 0x071E62F0: {2: 0x8}}

    def __init__(self):
        self.m = Module(LIBGPU); fix(self.m)
        self.stub = {}
        for lib, fns, _ in self.m.imports():
            for nid, a in fns: self.stub[a & ~1] = nm((lib, nid))
        self.exports = {(l, n): a for l, n, a, v in self.m.export_table() if l and not v}

    @staticmethod
    def pat(a):
        if ARG <= a < ARG + 0x100000: return P1 + ((a - ARG) >> 2) * 0x100
        if P1 <= a < P2: return P2 + (a - P1)
        if P2 <= a < OUT: return a + 0x1000
        if OUT <= a < OUT + 0x08000000: return a + 0x08000000
        return 0

    def run(self, nid):
        entry = self.exports[("SceGpuEs4User", nid)] if ("SceGpuEs4User", nid) in self.exports else self.exports[("SceGpuEs4UserForVsh", nid)]
        u = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        for l in self.m.loads:
            b = l[2]; data = bytes(self.m.mem[b]); u.mem_map(b & ~0xfff, (len(data) + (b & 0xfff) + 0xfff) & ~0xfff); u.mem_write(b, data)
        u.mem_map(STK - 0x10000, 0x20000); u.mem_map(0, 0x1000)   # (unrelocated import-variable movw/movt pairs read 0: the stack guard)
        R = dict(bridge=[], writes=[], calls=[], heap=HEAP, stop=None, n=0)
        mapped = set()
        def ensure(a):
            pg = a & ~0xfff
            if pg in mapped or not (ARG <= a < OUT + 0x0e000000 or HEAP <= a < HEAP + 0x1000000): return
            mapped.add(pg); u.mem_map(pg, 0x1000)
            if ARG <= a < OUT + 0x0e000000:
                u.mem_write(pg, b"".join(struct.pack("<I", self.pat(x)) for x in range(pg, pg + 0x1000, 4)))
        def touch(a, n):
            if n > 0x100000: raise ValueError(f"copy of {n:#x} bytes (a tagged length)")
            for x in range(a & ~0xfff, a + max(n, 1), 0x1000): ensure(x)
        def unm(uc, acc, addr, size, val, ud):
            if not (ARG <= addr < OUT + 0x0e000000 or HEAP <= addr < HEAP + 0x1000000): return False   # (else Unicorn retries forever)
            try: ensure(addr); return True
            except Exception: return False
        u.hook_add(UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED, unm)
        def wr(uc, acc, addr, val, size, ud=None): pass
        def wrh(uc, acc, addr, size, val, ud):
            if (ARG <= addr < OUT + 0x0e000000 or HEAP <= addr < HEAP + 0x1000000): R["writes"].append((addr, val))
        u.hook_add(UC_HOOK_MEM_WRITE, wrh)
        def cnt(uc, a, s, ud):
            R["n"] += 1
            if R["n"] > 100000: uc.emu_stop()
        u.hook_add(UC_HOOK_CODE, cnt)
        lo, hi = min(self.stub), max(self.stub) + 8
        def code(uc, a, s, ud):
            if a not in self.stub: return
            name = self.stub[a]; a0, a1, a2, a3 = [u.reg_read(UC_ARM_REG_R0 + i) for i in range(4)]; ret = 0
            try:
                if name == "PVRSRV_BridgeDispatchKM":
                    id_, sz, in_, insz, out, outsz, hk = struct.unpack("<7I", u.mem_read(a1, 0x1c))
                    touch(in_, insz)
                    inb = bytes(u.mem_read(in_, insz)) if insz else b""
                    k = len(R["bridge"])
                    R["bridge"].append(dict(id=id_, insz=insz, outsz=outsz, hk=hk, inb=inb))
                    if outsz:
                        touch(out, outsz)
                        words = [0 if o == 0 else OUT + (k << 20) + o * 0x100 for o in range(0, outsz, 4)]
                        for o, v in self.ORACLE.get(id_, {}).items(): words[o // 4] = v
                        u.mem_write(out, b"".join(struct.pack("<I", w) for w in words)[:outsz])
                elif name == "PVRSRVOpen": ret = 1
                elif name == "sceClibMemcpy_safe": touch(a1, a2); touch(a0, a2); u.mem_write(a0, bytes(u.mem_read(a1, a2))); ret = a0
                elif name == "sceClibMemset": touch(a0, a2); u.mem_write(a0, bytes([a1 & 0xff]) * a2); ret = a0
                elif name in ("sceKernelDelayThread", "sceKernelWaitEventFlag"): pass
                elif name in ("sceClibMspaceMalloc", "sceClibMspaceCalloc"):
                    n = a1 * a2 if name.endswith("Calloc") else a1
                    ensure(R["heap"]); ret = R["heap"]; R["heap"] += (n + 0xff) & ~0xff
                elif name == "sceClibMspaceCreate": ret = 0xcc000000
                elif name == "sceKernelAllocMemBlock": ret = 0x4000f000
                elif name == "sceKernelGetMemBlockBase": ensure(R["heap"]); u.mem_write(a1, struct.pack("<I", R["heap"])); R["heap"] += 0x100000
                R["calls"].append((name, [a0, a1, a2, a3]))
            except Exception as e:
                R["stop"] = f"stub {name}: {e}"; uc.emu_stop()
            u.reg_write(UC_ARM_REG_R0, ret & 0xffffffff); u.reg_write(UC_ARM_REG_PC, u.reg_read(UC_ARM_REG_LR))
        u.hook_add(UC_HOOK_CODE, code, begin=lo, end=hi)
        over = self.ARGS.get(nid, {})
        for i in range(4): u.reg_write(UC_ARM_REG_R0 + i, over.get(i, ARG + (i << 16)))
        u.reg_write(UC_ARM_REG_SP, STK); u.mem_write(STK, b"".join(struct.pack("<I", ARG + (k << 16)) for k in range(4, 12)))
        u.reg_write(UC_ARM_REG_LR, 0x7fff0001); u.mem_map(0x7fff0000, 0x1000); u.mem_write(0x7fff0000, b"\xfe\xe7")
        try: u.emu_start(entry | 1, 0x7fff0000)
        except UcError as e: R["stop"] = R["stop"] or str(e)
        R["ret"] = u.reg_read(UC_ARM_REG_R0)
        return R

    @classmethod
    def dec(cls, v):
        """a tagged value -> where it came from"""
        if ARG <= v < P1: k, o = (v - ARG) >> 16, (v - ARG) & 0xffff; return f"arg{k}" + (f"+{o:#x}" if o else "")
        if P1 <= v < P2: idx, res = (v - P1) // 0x100, (v - P1) % 0x100; a = ARG + idx * 4; return f"arg{(a - ARG) >> 16}->[{(a - ARG) & 0xffff:#x}]" + (f"+{res:#x}" if res else "")
        if P2 <= v < OUT: a = v - P2 + P1; res = (a - P1) % 0x100; return cls.dec(a - res) + f"->[{res:#x}]"
        if OUT <= v < OUT + 0x08000000: k, o = (v - OUT) >> 20, (v - OUT) & 0xfffff; return f"out{k}[{o // 0x100:#x}]" + (f"+{o % 0x100:#x}" if o % 0x100 else "")
        if STK - 0x10000 <= v < STK + 0x10000: return "stack"
        if HEAP <= v < HEAP + 0x1000000: return f"heap+{v - HEAP:#x}"
        return f"{v:#x}"

    def protocol(self, nid):
        """one export's protocol as text lines: its bridge calls with the provenance of every input word,
        and the output words it stores for its caller"""
        R = self.run(nid)
        lines = []
        for k, b in enumerate(R["bridge"]):
            words = []
            for o in range(4, len(b["inb"]), 4):
                v, = struct.unpack_from("<I", b["inb"], o); d = self.dec(v)
                if d != "0x0" and d != "stack": words.append(f"+{o:#x}={d}")
            lines.append(f"id {b['id']:#x} in[{b['insz']:#x}] out[{b['outsz']:#x}]" + (": " + " ".join(words) if words else ""))
        stores = []
        for addr, val in R["writes"]:
            d, s = self.dec(addr), self.dec(val)
            if (d.startswith("arg") or d.startswith("heap")) and s.startswith("out"): stores.append(f"{d} <- {s}")
        if stores: lines.append("caller receives: " + ", ".join(dict.fromkeys(stores)))
        if R["ret"] in (3, 0x1d, 0x22) and not R["bridge"]: lines.append(f"(returns {R['ret']:#x} on the tagged arguments: a parameter check)")
        return lines


def libgpu_ids(gm):
    """libgpu export -> the bridge ids its code builds (call graph; __stack_chk_fail is no-return)"""
    for a, k in gm.stub.items():
        if nm(k) == "__stack_chk_fail": gm.an.noreturn.add(a)
    code, base = gm.m.mem[gm.m.loads[0][2]], gm.m.loads[0][2]
    from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    def ids_of(f, arm):
        r = gm.an.function(f, arm, gm.entries); out = set()
        if not r: return out
        for a in r[0]:
            for i in md.disasm(bytes(code[a - base:a - base + 4]), a, 1):
                if i.mnemonic in ("movw", "mov.w", "movs", "mov") and "#" in i.op_str:
                    v = int(i.op_str.split("#")[1], 0)
                    if 0x1000 <= v <= 0x1093: out.add(v)
        return out
    out = {}
    for key, a in gm.exports.items():
        seen, todo, got = set(), [(a & ~1, not a & 1)], set()
        while todo:
            f, arm = todo.pop()
            if f in seen or f in gm.stub: continue
            seen.add(f); got |= ids_of(f, arm); todo += gm.direct(f, arm)
        out[key] = got
    return out


# ---------------------------------------------------------------- 3. libgxm over libgpu with a Python host
GXM_BASE, GPU_BASE, TRAMP, MEMBLK, TAG, DEVMEM = 0x81000000, 0x82000000, 0x7fff0000, 0xd0000000, 0x70000000, 0xc0000000


class GxmEmu:
    def __init__(self, recipe):
        self.recipe = recipe
        self.gxm = Module(LIBGXM); fix(self.gxm)
        self.gpu = Module(LIBGPU); self.gpu.relocate(GPU_BASE)
        u = self.u = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        for m in (self.gxm, self.gpu):
            for l in m.loads:
                b = l[2] + m.delta; data = bytes(m.mem[b]); lo, hi = b & ~0xfff, (b + len(data) + 0xfff) & ~0xfff
                u.mem_map(lo, hi - lo); u.mem_write(b, data)
        u.mem_map(STK - 0x100000, 0x100000); u.mem_map(TRAMP, 0x1000); u.mem_map(0, 0x1000)
        u.mem_write(TRAMP, b"\xfe\xe7" * 8); u.mem_write(TRAMP + 0x100, b"\x70\x47"); u.mem_write(TRAMP + 0x110, b"\x70\x47")
        u.reg_write(UC_ARM_REG_C1_C0_2, u.reg_read(UC_ARM_REG_C1_C0_2) | (0xf << 20)); u.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
        self.gpu_exports = {(l, n): a for l, n, a, v in self.gpu.export_table() if l and not v}
        self.stubs = {}
        for m in (self.gxm, self.gpu):
            for lib, fns, _ in m.imports():
                for nid, a in fns: self.stubs[a & ~1] = (lib, nid)
        u.hook_add(UC_HOOK_CODE, self._stub, begin=min(self.stubs), end=max(self.stubs) + 16)
        u.hook_add(UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED, self._unmapped)
        u.hook_add(UC_HOOK_MEM_READ, self._read)
        u.hook_add(UC_HOOK_CODE, self._count)
        u.hook_add(UC_HOOK_CODE, self._callback, begin=TRAMP + 0x100, end=TRAMP + 0x200)
        self.mapped = set(); self.nmemblk = 0; self.memblk = {}; self.memblk_top = MEMBLK; self.devmem_top = DEVMEM
        self.bridge = []; self.taint = collections.defaultdict(set); self.phase = "?"; self.n = 0; self.handle = 0x1000; self.tls = TRAMP + 0x800
        self.state = {}
        self.gxm_fn = {a & ~1: nm((l, n)) for l, n, a, v in self.gxm.export_table() if l and not v}
        self.gxm_entry = {nm((l, n)): a for l, n, a, v in self.gxm.export_table() if l and not v}
        self.wrapper_calls = []   # (libgpu export nid, lr, args) -- which libgxm code calls which wrapper

    def ensure(self, a, n=4):
        for pg in range(a & ~0xfff, a + n, 0x1000):
            if pg not in self.mapped:
                self.mapped.add(pg)
                try: self.u.mem_map(pg, 0x1000)
                except UcError: pass
    def rd(self, a, n): self.ensure(a, n); return bytes(self.u.mem_read(a, n))
    def wr(self, a, b): self.ensure(a, len(b)); self.u.mem_write(a, bytes(b))
    def r32(self, a): return struct.unpack("<I", self.rd(a, 4))[0]
    def w32(self, a, v): self.wr(a, struct.pack("<I", v & 0xffffffff))
    def new_handle(self, what=None):
        self.handle += 4
        if what: self.state.setdefault("handles", {})[self.handle] = what
        return self.handle
    def alloc_user(self, size, align=0x1000):
        size = (size + 0xfff) & ~0xfff; a = self.memblk_top; self.memblk_top += size + 0x1000; self.ensure(a, size); return a
    def tag(self, k, off): return TAG | (k & 0xff) << 16 | off

    def _unmapped(self, uc, acc, addr, size, val, ud):
        if 0x40000000 <= addr < 0x7f000000 or MEMBLK <= addr < MEMBLK + 0x10000000 or DEVMEM <= addr < DEVMEM + 0x10000000:
            self.ensure(addr, size); return True
        return False
    def _count(self, uc, a, s, ud):
        self.n += 1
        if self.n > 5_000_000: uc.emu_stop()
    def _callback(self, uc, a, s, ud):   # fake user callbacks: +0x100 hostAlloc(userData, size), +0x110 hostFree
        if a == TRAMP + 0x100: self.ret(self.alloc_user(uc.reg_read(UC_ARM_REG_R1), 0x10))
        elif a == TRAMP + 0x110: self.ret(0)
    def _read(self, uc, acc, addr, size, val, ud):
        if size < 4: return
        pc = uc.reg_read(UC_ARM_REG_PC)
        if not GXM_BASE <= pc < GXM_BASE + 0x30000: return   # libgxm consuming a host-returned word (libgpu's own copies don't count)
        try: data = uc.mem_read(addr, size & ~3)
        except UcError: return
        for o in range(0, len(data), 4):
            v, = struct.unpack_from("<I", data, o)
            if TAG <= v < TAG + 0x01000000: self.taint[((v >> 16) & 0xff, v & 0xffff)].add(pc)

    def args(self):
        u = self.u; sp = u.reg_read(UC_ARM_REG_SP)
        return [u.reg_read(UC_ARM_REG_R0 + i) for i in range(4)] + [self.r32(sp + 4 * i) for i in range(8)]
    def ret(self, v):
        self.u.reg_write(UC_ARM_REG_R0, v & 0xffffffff); self.u.reg_write(UC_ARM_REG_PC, self.u.reg_read(UC_ARM_REG_LR))
    def fn_name(self, pc):
        best = max((a for a in self.gxm_fn if a <= pc), default=None)
        return self.gxm_fn[best] if best is not None else hex(pc)

    def _stub(self, uc, a, size, ud):
        if a not in self.stubs: return
        lib, nid = self.stubs[a]
        if (lib, nid) in self.gpu_exports:
            self.wrapper_calls.append((nid, uc.reg_read(UC_ARM_REG_LR), self.args()[:8], self.phase))
            uc.reg_write(UC_ARM_REG_PC, self.gpu_exports[(lib, nid)] | 1); return
        try: r = self.kernel(nm((lib, nid)), self.args())
        except Exception as e:
            self.state.setdefault("errors", []).append(f"{nm((lib, nid))}: {e}"); uc.emu_stop(); r = 0
        self.ret(0 if r is None else r)

    def kernel(self, name, a):
        if name == "PVRSRV_BridgeDispatchKM": return self.bridge_call(a[1])
        if name == "PVRSRVOpen": return 1
        if name in ("sceKernelWaitEventFlag", "_sceKernelWaitEventFlag"):
            if a[3]: self.w32(a[3], a[1])
            return 0
        if name in ("sceKernelCreateEventFlag", "_sceKernelCreateEventFlag"): return 0x20001
        if name in ("sceKernelCreateThread", "sceKernelCreateThreadForUser"): return 0x30001
        if name == "sceKernelGetThreadId": return 0x30000
        if name in ("sceKernelIsGameBudget", "sceKernelGetStdout"): return 1
        if name == "sceKernelGetTLSAddr": self.tls += 4; return self.tls
        if name == "sceKernelGetProcessTitleId": self.wr(a[0], b"PCSA00014\0"); return 0
        if name in ("sceClibMemcpy_safe", "sceClibMemmove", "sceClibMemcpy"): self.wr(a[0], self.rd(a[1], a[2])); return a[0]
        if name == "sceClibMemset": self.wr(a[0], bytes([a[1] & 0xff]) * a[2]); return a[0]
        if name == "sceClibMemcmp": x, y = self.rd(a[0], a[2]), self.rd(a[1], a[2]); return 0 if x == y else (1 if x > y else -1)
        if name == "sceClibStrcmp":
            def s(p):
                out = b""
                while self.rd(p, 1) != b"\0": out += self.rd(p, 1); p += 1
                return out
            x, y = s(a[0]), s(a[1]); return 0 if x == y else (1 if x > y else -1)
        if name == "sceClibMspaceCreate": return 0xcc000000
        if name in ("sceClibMspaceMalloc", "sceClibMspaceCalloc"): return self.alloc_user(a[1] * a[2] if name.endswith("Calloc") else a[1], 0x100)
        if name == "sceKernelAllocMemBlock":
            self.nmemblk += 1; uid = 0x40010000 + self.nmemblk * 0x100; base = self.alloc_user(a[2])
            self.memblk[uid] = (base, a[2], a[1], self.rd(a[0], 32).split(b"\0")[0].decode("ascii", "replace")); return uid
        if name == "sceKernelGetMemBlockBase":
            if a[0] in self.memblk: self.w32(a[1], self.memblk[a[0]][0]); return 0
            return 0x80020006
        if name == "sceKernelFindMemBlockByAddr":
            for uid, (base, size, t, _) in self.memblk.items():
                if base <= a[0] < base + size: return uid
            return 0x80020006
        if name == "sceKernelGetMemBlockInfoByAddr":
            for uid, (base, size, t, _) in self.memblk.items():
                if base <= a[0] < base + size: self.wr(a[1], struct.pack("<6I", 0x18, base, size, 0x8000, 0xf, t)); return 0
            return 0x80020006
        return 0   # PVRSRVRelease, sceGpuSignalWait, thread/eventflag/mutex/free/delay/printf: nothing to do

    def bridge_call(self, pkg):
        id_, sz, in_, insz, out, outsz, hk = struct.unpack("<7I", self.rd(pkg, 0x1c))
        inb = self.rd(in_, insz) if insz else b""
        k = len(self.bridge)
        call = dict(id=id_, inb=inb, insz=insz, outsz=outsz, k=k, phase=self.phase, user=None)
        self.bridge.append(call)
        words = [0] + [TAG | (k & 0xff) << 16 | o for o in range(4, outsz, 4)] if outsz else []
        r = self.recipe(self, call, words)
        if outsz: self.wr(out, b"".join(struct.pack("<I", w & 0xffffffff) for w in words)[:outsz])
        call["out"] = list(words)
        return 0 if r is None else r

    def call(self, name, *args, phase=None):
        self.phase = phase or name
        u = self.u; sp = STK - 0x1000
        for i, v in enumerate(args[4:]): self.w32(sp + 4 * i, v)
        for i in range(4): u.reg_write(UC_ARM_REG_R0 + i, args[i] if i < len(args) else 0)
        u.reg_write(UC_ARM_REG_SP, sp); u.reg_write(UC_ARM_REG_LR, TRAMP | 1)
        self.n = 0
        try: u.emu_start(self.gxm_entry[name], TRAMP)   # (libgxm's exports are ARM code: even addresses)
        except UcError as e: self.state.setdefault("errors", []).append(f"{name}: {e} at {u.reg_read(UC_ARM_REG_PC):#x}")
        return u.reg_read(UC_ARM_REG_R0)

    def describe(self, v):
        s = self.state
        for k, label in (("hk", "hKernelServices"), ("hDevCookie", "hDevCookie"), ("hDevMemContext", "hDevMemContext")):
            if s.get(k) == v: return f" ({label})"
        if v in s.get("handles", {}): return f" ({s['handles'][v]})"
        for uid, (base, size, t, name) in self.memblk.items():
            if v == uid: return f" (memblock uid: {name}, {size:#x} bytes at {base:#x})"
            if base <= v < base + size: return f" (memblock {name}+{v - base:#x})"
        for h, (base, size, attr, heap) in s.get("devmem", {}).items():
            if base <= v < base + size: return f" (devmem {h:#x}+{v - base:#x})"
        if TAG <= v < TAG + 0x1000000: return f" (tagged: call {(v >> 16) & 0xff} out+{v & 0xffff:#x})"
        return ""


# the host recipe: what to return for each id. words[0] is the error; words[1:] arrive tagged (whatever
# the recipe leaves alone is reported if libgxm consumes it). Proven by the scenario below.
# PVRSRV_HEAP_INFO {id, hDevMemHeap, devVAddrBase, size, attribs, 1} for the heaps libgxm requires.
HEAPS = {0: (0x00000000, 0xf0000000, "GENERAL"), 4: (0x20000000, 0x08000000, "FRAGMENT_USSE"), 5: (0x10000000, 0x08000000, "VERTEX_USSE"),
         8: (0x30000000, 0x01000000, "SYNCINFO"), 0xe: (0x31000000, 0x01000000, "heap 0xe"), 0x12: (0x32000000, 0x01000000, "heap 0x12"),
         0x13: (0x33000000, 0x01000000, "heap 0x13")}


def recipe(e, c, words):
    W = lambda o: struct.unpack_from("<I", c["inb"], o)[0] if o + 4 <= len(c["inb"]) else None
    st, id_ = e.state, c["id"]
    if "heaps" not in st:
        st["heaps"], st["heap_by_handle"], st["group"] = [], {}, 2
        for hid, (hbase, hsize, name) in HEAPS.items():
            h = e.new_handle(f"hDevMemHeap {name}")
            st["heaps"].append([hid, h, hbase, hsize, 0, 1])
            st["heap_by_handle"][h] = dict(id=hid, base=hbase, top=hbase + (0x01000000 if hid == 4 else 0))   # fragment USSE: bank 4
    if id_ == 0x100c: words[1] = st["hk"] = e.new_handle()
    elif id_ == 0x1000: words[1:6] = [1, 7, 0, 0, 0]
    elif id_ == 0x1001: words[1] = st["hDevCookie"] = e.new_handle()
    elif id_ == 0x106c:
        p = W(8); req = e.r32(p); buf = bytearray(e.rd(p, 0x4c))
        for o in range(4, 0x4c, 4): struct.pack_into("<I", buf, o, e.tag(c["k"], 0x100 + o))
        if req == 2: struct.pack_into("<III", buf, 0x10, 0x10711, 0x13a5, 0); struct.pack_into("<I", buf, 0x20, 0x30322518)
        c["user"] = dict(req=req, ptr=p); e.wr(p, buf)
    elif id_ == 0x1003:
        words[1] = st["hDevMemContext"] = e.new_handle(); words[2] = len(st["heaps"])
        for i, h in enumerate(st["heaps"]): words[3 + i * 6:9 + i * 6] = h
    elif id_ == 0x1006:
        size = W(0x10); base = e.devmem_top; e.devmem_top += (size + 0xfff) & ~0xfff; e.ensure(base, size)
        words[1] = e.new_handle("hKernelMemInfo"); words[2] = words[3] = base; words[4] = W(0xc); words[5] = size; words[6] = 0; words[7] = size; words[8] = words[1]   # +0x1c (meminfo[0x10]) is what libgxm takes as the region size
        st.setdefault("devmem", {})[words[1]] = (base, size, W(0xc), W(8))
    elif id_ == 0x101d:
        base, size, heap = W(0x10), W(0x14), W(0xc)
        if heap: hb = st["heap_by_handle"][heap]; words[1] = hb["top"]; hb["top"] += (size + 0xfff) & ~0xfff
        else: words[1] = base
    elif id_ == 0x1058:   # registration group id: echo a nonzero value, hand out a fresh id (>= 2) for 0
        if W(8): words[1] = W(8)
        else: words[1] = st["group"]; st["group"] += 1
    elif id_ == 0x1086:
        words[1] = e.new_handle("hRenderTarget")
        st.setdefault("rt", {})[words[1]] = dict(uid=W(0x3c), owned=W(0x40), flags=W(8), width=W(0x1c), height=W(0x20), msaa=W(0x24))
    elif id_ == 0x108b:   # begin-scene info: the target's width, height, msaa x:16|y:16, (flags >> 17) & 1
        rt = st.get("rt", {}).get(W(4))
        if rt: words[1:5] = [rt["width"], rt["height"], rt["msaa"], (rt["flags"] >> 17) & 1]
    elif id_ in (0x1084, 0x108d, 0x104d, 0x1055): words[1] = e.new_handle({0x1084: "hRenderContext", 0x108d: "hTransferContext", 0x104d: "hDisplay", 0x1055: "hSyncObject"}[id_])
    elif id_ == 0x108c:   # kick: the GPU finishes at once -- perform every completion write of the descriptor
        done = []
        for base_off, count_off in ((0x140, 0x104), (0x2c0, 0x108)):
            for i in range(W(count_off)):
                ctl, val, h = W(base_off + 12 * i), W(base_off + 12 * i + 4), W(base_off + 12 * i + 8)
                mem = st.get("devmem", {}).get(h)
                if mem and ctl & 0x400000:
                    off = (ctl & 0xfffff) - 0xf; e.w32(mem[0] + off, val); done.append((mem[0] + off, val, ctl))
        c["writes"] = done
    elif id_ == 0x105e: words[0x40] = 0   # the error word is at +0x100
    elif id_ == 0x102a: words[1] = W(4)
    elif id_ == 0x1089:
        rt = st.get("rt", {}).get(W(4))
        if rt: words[1], words[2] = rt["uid"], rt["owned"]
    return 0


def scenario():
    """libgxm's own code over libgpu with the recipe: the calls a game makes up to its first draw, and
    what each returned"""
    e = GxmEmu(recipe)
    results = []
    cb = TRAMP + 0x100
    p = e.alloc_user(0x1000); e.wr(p, struct.pack("<5I", 0, 2, cb | 1, 8, 16 << 20))
    results.append(("sceGxmInitialize", e.call("sceGxmInitialize", p)))
    mem = e.alloc_user(0x100000); e.memblk[0x40020000] = (mem, 0x100000, 0x0c20d060, "game memory")
    results.append(("sceGxmMapMemory", e.call("sceGxmMapMemory", mem, 0x100000, 3)))
    usse = e.alloc_user(0x10000); e.memblk[0x40020100] = (usse, 0x10000, 0x0c20d060, "game USSE memory"); off = e.alloc_user(0x1000)
    r = e.call("sceGxmMapVertexUsseMemory", usse, 0x10000, off); results.append((f"sceGxmMapVertexUsseMemory (offset {e.r32(off):#x})", r))
    r = e.call("sceGxmMapFragmentUsseMemory", usse + 0x8000, 0x8000, off); results.append((f"sceGxmMapFragmentUsseMemory (offset {e.r32(off):#x})", r))
    sizes = [0x8000, 0x80000, 0x100000, 0x80000, 0x4000]; bufs = [e.alloc_user(s) for s in sizes]
    for i, (b, s) in enumerate(zip(bufs, sizes)):
        e.memblk[0x40030000 + i * 0x100] = (b, s, 0x0c20d060, ["host mem", "vdm ring", "vertex ring", "fragment ring", "fragment USSE ring"][i])
        if i < 4: e.call("sceGxmMapMemory", b, s, 3, phase="sceGxmCreateContext (the game maps the rings)")
    e.call("sceGxmMapFragmentUsseMemory", bufs[4], sizes[4], off, phase="sceGxmCreateContext (the game maps the rings)")
    cp = e.alloc_user(0x1000); e.wr(cp, struct.pack("<11I", bufs[0], sizes[0], bufs[1], sizes[1], bufs[2], sizes[2], bufs[3], sizes[3], bufs[4], sizes[4], e.r32(off)))
    ctxp = e.alloc_user(0x1000); results.append(("sceGxmCreateContext", e.call("sceGxmCreateContext", cp, ctxp)))
    rp = e.alloc_user(0x1000); rtp = e.alloc_user(0x1000); e.wr(rp, struct.pack("<IHHHHIi", 0, 960, 544, 1, 0, 0, -1))
    results.append(("sceGxmCreateRenderTarget (960x544, driverMemBlock = -1)", e.call("sceGxmCreateRenderTarget", rp, rtp)))
    rt = e.r32(rtp); uidp = e.alloc_user(0x1000)
    results.append((f"sceGxmRenderTargetGetDriverMemBlock (-> {e.r32(uidp):#x} after the call)" if False else "sceGxmRenderTargetGetDriverMemBlock", e.call("sceGxmRenderTargetGetDriverMemBlock", rt, uidp)))
    sp = e.alloc_user(0x1000); results.append(("sceGxmSyncObjectCreate", e.call("sceGxmSyncObjectCreate", sp)))
    sp2 = e.alloc_user(0x1000); e.call("sceGxmSyncObjectCreate", sp2)
    cbuf = e.alloc_user(960 * 544 * 4); e.memblk[0x40050000] = (cbuf, 960 * 544 * 4, 0x0c20d060, "colour buffer"); e.call("sceGxmMapMemory", cbuf, 960 * 544 * 4, 3, phase="sceGxmBeginScene (the game maps its surfaces)")
    dbuf = e.alloc_user(960 * 544 * 4); e.memblk[0x40050100] = (dbuf, 960 * 544 * 4, 0x0c20d060, "depth buffer"); e.call("sceGxmMapMemory", dbuf, 960 * 544 * 4, 3, phase="sceGxmBeginScene (the game maps its surfaces)")
    cs = e.alloc_user(0x1000); ds = e.alloc_user(0x1000)
    e.call("sceGxmColorSurfaceInit", cs, 0, 0, 0, 0, 960, 544, 960, cbuf)              # U8U8U8U8_ABGR, linear
    e.call("sceGxmDepthStencilSurfaceInit", ds, 0x01266000, 0x00011000, 960, dbuf, 0)   # S8D24, tiled
    results.append(("sceGxmBeginScene (fragment sync, colour + depth surfaces)", e.call("sceGxmBeginScene", e.r32(ctxp), 0, rt, 0, 0, e.r32(sp), cs, ds)))
    region = e.call("sceGxmGetNotificationRegion", phase="sceGxmEndScene")
    vn = e.alloc_user(0x1000); fn = e.alloc_user(0x1000); e.wr(vn, struct.pack("<II", region + 0x10, 0x1111)); e.wr(fn, struct.pack("<II", region + 0x20, 0x2222))
    results.append(("sceGxmEndScene (vertex notification region+0x10 <- 0x1111, fragment region+0x20 <- 0x2222)", e.call("sceGxmEndScene", e.r32(ctxp), vn, fn)))
    e.state["notification region"] = region
    for a, b in ((0x3fc, 0x400), (0x420, 0x424), (0x444, 0x448), (0x47c, 0x480)):
        e.state.setdefault("finish waits", []).append((e.r32(e.r32(ctxp) + a), e.r32(e.r32(ctxp) + b)))
    cbd = e.alloc_user(0x100)
    results.append(("sceGxmDisplayQueueAddEntry(sync2, sync1, data)", e.call("sceGxmDisplayQueueAddEntry", e.r32(sp2), e.r32(sp), cbd)))
    results.append(("sceGxmNotificationWait (fragment)", e.call("sceGxmNotificationWait", fn)))
    results.append(("sceGxmFinish", e.call("sceGxmFinish", e.r32(ctxp))))
    pbuf = e.alloc_user(0x40000); e.memblk[0x40040000] = (pbuf, 0x40000, 0x0c20d060, "patcher buffer")
    e.call("sceGxmMapMemory", pbuf, 0x40000, 3, phase="sceGxmShaderPatcherCreate (the game maps the patcher memory)")
    pv = e.alloc_user(0x10000); e.memblk[0x40040100] = (pv, 0x10000, 0x0c20d060, "patcher vertex USSE"); voff = e.alloc_user(0x1000)
    e.call("sceGxmMapVertexUsseMemory", pv, 0x10000, voff, phase="sceGxmShaderPatcherCreate (the game maps the patcher memory)")
    pf = e.alloc_user(0x10000); e.memblk[0x40040200] = (pf, 0x10000, 0x0c20d060, "patcher fragment USSE"); foff = e.alloc_user(0x1000)
    e.call("sceGxmMapFragmentUsseMemory", pf, 0x10000, foff, phase="sceGxmShaderPatcherCreate (the game maps the patcher memory)")
    pp = e.alloc_user(0x1000); php = e.alloc_user(0x1000)
    e.wr(pp, struct.pack("<17I", 0, cb | 1, (cb + 0x10) | 1, 0, 0, pbuf, 0x40000, 0, 0, pv, 0x10000, e.r32(voff), 0, 0, pf, 0x10000, e.r32(foff)))
    results.append(("sceGxmShaderPatcherCreate", e.call("sceGxmShaderPatcherCreate", pp, php)))
    results.append(("sceGxmSyncObjectDestroy", e.call("sceGxmSyncObjectDestroy", e.r32(sp))))
    e.call("sceGxmSyncObjectDestroy", e.r32(sp2))
    results.append(("sceGxmDestroyRenderTarget", e.call("sceGxmDestroyRenderTarget", rt)))
    results.append(("sceGxmDestroyContext", e.call("sceGxmDestroyContext", e.r32(ctxp))))
    results.append(("sceGxmUnmapMemory", e.call("sceGxmUnmapMemory", mem)))
    results.append(("sceGxmTerminate", e.call("sceGxmTerminate")))
    return e, results


# ---------------------------------------------------------------- 4. what the code means (curated from reading the handlers)
# id: (name, what the kernel does with its input / what it returns, the host recipe)
KNOW = {
    0x1000: ("ENUM_DEVICES", "in none; out {err, count, ids[5] = {type (7 = SGX), class, index, 0, 0}} from PVRSRVEnumerateDevicesKM. libgpu accepts count 0 or 1, copies ids to conn+8..0x1c; conn[8] == 7 selects the SGX connect callback (0x81000219: a 0x106c request 2).", "count 1, {7, 0, 0, 0, 0}"),
    0x1001: ("ACQUIRE_DEVICEINFO", "in {_, device index (conn[0x10]), type 0}; PVRSRVAcquireDeviceDataKM, then out[4] = handle (type 2) of the device node.", "a cookie"),
    0x1003: ("CREATE_DEVMEMCONTEXT", "in {_, hDevCookie}; out 0x30c = {err, hDevMemContext (type 3), heapCount, heaps[32] x 0x18}: PVRSRV_HEAP_INFO {ui32HeapID, hDevMemHeap (type 4), sDevVAddrBase, ui32HeapByteSize, ui32Attribs, 1} -- 0x81003260 copies the device's heap records (stride 0x28: id@0, base@0xc, size@0x10, attribs@0x14, type@0x18 = 0 per-context / 3 shared) and the handler then puts a handle for each heap at +4. libgxm (0x8100c800) switches on id & 0xffffff: keeps +4 (handle) of ids 0, 4, 5, 8, 0xe, 0x12, 0x13 and +8 (base) of 4, 5, 0x12, 0x13; all seven handles must be nonzero (else SCE_GXM_ERROR_OUT_OF_MEMORY). It then passes the heap handle as ALLOC_DEVICEMEM in[8]. The records are built at boot by the init server (SceGpuEs4Init -> DevInitSGXPart2KM), so the real device-virtual bases are not in SceGpuEs4's data; libgxm uses the bases only to turn a mapped device address into a USSE offset: vertex (heap 5) offset = (delta >> 23) << 24 | delta & 0x7fffff, fragment (heap 4) offset = (delta >> 22) << 24 | delta & 0x3fffff, delta = address - base; sceGxmCreateContext requires the fragment ring offset's top byte in {4..7, 9}, so fragment USSE mappings must sit at fragmentBase + 0x01000000 (bank 4) and up.", "7 heaps (see HEAPS): ids 0, 4, 5, 8, 0xe, 0x12, 0x13, distinct handle cookies at +4, the bases at +8, +0x14 = 1; count 7"),
    0x1004: ("DESTROY_DEVMEMCONTEXT", "in {_, hDevCookie, hDevMemContext}.", "no-op"),
    0x1005: ("GET_DEVMEM_HEAPINFO", "in {_, hDevCookie, hDevMemContext}; out {err, count, heaps...}. Not reached by libgpu's exports.", "unreached"),
    0x1006: ("ALLOC_DEVICEMEM", "in 0x1c {_, hDevCookie, hDevMemHeap (type 4), attribs, size, align, group id}; gates: in[0x18] > 1 and attribs bit 22 (0x400000) set, else error 3; attribs bit 3 must be set or the kernel panics; bit 21 is cleared. out 0x24 = {err, hKernelMemInfo (type 5), +8 CPU address, +0xc device address, +0x10 flags, +0x14 size, +0x18 0, +0x1c meminfo[0x10] (sync/hMemInfo), +0x20 hKernelMemInfo again}. libgxm's calls: heap 8 attribs 0x40000f sizes 0x400 and 0x820 align 4; heap 0 attribs 0x40000b size 0x10 align 1; heap 0x13 attribs 0x400009 size 0x810 align 0x10; the group id is what 0x1058 returned for the parameter buffer. libgxm consumes +8, +0xc and +0x10.", "allocate size bytes of guest memory: +8 = +0xc = its address (device address = CPU address), +0x10 = attribs, +0x14 = size, cookies at +4 and +0x20"),
    0x1007: ("FREE_DEVICEMEM", "in {_, hDevCookie, hKernelMemInfo}; PVRSRVFreeDeviceMemKM, release handle.", "no-op (free the memory if tracked)"),
    0x100c: ("CONNECT_SERVICES", "out {0, hKernelServices = pp+0xc}; libgpu puts it in every package's last word.", "a cookie"),
    0x100d: ("DISCONNECT_SERVICES", "out {0}.", "no-op"),
    0x101d: ("MAP_MEMORY", "in 0x20 {_, hDevCookie, hDevMemContext, hDevMemHeap or 0, base, size, max size (0x800000 for USSE, 0 otherwise), attribs}; attribs bit0 R, bit1 W, bit8 = plain (libgxm: 0x103 for sceGxmMapMemory; 1 with a heap for USSE); 0x81003518: base/size page aligned, size <= max if nonzero, the block must be a user memblock (memtype 0x8000 -> MMU attribute 0x7000014, 0xd000 -> 0x700000e), ksceGpuMmuMapMemory; out[4] = device address: the CPU address for plain mappings, a page-granular range carved from the heap otherwise. 0x18 = busy, retry.", "out[4] = base for plain mappings; for a heap, heapAllocBase + running offset (fragment USSE from bank 4)"),
    0x101e: ("UNMAP_MEMORY", "in {_, device address (not a handle), hDevMemHeap or 0, x}; 0x810037a8 unmaps; may return 0x18 (retry).", "no-op"),
    0x101f: ("VSH_SYNC_EXPORT", "in {_, process id?, hSyncInfo}: a global handle for the sync info (Vsh).", "unreached by games"),
    0x1020: ("VSH_SYNC_OPEN", "in {_, global handle}; out {err, per-process handle}.", "unreached by games"),
    0x1021: ("VSH_SYNC_CLOSE", "in {_, global handle, x}.", "unreached by games"),
    0x1022: ("VSH_SYNC_DESTROY", "in {_, global handle}.", "unreached by games"),
    0x102a: ("GET_MISC_INFO", "in/out 0x9c {_, PVRSRV_MISC_INFO 0x98}; echoed, then PVRSRVGetMiscInfoKM honours state bits 0x98 only (0x10: DDK version {1, 7, 0x11, 0x13a5} at +0x88 and a memory-stats string into the user buffer at +8/+0xc). libgxm requests 8 and reads nothing back.", "echo the struct, [4] = 0"),
    0x104d: ("DISPLAY_CREATE", "in none; a 0x44-byte display object, one of 32 slots in pp+0x208; out {err, handle (type 0x16)}. sceGxmInitialize creates two.", "a cookie"),
    0x104e: ("DISPLAY_DESTROY", "in {_, handle}.", "no-op"),
    0x104f: ("DISPLAY_FLIP", "sceGxmDisplayQueueAddEntry (libgpu 0D27286F): in 0x20 {_, hDisplay, sync handles[count] @8, flags @0x18 (libgxm: 2), count <= 4 @0x1c (libgxm: {oldSync, newSync}, 1 if equal)}; the kernel records each sync and snapshots its counters (data[8] -> object+0x1c+4i, data[0] -> +0x2c+4i), flags&1 -> readOpsPending++, flags&2 -> writeOpsPending++, then queues the object for the display driver (flips on vsync). Duplicate syncs -> 0x18 (retry).", "remember the flip on the object; out {0}"),
    0x1050: ("DISPLAY_RELEASE", "libgxm's display thread after the game's callback (libgpu 116449D2): in {_, hDisplay}; every slot set; per sync (0x81000dee) the complete counters must equal the snapshots, then flags&1 -> readOpsComplete++, flags&2 -> writeOpsComplete++; 0x810049d8 reschedules.", "out {0}"),
    0x1055: ("SYNC_OBJECT_CREATE", "sceGxmSyncObjectCreate: in {_, hDevCookie}; allocates a sync info in the device's sync heap (max 0x30 per process, error 0xb9); out {err, handle (type 6)} -- nothing else. **The SceGxmSyncObject* libgxm returns to the game is this handle value itself** (0x104f receives it unchanged); its counters live in the kernel and nothing in user space reads them.", "a cookie"),
    0x1056: ("SYNC_OBJECT_DESTROY", "in {_, handle}.", "no-op"),
    0x1057: ("DISPLAY_FLIP_STATUS", "polled by libgxm's display thread (libgpu 5C3A6DFE(hDisplay, timeout*)): in {_, hDisplay}; out 0xc {status, pp[0x20c] = the per-process GPU event flag id, object+0x3c = its bit}. status (0x81001bb4): 0x804c000c if no flip is recorded, 0x804c0006 while a sync has not reached its snapshot, 0x804c0007, 0 when the flip's GPU work is complete; 0x804c0005 if the object is not on the display list. libgpu: a nonzero dispatcher result -> returns 0x804c0000; status 0x804c0006 -> sceKernelWaitEventFlag(out[4], out[8], 5, NULL, timeout*) and returns that; else returns status. The thread treats >= 0 as complete (calls the game's display callback, then 0x1050) and on < 0 printfs and polls again at once.", "dispatcher result 0, out {0, 0, 0} (complete) -- never leave it unimplemented"),
    0x1058: ("REGISTER_MEMBLOCK", "in {_, memblock UID, group id in/out, flags}; out {err, group id}. 0x81007994: PUID->GUID, memblock type must be a user type with memtype 0x8000, a kernel alias block is allocated (flags 1: the user mapping is first unmapped -- the parameter buffer is GPU-only), a node {uid, guid, kernel uid, flags, group @0x10, mapping info @0x24} is listed in pp+0x24; the in/out word: nonzero is kept as the node's group id, 0 gets a fresh id from a counter (reset to 2 on overflow) which is written back. libgxm registers the SceGxmDriver block (1 MB, type 0x0c208060) with 0, then the parameter buffer (16 MB, 0x09408060, flags 1) with the id it got, keeps it in dev[0x194] and passes it as ALLOC_DEVICEMEM's in[0x18]; sceGxmCreateRenderTarget registers the driver memblock with its CPU base as the id (flags 1). in[8] == 1 is rejected (error 3).", "out[4] = in[8] if nonzero, else the next id of a counter starting at 2"),
    0x1059: ("UNREGISTER_MEMBLOCK", "in {_, UID, 0, flags}; 0x81003a2c removes the node (frees the alias block, remaps the user block if flags 1).", "no-op"),
    0x105b: ("VSH_CHECK_MEMORY", "in {_, hDevMemContext, address, size, kind}; checks a mapping (0x8100867c).", "unreached by games"),
    0x105c: ("MEMBLOCK_INFO", "in {_, UID}; out {err, +4, +8, +0xc} via pp+0x20 (0x81003ca4). Not reached by libgxm.", "unreached"),
    0x105e: ("SGX_GET_CLIENT_INFO", "in {_, hDevCookie}; out 0x104: [0] = process id, [0xa0..0x100) = 0x60 bytes of devinfo+0x800, [0x100] = error (not at +0!). libgpu copies [4..0xa0) and [0xb0..0xc8) into libgxm's 0xb4-byte client struct at dev+0xac; libgxm reads none of it.", "zeros, [0x100] = 0"),
    0x105f: ("SGX_RELEASE_CLIENT_INFO", "in 0x108 {_, hDevCookie, 0, the 0x98-byte client struct}; decrements devinfo[0x54][0x90].", "no-op"),
    0x1060: ("x60", "in {_, hDevCookie}; 0x81006a1c, handle type 5 out[4]. Not reached by libgpu's exports.", "unreached"),
    0x1067: ("x67", "in {_, hDevCookie, hSyncInfo, x}; 0x81006ca8. Unreached.", "unreached"),
    0x106c: ("SGX_MISC_INFO", "in {_, hDevCookie, user pointer}: a 0x4c-byte user struct copied in and out, [0] = request 0..0xd (0 GPU clock, 1 SoC revision + core info, 2 driver version, 3 register pokes, 9 nop, 10/11 timing, 12 counters, 13 dev-tool display). Request 2 (the only one libgxm issues) fills [0x10] = 0x10711, [0x14] = 0x13a5, [0x20] = 0x30322518, which libgpu's SGX connect callback checks exactly.", "request 2: those three words"),
    0x1073: ("x73", "in {_, hDevCookie, x}; 0x81006d24. Unreached.", "unreached"),
    0x1075: ("x75", "in {_, hDevCookie, hKernelMemInfo, ..., sync handles}; 0x8100668c. Unreached.", "unreached"),
    0x1083: ("SET_RENDER_CONTEXT_PRIORITY", "in {_, hRenderContext, priority 0..2}; out {err, old}. Unreached.", "unreached"),
    0x1084: ("REGISTER_RENDER_CONTEXT", "in 0x2c {_, hDevCookie, desc 0x24 @8}: desc {6, hDevCookie, hDevMemContext (looked up), 0xf80000, 0xf80000, 0, 0x80, 0, 1} (libgxm's values); 0x8100b868 builds the HW render context; out {err, handle (type 0x18)}.", "a cookie"),
    0x1085: ("UNREGISTER_RENDER_CONTEXT", "in {_, handle, flag}.", "no-op"),
    0x1086: ("CREATE_RENDER_TARGET", "in 0x4c {_, hDevCookie, desc 0x44 @8}; desc {flags (2 | 0x10000 if f&1 | 0x60000 if f&2 | 0x80000 if f&4 | 0x100000 if f&8), scenes*2, (2|3)*scenes, hRenderContext (looked up), hDevMemContext, width <= 0x1000, height <= 0x1000, msaa x:16 | y:16 (0x10001/0x20001/0x20002), 0, 0, 1.0f, 0, 1:16 | (f>>8&0xf):8 | (f>>12&0xf):8, driver memblock UID, 1 if libgxm allocated it, its group id/base (from 0x1058), multisampleLocations}; pp counts targets by kind (bit 19 -> max 3, bit 20 -> max 1, else 0x2f; error 0xb8); out {err, handle (type 0x19)}.", "a cookie; remember desc+0x34 (UID) and +0x38 (owned) for 0x1089"),
    0x1087: ("DESTROY_RENDER_TARGET", "in {_, hDevCookie, hRenderContext, hRenderTarget}.", "no-op"),
    0x1088: ("RENDER_TARGET_INFO", "in {_, hRT}; out 0x50 of the kernel RT object (incl. a kernel pointer at +0x28). Not reached by libgxm.", "unreached"),
    0x1089: ("RT_GET_MEMBLOCK", "in {_, hRT}; out {err, desc UID (rt+8), owned flag (rt+0xc)}; sceGxmRenderTargetGetDriverMemBlock returns owned ? -1 : UID; sceGxmDestroyRenderTarget frees the block when owned.", "{UID, owned} recorded at 0x1086"),
    0x108a: ("x8a", "always out[0] = 0x13 (not supported); libgpu 9E8E2957, unreached by libgxm.", "unreached"),
    0x108c: ("SGX_DOKICK", "sceGxmEndScene / Draw* / MidSceneFlush (libgpu F0CCE9DB(conn, cmd 0x2f4, out6*)): in 0x2fc = {_, hDevCookie, the 0x2f4-byte kick descriptor @8}. Descriptor (offsets in the bridge input): +8..+0x104 SGX register state (e.g. +0x20/+0x24 width-1/height-1, +0x4c/+0x54 depth-stencil address, +0x74 fragment ring base, +0xe4 VDM ring base, +0xfc/+0x100 width/height) -- the host does not decode it; **+0x104 count A (<= 0x20) of 12-byte records at +0x140 completed with the vertex (TA) phase; +0x108 count B (<= 4) of records at +0x2c0 completed with the fragment (3D) phase; +0x110 user pointer to a word holding the fragment sync object handle (0 if none)**; +0x2f0 hTransferContext, +0x2f4 hRenderContext, +0x2f8 hRenderTarget (all looked up and replaced by kernel pointers). A record is `{ctl, value, hKernelMemInfo}` with `ctl = 0x400000 | (byteOffset + 0xf)`: when the phase completes the GPU writes `value` to `meminfo + byteOffset`. libgxm's records: A = {status+0 <- vertex progress, status+4 <- ..., vertexNotification}, B = {status+8, status+0xc, fragmentNotification}, where 'status' is its first ALLOC_DEVICEMEM (0x400 bytes, heap 8) and the notification region is its second (0x820 bytes, heap 8; sceGxmGetNotificationRegion = base+0x20; NotificationWait requires the address inside [base, base+size) with size = ALLOC out+0x1c). sceGxmFinish waits `*ctx[0x3fc] == ctx[0x400]`, `*ctx[0x420] == ctx[0x424]`, `*ctx[0x444] == ctx[0x448]`, `*ctx[0x47c] == ctx[0x480]` -- exactly the four internal records' addresses/values; ring-buffer reuse waits on the same words. The kernel gates on pp+0x40 (0x18 = busy) and submits via 0x8100a538; out 0x1c = {err, 6 status words} which libgpu copies to its caller and libgxm does not read. Verified by emulation: performing the record writes makes NotificationWait and Finish return 0.", "for every A then B record: write value to meminfo + ((ctl & 0xfffff) - 0xf); bump the signal counter; out {0, 0...}"),
    0x108d: ("REGISTER_TRANSFER_CONTEXT", "in 0x14 {_, hDevCookie, 0, hDevMemContext, 0}; out {err, handle (type 0x1a)}.", "a cookie"),
    0x108e: ("UNREGISTER_TRANSFER_CONTEXT", "in {_, handle, flag}.", "no-op"),
    0x108f: ("SUBMIT_TRANSFER", "sceGxmTransfer{Copy,Downscale,Fill} (libgpu 5564C1BE(cmd*, wordCount <= 0x1a, conn, hTransferContext, hSync or 0, x, y, notification record* or NULL)): in 0x94 = {_, hDevCookie, +8 the 2D blit command words (wordCount of them, <= 0x68 bytes), +0x70 wordCount, +0x74 hTransferContext (type 0x1a), +0x78 hSync (type 6) or 0, +0x7c, +0x80, +0x84 {ctl, value, hKernelMemInfo} = the notification record (same encoding as the kick's), +0x90 = 1 if a notification was given}. Kernel: pp[0x214] == 0x1000000 (else 0x13), pp+0x40 busy gate, 0x8100ce90 queues the words in the transfer CCB with the sync and notification. The surfaces are only in the command words (and in the sceGxmTransfer* arguments the host sees at the GXM boundary).", "perform the blit from the GXM-level arguments; write value to meminfo + ((ctl & 0xfffff) - 0xf) when in[0x90]; bump the signal counter; out {0}"),
    0x1090: ("TRANSFER_FINISH", "sceGxmTransferFinish (libgpu 7E965369): in {_, hTransferContext}; 0x8100cfc8 waits for the transfer queue to drain; 0x18 = retry.", "no-op, out {0}"),
    0x1091: ("x91_INFO", "out 0x28 via ForDriver 990338F6(out+4). libgpu 768482F6, unreached by libgxm.", "unreached"),
    0x1092: ("x92_INFO", "out 0x124 via ForDriver C0C73C1B(out+4). libgpu C0D16CE5, unreached by libgxm.", "unreached"),
    0x108b: ("RT_BEGIN_SCENE_INFO", "sceGxmBeginScene (libgpu 83D972DF(conn, hRT, out4*)): in {_, hRT}; out 0x14 {err, width, height, msaa x:16 | y:16, (flags >> 17) & 1} from the render target; libgxm checks width/height against the colour surface (SCE_GXM_ERROR_INVALID_VALUE if they differ).", "from the 0x1086 descriptor: desc+0x14, +0x18, +0x1c, (desc+0 >> 17) & 1"),
    0x1093: ("WAIT_IDLE", "in {0, 0}; waits an event flag (ThreadmgrForDriver 1096042E), clears pp+0x4c/0x50; out {0}. sceGxmDestroyContext.", "no-op"),
}

MECHANICS = """## Mechanics (the kernel's dispatcher 0x810077dd -> 0x81001d30)

- `PVRSRV_BridgeDispatchKM(id, package)`, package = `{id, 0x1c, in, in_size, out, out_size, hKernelServices}`; `in_size`/`out_size`
  <= 0x1000, both copied through a kernel buffer; `handler(index, in_copy, out_copy, perProcessData)`; the copy-out happens only
  when the handler returns 0. Dispatcher results: 0, -14 (bad package / not connected), -25 (the "not supported" stub),
  0x18 (busy: libgpu's wrapper 0x810015cc sleeps 0xc9 us and retries).
- Every handler's **out[0] is its PVRSRV_ERROR** (0 OK, 3 INVALID_PARAMS, 0x13 NOT_SUPPORTED, 0x18 RETRY, 0xb8/0xb9 too many
  render targets / sync objects). **No handler reads in[0]** (libgpu leaves it uninitialised).
- Before the init server has run only 0x100c, 0x100d, 0x1048, 0x1049 pass; 0x104f, 0x1050, 0x1057, 0x1093 bypass the busy gate.
- Per-process data: +0x8 resman context, +0xc hKernelServices, +0x10 handle base, +0x24 registered memblocks, +0x30/34/38 render
  target counts, +0x3c sync object count, +0x40 kick in progress, +0x208 display mask, +0x214 == 0x1000000 enables transfers.
- Handles (PVRSRVAllocHandle 0x81002490) are per-process opaque words; types: 2 dev cookie, 3 devmem context, 4 heap, 5 kernel
  meminfo, 6 sync info, 0x16 display, 0x18 render context, 0x19 render target, 0x1a transfer context. The host returns any
  distinct nonzero cookie; the client only passes them back.
- libgpu's connection: `conn[0]` -> `{hKernelServices...}`; `conn[4]` = hDevCookie; the SGX connect callback runs 0x106c request 2.
"""


APPENDIX = """
## Completion only: what the host writes when GPU work "finishes" (read from libgxm, libgpu and the kernel)

**Kick (0x108c).** Every completion is a *write record* in the descriptor: `{ctl, value, hKernelMemInfo}`, `ctl = 0x400000 | (byteOffset + 0xf)`
(so `byteOffset = (ctl & 0xfffff) - 0xf`), target = that kernel meminfo's memory + byteOffset. Count A at +0x104 (<= 32 records at +0x140) are
the TA/vertex-phase writes, count B at +0x108 (<= 4 at +0x2c0) the 3D/fragment-phase writes. In an EndScene kick libgxm sends:
A = `{status+0 <- vertexProgress, status+4 <- ..., vertexNotification.address <- .value}`, B = `{status+8 <- ..., status+0xc <- fragmentProgress,
fragmentNotification.address <- .value}` where `status` is libgxm's first ALLOC_DEVICEMEM (0x400 bytes from heap 8) and the notification region
its second (0x820 bytes; `sceGxmGetNotificationRegion()` = base + 0x20). The four status words are what `sceGxmFinish` polls
(`*ctx[0x3fc] == ctx[0x400]` etc.) and what ring-buffer reuse waits on, so writing them at kick time makes libgxm see the rings as consumed.
The fragment sync object travels as +0x110 = pointer to a word holding its handle; its counters are kernel-internal (the display flip
compares them), nothing user-visible. The kick's 6 output words are not read by libgxm. Host: perform the A then B writes, bump the
signal counter, return out {0,...}; the GPU state words (+8..+0x104) need not be decoded.

**Transfer (0x108f).** `{_, hDevCookie, command words @8 (<= 0x1a), count @0x70, hTransferContext @0x74, hSync or 0 @0x78, @0x7c, @0x80,
notification record {ctl, value, hKernelMemInfo} @0x84, 1 if present @0x90}`. Host: do the copy/fill/downscale from the sceGxmTransfer*
arguments (seen at the GXM boundary), then write the notification exactly like a kick record. 0x1090 TransferFinish: out {0}.

**Display (0x104f / 0x1057 / 0x1050).** libgxm's thread `SceGxmDisplayQueue` (0x8100b05c) polls 0x1057 for the oldest queued slot; result
>= 0 -> it calls the game's display callback itself, then 0x1050; result < 0 -> it polls again immediately (an unimplemented 0x1057 makes
libgpu return 0x804c0000: the 11-million-call spin). Host: 0x104f out {0}; 0x1057 dispatcher result 0, out {0, 0, 0}; 0x1050 out {0}.
Do not call the game's callback from the host.

**sceGpuSignalWait(uint32_t *lastSeen, uint32_t timeoutUs)** (kernel 0x81009050): under a mutex, if `*lastSeen == gpuSignalCounter`
wait on a condition variable (the GPU interrupt bumps the counter and signals it) with the timeout, then `*lastSeen = gpuSignalCounter`;
returns 0 or the wait's error (timeout). libgxm calls it with a per-thread TLS word (sceKernelGetTLSAddr(0x44)) and 100000 us from every
`while (*addr != value)` loop (NotificationWait, Finish, ring reuse) and ignores the result. Host: keep a counter bumped on every kick/transfer
completion; store it to *lastSeen; return 0 (optionally yield when nothing changed).
"""


def main():
    outp = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else os.path.join(DEC, "maps", "gpu-bridge.md")
    log = lambda *a: print(*a, file=sys.stderr, flush=True)
    log("kernel table"); tab = kernel_table()
    km = Mod(KERNEL)
    knames = {a: nm(k) for a, k in km.stub.items()}
    knames.update({a & ~1: nm(k) for k, a in km.exports.items()})
    for a, n in list(knames.items()):
        if n in ("__stack_chk_fail", "ksceKernelPanic"): km.an.noreturn.add(a)
    gm = Mod(LIBGPU)
    log("libgpu ids"); ids = libgpu_ids(gm)
    gx = Mod(LIBGXM)
    for a, k in gx.stub.items():
        if nm(k) == "__stack_chk_fail": gx.an.noreturn.add(a)
    reach = {}      # libgxm export -> libgpu exports
    for k, a in gx.exports.items():
        g = sorted(nm(i) for i in gx.reach(a) if i[0].startswith("SceGpuEs4"))
        if g: reach[nm(k)] = g
    by_gpu = collections.defaultdict(set)
    for gxn, gs in reach.items():
        for g in gs: by_gpu[g].add(gxn)
    log("libgpu protocols"); gpu = GpuEmu()
    protocols = {}
    for (lib, nid) in gm.exports:
        if lib.startswith("SceGpuEs4User"): log(f"  {nid:08X}"); protocols[nid] = gpu.protocol(nid)
    log("scenario"); e, results = scenario()
    for n, r in results: log(f"  {n} -> {r:#x}")

    real = sorted(i for i, h in tab.items() if h != NOT_SUPPORTED)
    w = open(outp, "w").write
    w(f"# GPU bridge map: SceGpuEs4 <- libgpu_es4 <- libgxm\n\nGenerated by `dev/research/gpumap.py` from `kd/bootimage/SceGpuEs4.skprx.elf`, `us/libgpu_es4.suprx.elf`, "
      f"`us/libgxm_es4.suprx.elf`. Derived from Sony code: local only. {len(real)} real bridge ids of 0x1000-0x1093; the dispatch table at {TABLE:#x} is filled by {INIT:#x}.\n\n")
    w(MECHANICS)
    # the scenario
    w("\n## The scenario: libgxm's own code over libgpu, the host recipe answering the bridge\n\n"
      "Each call's result (0 = libgxm is satisfied with what the host returned), then every bridge call it made with the input words decoded.\n\n")
    w("| libgxm call | result |\n|---|---|\n")
    for name, r in results: w(f"| `{name}` | {'OK' if r == 0 else f'{r:#x}'} |\n")
    if e.state.get("errors"): w("\nEmulation errors: " + "; ".join(e.state["errors"]) + "\n")
    w("\n")
    for phase in dict.fromkeys(c["phase"] for c in e.bridge):
        w(f"### {phase}\n\n")
        for c in e.bridge:
            if c["phase"] != phase: continue
            name = KNOW.get(c["id"], ("?",))[0]
            words = []
            for o in range(4, len(c["inb"]), 4):
                v, = struct.unpack_from("<I", c["inb"], o)
                if v: words.append(f"+{o:#x}={v:#x}{e.describe(v)}")
            if c["user"]: words.append(f"user struct at in[8], request {c['user']['req']}")
            if c.get("writes"): words.append("completion writes: " + ", ".join(f"[{a:#x}] <- {v:#x} (ctl {k:#x})" for a, v, k in c["writes"]))
            cons = sorted((off, pcs) for (k, off), pcs in e.taint.items() if k == c["k"])
            w(f"- `{c['id']:#x}` {name} in[{c['insz']:#x}] out[{c['outsz']:#x}]" + (": " + ", ".join(words) if words else "") + "\n")
            if cons: w("  - libgxm consumes: " + ", ".join(f"out+{off:#x} ({', '.join(sorted({e.fn_name(p) for p in pcs}))})" if off < 0x100 else f"user+{off - 0x100:#x}" for off, pcs in cons) + "\n")
    if e.state.get("finish waits"):
        w(f"\nAfter EndScene: notification region {e.state['notification region']:#x}; sceGxmFinish waits for " + ", ".join(f"*{a:#x} == {v:#x}" for a, v in e.state["finish waits"]) + " (the kick's internal records).\n")
    heapcons = sorted(((k, off), pcs) for (k, off), pcs in e.taint.items() if 0xd0 <= k < 0xe0)
    # who reaches what
    w("\n## Who reaches what\n\n| libgxm export | libgpu exports (bridge ids) |\n|---|---|\n")
    for gxn in sorted(reach):
        w(f"| `{gxn}` | " + ", ".join(f"`{g}`" + (f" ({', '.join(f'{i:#x}' for i in sorted(ids.get(next((k for k in gm.exports if nm(k) == g), None), ())))})" if g != "sceGpuSignalWait" else "") for g in reach[gxn]) + " |\n")
    w("\n`sceGxmCreateContext` and `sceGxmShaderPatcher*` reach no bridge id (pure: they only need the game's prior Map* calls).\n"
      "**No submission id is reached outside the GXM boundary**: 0x108c (kick), 0x108f/0x1090 (transfer), 0x104f (display flip) and 0x108b (begin-scene info) "
      "are reached only from the functions `gxm_boundary()` intercepts, plus the internal `_sceGxmBeginScene`/`_sceGxmMidSceneFlush` that nothing outside libgxm calls; "
      "`sceGxmDestroyContext` reaches 0x1093 (wait idle) and `sceGpuSignalWait`, both no-ops for the host.\n")
    # per id
    w("\n## The ids\n\n")
    for i in real:
        h = tab[i]; name, meaning, host = KNOW.get(i, (f"id {i:#x}", "?", "?"))
        s = kernel_scan(km, knames, h)
        w(f"### {i:#x} {name}\n\n- kernel handler `{h:#x}`" + (f" ({s[4]} bytes); reads in {' '.join(f'+{o:#x}' + (f':{z}' if z != 4 else '') for o, z in sorted(s[0].items())) or '-'}; writes out {' '.join(f'+{o:#x}' + (f':{z}' if z != 4 else '') for o, z in sorted(s[1].items())) or '-'}; pp {' '.join(f'+{o:#x}' for o in sorted(s[2])) or '-'}; calls {', '.join(dict.fromkeys(c for c, a in s[3] if not c.startswith('sub_') and c != '__stack_chk_fail')) or '-'}" if s else "") + "\n")
        gpu_exports = sorted(k for k, v in ids.items() if i in v)
        for k in gpu_exports:
            nid = k[1]; lines = protocols.get(nid, [])
            mine = [l for l in lines if l.startswith(f"id {i:#x}")] + [l for l in lines if not l.startswith("id ")]
            w(f"- libgpu `{nm(k)}` <- {', '.join(f'`{g}`' for g in sorted(by_gpu.get(nm(k), []))) or 'no libgxm export'}" + (": " + "; ".join(mine) if mine else "") + "\n")
        w(f"- meaning: {meaning}\n- host: {host}\n\n")
    # the knowledge the scenario adds
    if heapcons:
        w("## Heap entries libgxm reads (from the scenario)\n\n" + ", ".join(f"heap{k - 0xd0}+{off:#x}" for (k, off), pcs in heapcons) + "\n\n")
    w("## Host heap table proven by the scenario\n\n| id | base | size | role |\n|---|---|---|---|\n")
    for hid, (b, sz, role) in HEAPS.items(): w(f"| {hid:#x} | {b:#x} | {sz:#x} | {role}{' (allocations from +0x01000000: bank 4)' if hid == 4 else ''} |\n")
    w(APPENDIX)
    w("\n## Open questions\n\n- The heap records' real device-virtual bases are built at boot by SceGpuEs4Init (not in SceGpuEs4's data); the synthetic bases above satisfy libgxm's arithmetic. "
      "Real values would only change the USSE offsets libgxm writes into its own GPU programs, which the host's renderer does not read.\n"
      "- Which fields of the 0x60-byte block in 0x105e (devinfo+0x800) libgxm would read in paths the scenario does not exercise (none in Initialize..ShaderPatcherCreate).\n")
    print(f"wrote {outp}")


if __name__ == "__main__":
    main()
