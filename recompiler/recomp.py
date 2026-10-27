#!/usr/bin/env python3
"""Static recompiler (prototype): ARMv7 Thumb-2 + VFP + NEON guest code -> C over a Cpu struct
(recompiler/rt.h), one C function per guest function, exact by construction: every instruction
becomes the statements it executes, flags included, against guest memory in a 4 GiB arena.

Input is a module's Ghidra export data (<name>.data.json: code bytes, function entries and
bodies), so it runs on anything the decomp pipeline has exported (firmware modules today, game
modules later). Unsupported instructions become UNIMPL(addr) and are counted.

usage: recomp.py <export.data.json> <out.c> [--split N] [--module NAME] [--hooks "addr:name ..."]
--split N writes <out>.h (runtime + prototypes), <out>_part0..N-1.c (the functions) and <out>.c
(import names and the lookup table); compile all of them together.
--hooks "0xADDR:name ...": at the entry of each listed guest function (before its body, after
ENTER), emit a call to a host function recomp_hook_<name>(Cpu *c) -- a per-game feature (packs/
<title>/pack.toml's HOOKS, read by vitarecomp's recompile step), not a runtime change: rt.h is
untouched, and with no --hooks the generated code is byte-identical to a plain run. The host side
(the actual recomp_hook_<name> definitions) lives in the game's packs/<title>/hooks.c (the shared ones: runtime/core/hooks.c).
--insn-hooks "0xADDR:name ...": the same, after one instruction instead of at a function's entry:
recomp_ihook_<name>(Cpu *c, int reg), reg = the instruction's destination core register (-1 if it
has none) -- for a constant a function builds mid-body (a pack's IHOOKS).
--unordered-loads: plain loads everywhere (A/B only). By default a function that uses exclusives or
barriers (lock-free code) gets ordered loads (rt.h RD32A...); see SYNC_MN.
"""
import bisect, json, os, re, sys
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_ARM
from capstone.arm import (ARM_OP_REG, ARM_OP_IMM, ARM_OP_MEM, ARM_OP_FP, ARM_SFT_INVALID)

md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
md.detail = True
md_arm = Cs(CS_ARCH_ARM, CS_MODE_ARM)   # ARM-mode functions (hand-written library code, e.g. FMOD's mixers)
md_arm.detail = True
COND = ("eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "cs", "cc")
GPR = {f"r{i}": i for i in range(13)} | {"sb": 9, "sl": 10, "fp": 11, "ip": 12, "sp": 13, "lr": 14, "pc": 15}
HOOKS = {}   # --hooks: {entry addr: host hook name}; empty unless a game's conf asks for one (see usage above)
IHOOKS = {}  # --insn-hooks: {instruction addr: host hook name}, called after that instruction
# lock-free code: a function with any of these gets ordered loads (each one before every later load, rt.h RD32A). ARMv7
# lets loads pass each other, so code that leans on their order without a barrier between them is wrong on paper and
# rarely on the Vita's Cortex-A9, but a wide host core reorders them often enough to show: Killzone's job queue read a
# slot before the commit index that published it, got the worker from the lap before and froze (2026-10-18).
SYNC_MN = {"ldrex", "strex", "ldrexb", "strexb", "ldrexh", "strexh", "ldrexd", "strexd", "dmb", "dsb"}
ORDERED_LOADS = [True]   # --unordered-loads clears it
# the import stubs of setjmp: a call to one becomes a host _setjmp in the calling function's own frame
# (rt.h's RecompJmp), so the host's guest longjmp (main.c) can unwind the host stack back to it -- a
# recompiled function is a C function, and Sony's longjmp, recompiled, could only return to its caller
# (every Lua error crashed: Lua 5.1 raises them with longjmp; 2026-10-13)
SETJMP_STUBS = set()


class Unsupported(Exception):
    pass


class Fn:
    def __init__(self, entry, insns, entries, arm=False):
        self.entry, self.insns, self.entries, self.arm = entry, insns, entries, arm
        self.pc_off = 8 if arm else 4     # a PC read sees the instruction's address + 8 (ARM) or + 4 (Thumb)
        self.tbit = 0 if arm else 1       # what a call's return address carries
        self.addrs = {i.address for i in insns}
        self.lines, self.unimpl = [], []
        self.ordered = ORDERED_LOADS[0] and any(i.mnemonic in SYNC_MN for i in insns)

    def reg(self, rid):
        n = md.reg_name(rid)
        return n

    def r(self, rid, insn):
        """a GPR operand's value (pc reads as the instruction's address + 4)"""
        n = md.reg_name(rid)
        if n == "pc":
            return f"0x{insn.address + self.pc_off:x}u"
        if n not in GPR:
            raise Unsupported(f"register {n}")
        return f"c->r[{GPR[n]}]"

    def dst(self, rid):
        n = md.reg_name(rid)
        if n not in GPR or n == "pc":
            raise Unsupported(f"destination {n}")
        return f"c->r[{GPR[n]}]"

    def operand(self, o, insn, setc=False):
        """value of a flexible second operand (immediate, register, shifted register)"""
        if o.type == ARM_OP_IMM:
            return f"0x{o.imm & 0xffffffff:x}u"
        if o.type != ARM_OP_REG:
            raise Unsupported("operand type")
        v = self.r(o.reg, insn)
        st = o.shift.type
        if st == ARM_SFT_INVALID or (st in (1, 2, 3, 4) and o.shift.value == 0 and st != 5):
            return v
        amt = (f"(c->r[{GPR[md.reg_name(o.shift.value)]}] & 0xff)" if st >= 6 else str(o.shift.value))
        return f"SHIFT(c, {v}, {st}, {amt}, {1 if setc else 0})"

    def addr(self, op, insn):
        m = op.mem
        base = self.r(m.base, insn) if md.reg_name(m.base) != "pc" else f"0x{(insn.address + self.pc_off) & ~3:x}u"
        if m.index:
            idx = self.r(m.index, insn)
            # this capstone reports [rN, rM, lsl #k] in the operand's shift, not mem.lshift. ARM mode
            # also has lsr/asr/ror/rrx offsets ([r5, sl, lsr #12] in libgxm): those went through as lsl
            st, sh = (op.shift.type, op.shift.value) if op.shift.type else ((2, m.lshift) if m.lshift else (0, 0))
            if st == 2 and sh:
                idx = f"({idx} << {sh})"
            elif st in (1, 3, 4, 5):
                idx = f"SHIFT(c, {idx}, {st}, {sh if st != 5 else 1}u, 0)"
            return f"({base} {'-' if m.scale == -1 else '+'} {idx})"
        return f"({base} + {m.disp})" if m.disp else base

    def emit(self, s):
        self.lines.append(s)

    # ------------------------------------------------------------------ instructions
    def translate(self, i):
        mn = i.mnemonic.split(".")[0]
        dt = i.mnemonic.split(".")[1] if "." in i.mnemonic else ""
        if i.cc != 15 and mn not in ("b", "it", "ite", "itt", "itet", "itte", "ittt", "itee", "itett",
                                     "ittee", "iteet", "itttt", "iteee", "itete", "ittte", "itetee"):
            for cc in COND:
                if mn.endswith(cc) and len(mn) > len(cc):
                    mn = mn[:-len(cc)]
                    break
        ops = list(i.operands)
        # capstone's update_flags is also true for some non-S forms (adc.w): trust the mnemonic's S
        S = i.update_flags and mn.endswith("s") and mn != "mls"
        nxt = i.address + i.size
        cc = i.cc
        out = []
        A = i.address

        def guard(body):
            if cc == 15 or mn in ("b", "cbz", "cbnz"):
                return body
            return [f"if (COND(c, {cc})) {{"] + ["\t" + b for b in body] + ["}"]

        if mn.startswith("it"):
            return []
        if mn == "mov" and ops[0].type == ARM_OP_REG and md.reg_name(ops[0].reg) == "pc":
            # a computed jump (a jump table's target) inside the function, or a tail call
            t = self.r(ops[1].reg, i)
            return guard([f"{{ uint32_t t = {t} & ~1u; GuestFn f = recomp_lookup(t); if (f) {{ f(c); return; }} JUMP(t); }}"])
        if mn in ("mov", "movs", "mvn", "mvns"):
            v = self.operand(ops[1], i, setc=S)
            if mn.startswith("mvn"):
                v = f"~{v}"
            ic = self.imm_carry(i) if S and ops[1].type == ARM_OP_IMM else None
            out = [f"{{ uint32_t t = {v}; {self.dst(ops[0].reg)} = t;" + (" NZ(c, t);" if S else "")
                   + (f" c->c = {ic};" if ic is not None else "") + " }"]
        elif mn == "movw":
            out = [f"{self.dst(ops[0].reg)} = 0x{ops[1].imm & 0xffff:x}u;"]
        elif mn == "movt":
            d = self.dst(ops[0].reg)
            out = [f"{d} = ({d} & 0xffffu) | (0x{ops[1].imm & 0xffff:x}u << 16);"]
        elif mn in ("add", "adds", "sub", "subs", "adc", "adcs", "sbc", "sbcs", "rsb", "rsbs", "addw", "subw"):
            # Thumb's pc-relative add/sub with an immediate (ADDW/SUBW T4, ADD T1/T3 from the pc: the ADR forms)
            # read the pc as Align(address + 4, 4), not address + 4 -- Killzone's MD5 formed each round's
            # continuation with `addw ip, pc, #0x4f` at a 2-aligned site and `bx ip` landed two bytes into
            # an instruction (every "missed function" of its boot was one of those odd addresses)
            if not self.arm and mn in ("add", "addw", "sub", "subw") and len(ops) == 3 and ops[1].type == ARM_OP_REG and md.reg_name(ops[1].reg) == "pc" \
               and ops[2].type == ARM_OP_IMM and md.reg_name(ops[0].reg) != "pc":
                base = (A + 4) & ~3
                return [f"{self.dst(ops[0].reg)} = 0x{(base + ops[2].imm if mn in ('add', 'addw') else base - ops[2].imm) & 0xffffffff:x}u;"]
            if md.reg_name(ops[0].reg) == "pc":
                # ARM's switch: "add pc, pc, rN, lsl #2" into a table of branches right after it
                if mn == "add" and self.arm and len(ops) == 3 and ops[1].type == ARM_OP_REG and md.reg_name(ops[1].reg) == "pc":
                    return guard([f"JUMP(0x{A + 8:x}u + {self.operand(ops[2], i)});"])
                if mn == "add" and not self.arm and len(ops) == 2 and ops[1].type == ARM_OP_REG and md.reg_name(ops[1].reg) != "pc":
                    # Thumb's "add pc, rN": a computed jump to (the address + 4) + rN, bit 0 dropped, as mov pc
                    return guard([f"{{ uint32_t t = (0x{A + 4:x}u + {self.r(ops[1].reg, i)}) & ~1u; GuestFn f = recomp_lookup(t); if (f) {{ f(c); return; }} JUMP(t); }}"])
                raise Unsupported("alu to pc")
            if len(ops) == 2:
                a, b = self.r(ops[0].reg, i), self.operand(ops[1], i)
            else:
                a, b = self.r(ops[1].reg, i), self.operand(ops[2], i)
            base = mn.rstrip("s") if mn not in ("adds", "subs") else mn[:-1]
            base = {"addw": "add", "subw": "sub"}.get(base, base)
            if base == "add":
                e = f"ADDC(c, {a}, {b}, 0, {1 if S else 0})"
            elif base == "sub":
                e = f"SUBC(c, {a}, {b}, 1, {1 if S else 0})"
            elif base == "adc":
                e = f"ADDC(c, {a}, {b}, c->c, {1 if S else 0})"
            elif base == "sbc":
                e = f"SUBC(c, {a}, {b}, c->c, {1 if S else 0})"
            elif base == "rsb":
                e = f"SUBC(c, {b}, {a}, 1, {1 if S else 0})"
            else:
                raise Unsupported(mn)
            out = [f"{self.dst(ops[0].reg)} = {e};"]
        elif mn in ("and", "ands", "orr", "orrs", "eor", "eors", "bic", "bics", "orn", "orns"):
            base = mn[:3]
            if len(ops) == 2:
                a, b = self.r(ops[0].reg, i), self.operand(ops[1], i, setc=S)
            else:
                a, b = self.r(ops[1].reg, i), self.operand(ops[2], i, setc=S)
            e = {"and": f"{a} & {b}", "orr": f"{a} | {b}", "eor": f"{a} ^ {b}", "bic": f"{a} & ~{b}", "orn": f"{a} | ~{b}"}[base]
            ic = self.imm_carry(i) if S and ops[-1].type == ARM_OP_IMM else None
            out = [f"{{ uint32_t t = {e}; {self.dst(ops[0].reg)} = t;" + (" NZ(c, t);" if S else "")
                   + (f" c->c = {ic};" if ic is not None else "") + " }"]
        elif mn in ("lsl", "lsls", "lsr", "lsrs", "asr", "asrs", "ror", "rors") and len(ops) == 2 and ops[1].type == ARM_OP_REG and ops[1].shift.type:
            # ARM mode's "lsr r8, r8, #4" is an alias of "mov r8, r8, lsr #4": the shift rides on the
            # operand (by an immediate or a register), not in a third operand -- read as the Thumb
            # two-operand form it shifted by the register's own low byte
            v = self.operand(ops[1], i, setc=S)
            out = [f"{{ uint32_t t = {v}; {self.dst(ops[0].reg)} = t;" + (" NZ(c, t);" if S else "") + " }"]
        elif mn in ("lsl", "lsls", "lsr", "lsrs", "asr", "asrs", "ror", "rors"):
            t = {"asr": 1, "lsl": 2, "lsr": 3, "ror": 4}[mn[:3]]
            src = ops[1] if len(ops) == 3 else ops[0]
            amt_op = ops[-1]
            amt = (f"0x{amt_op.imm:x}u" if amt_op.type == ARM_OP_IMM else f"({self.r(amt_op.reg, i)} & 0xff)")
            if amt_op.type == ARM_OP_IMM and amt_op.imm == 0 and t in (1, 3):
                amt = "32"   # asr/lsr #32 encodes as #0 in some forms
            out = [f"{{ uint32_t t = SHIFT(c, {self.r(src.reg, i)}, {t}, {amt}, {1 if S else 0}); {self.dst(ops[0].reg)} = t;"
                   + (" NZ(c, t);" if S else "") + " }"]
        elif mn in ("cmp", "cmn", "tst", "teq"):
            a, b = self.r(ops[0].reg, i), self.operand(ops[1], i, setc=mn in ("tst", "teq"))
            if mn == "cmp":
                out = [f"SUBC(c, {a}, {b}, 1, 1);"]
            elif mn == "cmn":
                out = [f"ADDC(c, {a}, {b}, 0, 1);"]
            else:
                ic = self.imm_carry(i) if ops[1].type == ARM_OP_IMM else None
                out = [f"NZ(c, {a} {'&' if mn == 'tst' else '^'} {b});" + (f" c->c = {ic};" if ic is not None else "")]
        elif mn in ("mul", "muls"):
            a = self.r(ops[1].reg, i); b = self.r(ops[2].reg if len(ops) == 3 else ops[0].reg, i)
            out = [f"{{ uint32_t t = {a} * {b}; {self.dst(ops[0].reg)} = t;" + (" NZ(c, t);" if S else "") + " }"]
        elif mn in ("mla", "mls"):
            e = f"{self.r(ops[3].reg, i)} {'+' if mn == 'mla' else '-'} {self.r(ops[1].reg, i)} * {self.r(ops[2].reg, i)}"
            out = [f"{self.dst(ops[0].reg)} = {e};"]
        elif mn in ("umull", "smull", "umlal", "smlal"):
            lo, hi, a, b = (self.dst(ops[k].reg) for k in range(2)), None, self.r(ops[2].reg, i), self.r(ops[3].reg, i)
            lo, hi = self.dst(ops[0].reg), self.dst(ops[1].reg)
            ty, cast = ("uint64_t", "(uint64_t)") if mn[0] == "u" else ("int64_t", "(int64_t)(int32_t)")
            acc = f" + (({ty})(((uint64_t){hi} << 32) | {lo}))" if mn.endswith("lal") else ""
            out = [f"{{ {ty} t = {cast}{a} * {cast}{b}{acc}; {lo} = (uint32_t)t; {hi} = (uint32_t)((uint64_t)t >> 32); }}"]
        elif re.fullmatch(r"smul[bt][bt]|smla[bt][bt]", mn):
            # 16x16 signed multiplies: bottom/top halves of each operand (smla adds an accumulator)
            hx, hy = mn[-2], mn[-1]
            x = f"(int32_t)(int16_t)({self.r(ops[1].reg, i)}{' >> 16' if hx == 't' else ''})"
            y = f"(int32_t)(int16_t)({self.r(ops[2].reg, i)}{' >> 16' if hy == 't' else ''})"
            acc = f" + {self.r(ops[3].reg, i)}" if mn.startswith("smla") else ""
            out = [f"{self.dst(ops[0].reg)} = (uint32_t)({x} * {y}){acc};"]
        elif mn in ("udiv", "sdiv"):
            a, b = self.r(ops[1].reg, i), self.r(ops[2].reg, i)
            e = (f"({b} ? {a} / {b} : 0)" if mn == "udiv"
                 else f"({b} ? (uint32_t)((int32_t){a} == (int32_t)0x80000000 && (int32_t){b} == -1 ? (int32_t)0x80000000 : (int32_t){a} / (int32_t){b}) : 0)")
            out = [f"{self.dst(ops[0].reg)} = {e};"]
        elif mn in ("uxtb", "uxth", "sxtb", "sxth"):
            v = self.operand(ops[1], i)
            e = {"uxtb": f"(uint8_t)({v})", "uxth": f"(uint16_t)({v})", "sxtb": f"(uint32_t)(int8_t)({v})",
                 "sxth": f"(uint32_t)(int16_t)({v})"}[mn]
            out = [f"{self.dst(ops[0].reg)} = {e};"]
        elif mn in ("uxtab", "uxtah", "sxtab", "sxtah"):
            v = self.operand(ops[2], i)
            e = {"uxtab": f"(uint8_t)({v})", "uxtah": f"(uint16_t)({v})", "sxtab": f"(uint32_t)(int8_t)({v})",
                 "sxtah": f"(uint32_t)(int16_t)({v})"}[mn]
            out = [f"{self.dst(ops[0].reg)} = {self.r(ops[1].reg, i)} + {e};"]
        elif mn in ("ubfx", "sbfx"):
            src, lsb, w = self.r(ops[1].reg, i), ops[2].imm, ops[3].imm
            e = (f"(({src} >> {lsb}) & 0x{(1 << w) - 1:x}u)" if mn == "ubfx"
                 else f"(uint32_t)(((int32_t)({src} << {32 - lsb - w})) >> {32 - w})")
            out = [f"{self.dst(ops[0].reg)} = {e};"]
        elif mn in ("bfi", "bfc"):
            d = self.dst(ops[0].reg)
            lsb, w = (ops[2].imm, ops[3].imm) if mn == "bfi" else (ops[1].imm, ops[2].imm)
            m = ((1 << w) - 1) << lsb
            src = f"(({self.r(ops[1].reg, i)} << {lsb}) & 0x{m:x}u)" if mn == "bfi" else "0"
            out = [f"{d} = ({d} & 0x{~m & 0xffffffff:x}u) | {src};"]
        elif mn == "clz":
            out = [f"{self.dst(ops[0].reg)} = CLZ({self.r(ops[1].reg, i)});"]
        elif mn == "rbit":
            out = [f"{self.dst(ops[0].reg)} = RBIT({self.r(ops[1].reg, i)});"]
        elif mn in ("rev", "rev16", "revsh"):
            v = self.r(ops[1].reg, i)
            e = {"rev": f"BSWAP({v})", "rev16": f"(((({v}) >> 8) & 0x00ff00ffu) | ((({v}) << 8) & 0xff00ff00u))",
                 "revsh": f"(uint32_t)(int16_t)((({v} & 0xff) << 8) | (({v} >> 8) & 0xff))"}[mn]
            out = [f"{self.dst(ops[0].reg)} = {e};"]
        elif mn in ("ldr", "ldrb", "ldrh", "ldrsb", "ldrsh", "str", "strb", "strh",
                    "ldrd", "strd", "ldrex", "strex", "ldrexd", "strexd", "ldrexb", "strexb", "ldrexh", "strexh"):
            out = self.mem(i, mn, ops)
            if out is None:   # a load into pc: a return or computed jump (ARM: possibly conditional, "ldrls pc, ...")
                return guard(self.load_pc(i, ops))
        elif mn in ("push", "pop", "ldm", "stm", "ldmia", "stmia", "ldmdb", "stmdb", "ldmfd", "stmfd", "stmea",
                    "ldmib", "stmib", "ldmda", "stmda"):
            r = self.multi(i, mn, ops)
            return guard(r) if cc != 15 else r
        elif mn in ("b", "bl", "blx", "bx", "cbz", "cbnz", "tbb", "tbh"):
            return self.branch(i, mn, ops)
        elif mn in ("dmb", "dsb"):
            out = ["__atomic_thread_fence(__ATOMIC_SEQ_CST);"]   # other guest threads are host threads
        elif mn == "clrex":
            out = ["CLREX();"]
        elif mn == "mrc" and re.fullmatch(r"p15, #0, (\w+), c13, c0, #3", i.op_str):
            out = [f"{self.dst(ops[2].reg)} = recomp_tpidruro;"]   # the thread's TLS pointer
        elif mn in ("nop", "pld", "pli", "isb", "hint", "yield", "wfe", "wfi", "sev"):
            out = []   # hints
        elif mn == "adr":
            out = [f"{self.dst(ops[0].reg)} = 0x{((A + self.pc_off) & ~3) + ops[1].imm & 0xffffffff:x}u;"]
        elif mn.startswith("v"):
            out = self.vfp(i, mn, dt, ops)
        else:
            raise Unsupported(mn)
        return guard(out)

    def mem(self, i, mn, ops):
        if mn == "ldrexd":   # (rt, rt2, [rn])
            return [f"{{ uint64_t v = LDREXD({self.addr(ops[2], i)}); {self.dst(ops[0].reg)} = (uint32_t)v; {self.dst(ops[1].reg)} = (uint32_t)(v >> 32); }}"]
        if mn == "strexd":   # (rd, rt, rt2, [rn])
            return [f"{{ {self.dst(ops[0].reg)} = STREXD({self.addr(ops[3], i)}, (uint64_t){self.r(ops[2].reg, i)} << 32 | {self.r(ops[1].reg, i)}); }}"]
        if mn in ("strex", "strexb", "strexh"):   # (rd, rt, [rn]): a compare-and-swap against what LDREX saw (rt.h)
            return [f"{{ uint32_t ea = {self.addr(ops[2], i)}; {self.dst(ops[0].reg)} = {mn[:5].upper()}{mn[5:].upper()}(ea, {self.r(ops[1].reg, i)}); }}"]
        if mn in ("ldrexb", "ldrexh"):   # (rt, [rn])
            return [f"{{ {self.dst(ops[0].reg)} = {mn.upper()}({self.addr(ops[1], i)}); }}"]
        load = mn.startswith("ld")
        pair = mn in ("ldrd", "strd")
        regs = ops[:2] if pair else ops[:1]
        m_op = ops[2] if pair else ops[1]
        if m_op.type != ARM_OP_MEM:
            raise Unsupported("memory operand")
        if load and any(md.reg_name(o.reg) == "pc" for o in regs):
            return None
        m = m_op.mem
        post = len(ops) > (3 if pair else 2)
        base_r = md.reg_name(m.base)
        a = self.r(m.base, i) if post else self.addr(m_op, i)
        sz = {"ldr": 4, "str": 4, "ldrb": 1, "strb": 1, "ldrh": 2, "strh": 2, "ldrsb": 1, "ldrsh": 2,
              "ldrd": 4, "strd": 4, "ldrex": 4, "strex": 4}[mn]
        out = [f"{{ uint32_t ea = {a};"]
        for k, o in enumerate(regs):
            off = f" + {4 * k}" if k else ""
            if load:
                rd = {"ldr": "RD32", "ldrd": "RD32", "ldrex": "LDREX", "ldrb": "RD8", "ldrh": "RD16",
                      "ldrsb": "(uint32_t)(int8_t)RD8", "ldrsh": "(uint32_t)(int16_t)RD16"}[mn]
                if self.ordered and mn != "ldrex":
                    rd = re.sub(r"RD(8|16|32)$", r"RD\1A", rd)
                out.append(f" {self.dst(o.reg)} = {rd}(ea{off});")
            else:
                wr = {4: "WR32", 2: "WR16", 1: "WR8"}[sz]
                out.append(f" {wr}(ea{off}, {self.r(o.reg, i)});")
        if post:
            po = ops[-1]
            inc = (f"0x{po.imm & 0xffffffff:x}u" if po.type == ARM_OP_IMM else self.r(po.reg, i))
            out.append(f" {self.dst(m.base)} = ea {'-' if getattr(po, 'subtracted', False) else '+'} {inc};")
        elif i.writeback:
            out.append(f" {self.dst(m.base)} = ea;")
        out.append(" }")
        return ["".join(out)]

    def load_pc(self, i, ops):
        """ldr pc, [sp], #4 (a return) or ldr pc, [rN, ...] (a computed jump)"""
        m = ops[1].mem
        if md.reg_name(m.base) == "sp" and len(ops) == 3:
            return [f"c->r[13] += {ops[2].imm}; return;"]
        lo, hi = min(x.address for x in self.insns), max(x.address for x in self.insns)   # this function's own code
        # a function's entry is a tail call; anything else is a case label of this function's own
        # switch (ARM's "ldrls pc, [pc, rN, lsl #2]" table): jump there, as mov pc does
        return [f"{{ uint32_t t = RD32({self.addr(ops[1], i)}); GuestFn f = recomp_lookup(t & ~1u); if (f) {{ f(c); return; }} "
                f"if ((t & ~1u) >= 0x{lo:x}u && (t & ~1u) <= 0x{hi:x}u) JUMP(t & ~1u); recomp_call_external(c, t); return; }}"]

    def multi(self, i, mn, ops):
        if mn in ("push", "pop"):
            base, regs, wb = "sp", ops, True
            mn = "stmdb" if mn == "push" else "ldmia"
        else:
            base, regs, wb = md.reg_name(ops[0].reg), ops[1:], i.writeback
        load = mn.startswith("ld")
        before = mn in ("stmdb", "ldmdb", "stmfd")
        names = [md.reg_name(o.reg) for o in regs]
        n = len(names)
        b = f"c->r[{GPR[base]}]"
        # the lowest address: IA base, DB base - 4n, IB base + 4, DA base - 4n + 4 (ARM ARM LDM/STM)
        start = {"ib": " + 4", "da": f" - {4 * n - 4}"}.get(mn[-2:], f" - {4 * n}" if before else "")
        before = before or mn.endswith("da")   # the writeback direction
        out = [f"{{ uint32_t ea = {b}{start};"]
        ret = False
        for k, nm in enumerate(names):
            if load and nm == "pc":
                ret = True
                continue
            if load:
                out.append(f" c->r[{GPR[nm]}] = RD32(ea + {4 * k});")
            elif nm == "pc":   # stores the instruction's address + 8 (c->r[15] is only a jump's target)
                out.append(f" WR32(ea + {4 * k}, 0x{i.address + self.pc_off:x}u);")
            else:
                out.append(f" WR32(ea + {4 * k}, c->r[{GPR[nm]}]);")
        if wb and not (load and base in names):
            out.append(f" {b} = {b} {'-' if before else '+'} {4 * n};")
        out.append(" }")
        if ret:
            out.append("return;")
        return ["".join(out)]

    def call(self, target, nxt):
        t = target & ~1
        if t in self.entries:
            return [f"c->r[14] = 0x{nxt | self.tbit:x}u; fn_{t:08x}(c); if (c->stop) return;"]
        if t in SETJMP_STUBS:   # 0 now; a guest longjmp to this buffer comes back here with its value
            return [f"c->r[14] = 0x{nxt | self.tbit:x}u; {{ RecompJmp *rj = recomp_setjmp_begin(c); if (!_setjmp(rj->host)) c->r[0] = 0; }} if (c->stop) return;"]
        return [f"c->r[14] = 0x{nxt | self.tbit:x}u; recomp_call_external(c, 0x{target:x}u); if (c->stop) return;"]

    def branch(self, i, mn, ops):
        nxt = i.address + i.size
        cc = i.cc
        if mn in ("b",):
            t = ops[0].imm & 0xffffffff
            if t in self.addrs:
                body = [f"goto L_{t:08x};"]
            else:   # a tail call into another function
                body = self.call(t, nxt) + ["return;"]
            return body if cc == 15 else [f"if (COND(c, {cc})) {{"] + body + ["}"]
        if mn in ("cbz", "cbnz"):
            r, t = self.r(ops[0].reg, i), ops[1].imm & 0xffffffff
            test = f"{r} {'==' if mn == 'cbz' else '!='} 0"
            if t in self.addrs:
                return [f"if ({test}) goto L_{t:08x};"]
            return [f"if ({test}) {{"] + self.call(t, nxt) + ["return; }"]
        if mn == "bl" or (mn == "blx" and ops[0].type == ARM_OP_IMM):
            body = self.call(ops[0].imm & 0xffffffff, nxt)
            return body if cc == 15 else [f"if (COND(c, {cc})) {{"] + body + ["}"]
        if mn in ("blx", "bx"):
            rn = md.reg_name(ops[0].reg)
            if mn == "bx" and rn == "lr":
                body = ["return;"]
            else:
                r = self.r(ops[0].reg, i)
                body = [f"{{ uint32_t t = {r};" + (f" c->r[14] = 0x{nxt | self.tbit:x}u;" if mn == "blx" else "")
                        + " GuestFn f = recomp_lookup(t & ~1u); if (f) f(c); else recomp_call_external(c, t); }",
                        "if (c->stop) return;"] + (["return;"] if mn == "bx" else [])
            return body if cc == 15 else [f"if (COND(c, {cc})) {{"] + body + ["}"]
        if mn in ("tbb", "tbh"):
            m = ops[0].mem
            if md.reg_name(m.base) != "pc":
                raise Unsupported("tbb base")
            idx = self.r(m.index, i)
            tab = i.address + 4
            ld = f"RD8(0x{tab:x}u + {idx})" if mn == "tbb" else f"RD16(0x{tab:x}u + 2 * {idx})"
            return [f"{{ uint32_t t = 0x{tab:x}u + 2 * {ld}; JUMP(t); }}"]
        raise Unsupported(mn)

    # ------------------------------------------------------------------ VFP / NEON
    def imm_carry(self, i):
        """C from a Thumb-2 modified immediate, as the flag-setting logical ops and mov/mvn set it:
        bit 31 of the constant when the encoding rotated it (imm12<11:10> != 00), else unchanged
        (None). 16-bit encodings never change C this way. An ARM-mode data-processing immediate is
        imm8 rotated right by 2 * rot4: C is bit 31 of the result when rot4 != 0 (libfios2's
        `ands r3, r2, #0xff000000` and libgxm's `bics` left C unchanged)."""
        if self.arm:
            w = int.from_bytes(i.bytes[:4], "little")
            if (w >> 25) & 7 != 0b001:   # not the data-processing (immediate) form
                return None
            rot = ((w >> 8) & 0xf) * 2
            if rot == 0:
                return None
            v = w & 0xff
            return (((v >> rot) | (v << (32 - rot))) & 0xffffffff) >> 31
        if i.size != 4:
            return None
        h1, h2 = int.from_bytes(i.bytes[0:2], "little"), int.from_bytes(i.bytes[2:4], "little")
        if (h1 >> 11) != 0b11110 or (h2 >> 15):   # not the data-processing (modified immediate) form
            return None
        imm12 = ((h1 >> 10) & 1) << 11 | ((h2 >> 12) & 7) << 8 | (h2 & 0xff)
        if (imm12 >> 10) == 0:
            return None
        rot = imm12 >> 7
        val = (0x80 | (imm12 & 0x7f))
        val = ((val >> rot) | (val << (32 - rot))) & 0xffffffff
        return val >> 31

    def vreg(self, rid):
        n = md.reg_name(rid)
        if n in GPR:   # first: sb/sl/sp/ip/fp are core registers, not VFP ones
            return ("r", GPR[n])
        if n[0] == "s":
            return ("s", int(n[1:]))
        if n[0] == "d":
            return ("d", int(n[1:]))
        if n[0] == "q":
            return ("q", int(n[1:]))
        return ("r", GPR[n]) if n in GPR else (None, None)

    def neon_lanes(self, i, mn, dt, ops, regs):
        """NEON ops on 32-bit lanes of d/q registers (lane k of dN is s[2N+k], of qN is s[4N+k]);
        every lane is computed into a temporary first, so a destination can also be a source"""
        if not regs or not regs[0] or not regs[0][0] or regs[0][0] not in "dq":
            if mn == "vmov" and dt == "32" and regs and regs[0] and regs[0][0] == "r" and len(regs) > 1 \
                    and regs[1] and regs[1][0] == "d":   # vmov.32 rT, dN[x]
                x = max(ops[1].vector_index, 0)
                return [f"c->r[{regs[0][1]}] = c->vfp.s[{2 * regs[1][1] + x}];"]
            return None
        q = regs[0][0] == "q"
        n = 4 if q else 2
        def dbytes(r):   # (first d index, d count) of a d or q register
            return (2 * r[1], 2) if r[0] == "q" else (r[1], 1)
        if mn == "vext" and len(ops) == 4 and ops[3].type == ARM_OP_IMM:
            # bytes of N (low) then M (high), from element imm: the register's width of them
            (dn, cnt), (an, _), (bn, _) = dbytes(regs[0]), dbytes(regs[1]), dbytes(regs[2])
            es = int(dt) // 8 if dt.isdigit() else 1
            nb = 8 * cnt
            return [f"{{ uint8_t t[32]; memcpy(t, &c->vfp.d[{an}], {nb}); memcpy(t + {nb}, &c->vfp.d[{bn}], {nb});"
                    f" memcpy(&c->vfp.d[{dn}], t + {ops[3].imm * es}, {nb}); }}"]
        if mn in ("vtbl", "vtbx") and dt == "8":
            # byte table lookup over 1..4 d registers: an index past the table gives 0 (vtbl) or
            # leaves the destination byte as it was (vtbx)
            dd = regs[0][1]
            tab = [r[1] for r in regs[1:-1]]
            ix = regs[-1][1]
            n = 8 * len(tab)
            loads = " ".join(f"memcpy(t + {8 * k}, &c->vfp.d[{d}], 8);" for k, d in enumerate(tab))
            miss = f"o[k]" if mn == "vtbx" else "0"
            return [f"{{ uint8_t t[32], ix[8], o[8]; {loads} memcpy(ix, &c->vfp.d[{ix}], 8); memcpy(o, &c->vfp.d[{dd}], 8);"
                    f" for (int k = 0; k < 8; k++) o[k] = ix[k] < {n} ? t[ix[k]] : {miss}; memcpy(&c->vfp.d[{dd}], o, 8); }}"]
        if mn == "vtrn" and dt == "32" and len(regs) == 2 and regs[0][0] == regs[1][0]:
            (a0, cnt), (b0, _) = dbytes(regs[0]), dbytes(regs[1])
            out = []
            for k in range(cnt):   # per d: swap D[1] with M[0]
                out.append(f"{{ uint32_t t = c->vfp.s[{2 * (a0 + k) + 1}]; c->vfp.s[{2 * (a0 + k) + 1}] = c->vfp.s[{2 * (b0 + k)}]; c->vfp.s[{2 * (b0 + k)}] = t; }}")
            return [" ".join(out)]
        if mn == "vdup" and dt in ("8", "16") and regs[1] and regs[1][0] in "rd":
            # the low element of a core register, or element x of dM, into every element
            (dn, cnt) = dbytes(regs[0])
            es = int(dt) // 8
            mask = (1 << (8 * es)) - 1
            rep_ = sum(1 << (8 * es * k) for k in range(8 // es))
            if regs[1][0] == "r":
                src = f"(c->r[{regs[1][1]}] & 0x{mask:x}u)"
            else:
                x = max(ops[1].vector_index, 0)
                src = f"((c->vfp.d[{regs[1][1]}] >> {8 * es * x}) & 0x{mask:x}u)"
            return [f"{{ uint64_t t = (uint64_t){src} * 0x{rep_:x}ull; " + " ".join(f"c->vfp.d[{dn + k}] = t;" for k in range(cnt)) + " }"]
        if mn in ("vsli", "vsri") and dt == "64" and len(ops) == 3 and ops[2].type == ARM_OP_IMM:
            (dn, cnt), (mn_, _) = dbytes(regs[0]), dbytes(regs[1])
            n = ops[2].imm
            out = []
            for k in range(cnt):
                d, m = dn + k, mn_ + k
                if mn == "vsli":
                    keep = (1 << n) - 1
                    out.append(f"c->vfp.d[{d}] = (c->vfp.d[{m}] << {n}) | (c->vfp.d[{d}] & 0x{keep:x}ull);" if n < 64 else "")
                else:
                    keep = ~((0xffffffffffffffff) >> n) & 0xffffffffffffffff if n < 64 else 0xffffffffffffffff
                    out.append(f"c->vfp.d[{d}] = {f'(c->vfp.d[{m}] >> {n}) | ' if n < 64 else ''}(c->vfp.d[{d}] & 0x{keep:x}ull);")
            return [" ".join(out)]
        if mn in ("vcnt", "vadd", "vsub") and dt in ("8", "i8") and len(ops) >= 2:
            (dn, cnt) = dbytes(regs[0]); (an, _) = dbytes(regs[1])
            bn = dbytes(regs[2])[0] if len(ops) == 3 else 0
            op = {"vcnt": "(uint8_t)__builtin_popcount(a[k])", "vadd": "(uint8_t)(a[k] + b[k])", "vsub": "(uint8_t)(a[k] - b[k])"}[mn]
            return [f"{{ uint8_t a[16], b[16], o[16]; memcpy(a, &c->vfp.d[{an}], {8 * cnt}); memcpy(b, &c->vfp.d[{bn}], {8 * cnt});"
                    f" for (int k = 0; k < {8 * cnt}; k++) o[k] = {op}; memcpy(&c->vfp.d[{dn}], o, {8 * cnt}); }}"]
        if mn == "vswp" and len(regs) == 2:
            (a0, cnt), (b0, _) = dbytes(regs[0]), dbytes(regs[1])
            return [" ".join(f"{{ uint64_t t = c->vfp.d[{a0 + k}]; c->vfp.d[{a0 + k}] = c->vfp.d[{b0 + k}]; c->vfp.d[{b0 + k}] = t; }}" for k in range(cnt))]
        if mn == "vrev64" and dt == "32":
            (a0, cnt), (b0, _) = dbytes(regs[0]), dbytes(regs[1])
            return [" ".join(f"c->vfp.d[{a0 + k}] = (c->vfp.d[{b0 + k}] >> 32) | (c->vfp.d[{b0 + k}] << 32);" for k in range(cnt))]
        if mn in ("vrev16", "vrev32", "vrev64") and dt in ("8", "16", "32") and int(dt) < int(mn[4:]) and len(regs) == 2:
            # the elements of each 16/32/64-bit container in reverse order (np_matching2's and libssl's AES and hashes)
            (a0, cnt), (b0, _) = dbytes(regs[0]), dbytes(regs[1])
            cont, es, nb = int(mn[4:]) // 8, int(dt) // 8, 8 * cnt
            return [f"{{ uint8_t t[16], o[16]; memcpy(t, &c->vfp.d[{b0}], {nb}); for (int k = 0; k < {nb}; k += {cont}) for (int e = 0; e < {cont // es}; e++)"
                    f" memcpy(o + k + e * {es}, t + k + ({cont // es - 1} - e) * {es}, {es}); memcpy(&c->vfp.d[{a0}], o, {nb}); }}"]
        if mn == "vuzp" and dt in ("8", "16", "32") and len(regs) == 2 and regs[0][0] == regs[1][0]:
            # de-interleave: of the elements of dD then dM (or qD, qM), the even ones into D and the odd ones into M
            (a0, cnt), (b0, _) = dbytes(regs[0]), dbytes(regs[1])
            es, nb = int(dt) // 8, 8 * cnt
            return [f"{{ uint8_t x[32], o[32]; memcpy(x, &c->vfp.d[{a0}], {nb}); memcpy(x + {nb}, &c->vfp.d[{b0}], {nb});"
                    f" for (int e = 0; e < {nb // es}; e++) {{ memcpy(o + e * {es}, x + 2 * e * {es}, {es}); memcpy(o + {nb} + e * {es}, x + (2 * e + 1) * {es}, {es}); }}"
                    f" memcpy(&c->vfp.d[{a0}], o, {nb}); memcpy(&c->vfp.d[{b0}], o + {nb}, {nb}); }}"]
        if mn in ("vmull", "vmul") and dt == "p8" and len(regs) == 3:
            # polynomial (carry-less) multiply of each byte pair over GF(2): vmull keeps the 16-bit product, vmul its low byte
            # (np_matching2's and libssl's AES: the inverse MixColumns)
            (an, cnt), (bn, _) = dbytes(regs[1]), dbytes(regs[2])
            if mn == "vmull":
                q0 = 2 * regs[0][1]
                return [f"{{ uint8_t x[8], y[8]; uint16_t o[8]; memcpy(x, &c->vfp.d[{an}], 8); memcpy(y, &c->vfp.d[{bn}], 8);"
                        f" for (int k = 0; k < 8; k++) {{ uint16_t r = 0; for (int j = 0; j < 8; j++) if (y[k] >> j & 1) r ^= (uint16_t)(x[k] << j); o[k] = r; }}"
                        f" memcpy(&c->vfp.d[{q0}], o, 16); }}"]
            (dn, _) = dbytes(regs[0])
            return [f"{{ uint8_t x[16], y[16], o[16]; memcpy(x, &c->vfp.d[{an}], {8 * cnt}); memcpy(y, &c->vfp.d[{bn}], {8 * cnt});"
                    f" for (int k = 0; k < {8 * cnt}; k++) {{ uint8_t r = 0; for (int j = 0; j < 8; j++) if (y[k] >> j & 1) r ^= (uint8_t)(x[k] << j); o[k] = r; }}"
                    f" memcpy(&c->vfp.d[{dn}], o, {8 * cnt}); }}"]
        if mn in ("vadd", "vsub") and dt == "i64" and len(regs) == 3 and all(r and r[0] in "dq" for r in regs):
            # 64-bit lanes (libssl's SHA-512)
            (dn, cnt), (an, _), (bn, _) = dbytes(regs[0]), dbytes(regs[1]), dbytes(regs[2])
            op = "+" if mn == "vadd" else "-"
            return ["{ " + " ".join(f"uint64_t t{k} = c->vfp.d[{an + k}] {op} c->vfp.d[{bn + k}];" for k in range(cnt)) + " " + " ".join(f"c->vfp.d[{dn + k}] = t{k};" for k in range(cnt)) + " }"]
        if mn == "vmovn" and dt in ("i16", "i32") and regs[0][0] == "d" and regs[1][0] == "q":
            es = int(dt[1:]) // 8   # source element size; the result keeps its low half
            ty, oty = ("uint16_t", "uint8_t") if es == 2 else ("uint32_t", "uint16_t")
            d, q0 = regs[0][1], 2 * regs[1][1]
            return [f"{{ {ty} a[{16 // es}]; {oty} o[{16 // es}]; memcpy(a, &c->vfp.d[{q0}], 16); for (int k = 0; k < {16 // es}; k++) o[k] = ({oty})a[k];"
                    f" memcpy(&c->vfp.d[{d}], o, 8); }}"]
        if mn == "vmovl" and dt in ("u8", "s8", "u16", "s16", "u32", "s32") and regs[0][0] == "q" and regs[1][0] == "d":
            es = int(dt[1:]) // 8
            ty, oty = {("u", 1): ("uint8_t", "uint16_t"), ("s", 1): ("int8_t", "int16_t"), ("u", 2): ("uint16_t", "uint32_t"),
                       ("s", 2): ("int16_t", "int32_t"), ("u", 4): ("uint32_t", "uint64_t"), ("s", 4): ("int32_t", "int64_t")}[(dt[0], es)]
            q0, d = 2 * regs[0][1], regs[1][1]
            return [f"{{ {ty} a[{8 // es}]; {oty} o[{8 // es}]; memcpy(a, &c->vfp.d[{d}], 8); for (int k = 0; k < {8 // es}; k++) o[k] = ({oty})a[k];"
                    f" memcpy(&c->vfp.d[{q0}], o, 16); }}"]
        if mn == "vmull" and dt in ("u8", "s8", "u16", "s16", "u32", "s32") and regs[0][0] == "q" and regs[1][0] == "d" and regs[2][0] == "d":
            # widening multiply: each element of dN times dM into twice the width (Killzone's blend/skinning code)
            es = int(dt[1:]) // 8
            ty, oty = {("u", 1): ("uint8_t", "uint16_t"), ("s", 1): ("int8_t", "int16_t"), ("u", 2): ("uint16_t", "uint32_t"),
                       ("s", 2): ("int16_t", "int32_t"), ("u", 4): ("uint32_t", "uint64_t"), ("s", 4): ("int32_t", "int64_t")}[(dt[0], es)]
            q0, a, b = 2 * regs[0][1], regs[1][1], regs[2][1]
            return [f"{{ {ty} x[{8 // es}], y[{8 // es}]; {oty} o[{8 // es}]; memcpy(x, &c->vfp.d[{a}], 8); memcpy(y, &c->vfp.d[{b}], 8);"
                    f" for (int k = 0; k < {8 // es}; k++) o[k] = ({oty})x[k] * ({oty})y[k]; memcpy(&c->vfp.d[{q0}], o, 16); }}"]
        if mn == "vshrn" and dt in ("i16", "i32", "i64") and regs[0][0] == "d" and regs[1][0] == "q" and len(ops) == 3 and ops[2].type == ARM_OP_IMM:
            # narrowing shift right: each element of qM shifted right by the immediate (1..half width), the low half kept
            es = int(dt[1:]) // 8; n = ops[2].imm
            ty, oty = {2: ("uint16_t", "uint8_t"), 4: ("uint32_t", "uint16_t"), 8: ("uint64_t", "uint32_t")}[es]
            d, q0 = regs[0][1], 2 * regs[1][1]
            return [f"{{ {ty} a[{16 // es}]; {oty} o[{16 // es}]; memcpy(a, &c->vfp.d[{q0}], 16); for (int k = 0; k < {16 // es}; k++) o[k] = ({oty})(a[k] >> {n});"
                    f" memcpy(&c->vfp.d[{d}], o, 8); }}"]
        if mn == "vmovn" and dt == "i64" and regs[0][0] == "d" and regs[1][0] == "q":
            d, q0 = regs[0][1], 2 * regs[1][1]
            return [f"c->vfp.d[{d}] = (uint64_t)(uint32_t)c->vfp.d[{q0 + 1}] << 32 | (uint32_t)c->vfp.d[{q0}];"]
        if mn == "vpaddl" and dt in ("u8", "u16", "s8", "s16"):
            (dn, cnt), (an, _) = dbytes(regs[0]), dbytes(regs[1])
            es = int(dt[1:]) // 8
            ty, oty = {("u", 1): ("uint8_t", "uint16_t"), ("s", 1): ("int8_t", "int16_t"),
                       ("u", 2): ("uint16_t", "uint32_t"), ("s", 2): ("int16_t", "int32_t")}[(dt[0], es)]
            n = 8 * cnt // es
            return [f"{{ {ty} a[16]; {oty} o[8]; memcpy(a, &c->vfp.d[{an}], {8 * cnt}); for (int k = 0; k < {n // 2}; k++) o[k] = ({oty})a[2 * k] + ({oty})a[2 * k + 1];"
                    f" memcpy(&c->vfp.d[{dn}], o, {8 * cnt}); }}"]
        # half precision, 4 lanes: vcvt.f32.f16 qD, dM / vcvt.f16.f32 dD, qM
        if mn == "vcvt" and i.mnemonic.endswith((".f32.f16", ".f16.f32")) and len(regs) == 2:
            if i.mnemonic.endswith(".f32.f16") and regs[0][0] == "q":
                dq, sd = 4 * regs[0][1], regs[1][1]
                tmp = " ".join(f"uint32_t t{k} = H2F((uint16_t)(c->vfp.d[{sd}] >> {16 * k}));" for k in range(4))
                return ["{ " + tmp + " " + " ".join(f"c->vfp.s[{dq + k}] = t{k};" for k in range(4)) + " }"]
            if i.mnemonic.endswith(".f16.f32") and regs[1][0] == "q":
                dd, sq = regs[0][1], 4 * regs[1][1]
                return [f"{{ uint64_t t = 0; for (int k = 0; k < 4; k++) t |= (uint64_t)F2H(c->vfp.s[{sq} + k]) << (16 * k); c->vfp.d[{dd}] = t; }}"]
        # by scalar: vmul/vmla/vmls.f32 qD|dD, qN|dN, dM[x] (capstone leaves the lane in the text)
        sm = re.search(r"d(\d+)\[(\d)\]$", i.op_str)
        if sm and dt == "f32" and mn in ("vmul", "vmla", "vmls") and len(ops) == 3:
            dbase = (4 if q else 2) * regs[0][1]
            abase = (4 if regs[1][0] == "q" else 2) * regs[1][1]
            sc = f"FZF(c->vfp.f[{2 * int(sm.group(1)) + int(sm.group(2))}])"
            parts = []
            for k in range(n):
                prod = f"FZF(FZF(c->vfp.f[{abase + k}]) * {sc})"
                e = prod if mn == "vmul" else f"FZF(c->vfp.f[{dbase + k}]) {'+' if mn == 'vmla' else '-'} {prod}"
                parts.append(f"uint32_t t{k} = F2B(FZF({e}));")
            return ["{ " + " ".join(parts) + " " + " ".join(f"c->vfp.s[{dbase + k}] = t{k};" for k in range(n)) + " }"]
        # narrowing high halves: v[r]addhn/v[r]subhn.i16|i32|i64 dD, qN, qM -- each element pair is
        # added (subtracted) at full width, rounded by half an output element if r, and the top half kept
        hm = re.fullmatch(r"v(r?)(add|sub)hn", mn)
        if hm and dt in ("i16", "i32", "i64") and regs[0][0] == "d" and regs[1][0] == "q" and regs[2][0] == "q":
            bits, op = int(dt[1:]), {"add": "+", "sub": "-"}[hm.group(2)]
            rnd = (1 << (bits // 2 - 1)) if hm.group(1) else 0
            d, a, b = 2 * regs[0][1], 4 * regs[1][1], 4 * regs[2][1]
            if bits == 64:
                e = lambda k: f"(uint32_t)((c->vfp.d[{a // 2 + k}] {op} c->vfp.d[{b // 2 + k}] + 0x{rnd:x}ull) >> 32)"
            elif bits == 32:
                h = lambda j: f"((c->vfp.s[{a + j}] {op} c->vfp.s[{b + j}] + 0x{rnd:x}u) >> 16)"
                e = lambda k: f"PACK16({h(2 * k)}, {h(2 * k + 1)})"
            else:
                h = lambda j: f"((uint16_t)(LANE16(c->vfp.s[{a + j // 2}], {j % 2}) {op} LANE16(c->vfp.s[{b + j // 2}], {j % 2}) + 0x{rnd:x}u) >> 8)"
                e = lambda k: f"PACK8({', '.join(h(4 * k + x) for x in range(4))})"
            return [f"{{ uint32_t t0 = {e(0)}; uint32_t t1 = {e(1)}; c->vfp.s[{d}] = t0; c->vfp.s[{d + 1}] = t1; }}"]
        # widening: vaddl/vsubl.{u,s}{8,16,32} qD, dN, dM (both operands widened) and vaddw/vsubw qD, qN, dM
        wm = re.fullmatch(r"v(add|sub)([lw])", mn)
        if wm and re.fullmatch(r"[us](8|16|32)", dt) and regs[0][0] == "q" and regs[2][0] == "d" and regs[1][0] == ("d" if wm.group(2) == "l" else "q"):
            bits, sg, op = int(dt[1:]), dt[0] == "s", {"add": "+", "sub": "-"}[wm.group(1)]
            es = bits // 8
            D, a, b = 4 * regs[0][1], regs[1][1], regs[2][1]
            def narrow(d, j):   # element j of dN|dM widened to 32 (sign- or zero-extended)
                w = f"c->vfp.s[{2 * d + j // (4 // es)}]"
                raw = {1: f"LANE8({w}, {j % 4})", 2: f"LANE16({w}, {j % 2})", 4: w}[es]
                return f"(uint32_t)(int{bits}_t){raw}" if sg else raw
            def wide(j):   # element j of qN at twice the width (vaddw's first operand), as uint32 or uint64
                if es == 4: return f"c->vfp.d[{2 * a + j}]"
                return f"LANE16(c->vfp.s[{4 * a + j // 2}], {j % 2})" if es == 1 else f"c->vfp.s[{4 * a + j}]"
            A_ = (lambda j: narrow(a, j)) if wm.group(2) == "l" else wide
            if es == 4:   # 64-bit results, one per d of qD
                cast = "(uint64_t)(int64_t)(int32_t)" if sg else "(uint64_t)"
                A64 = (lambda j: f"{cast}c->vfp.s[{2 * a + j}]") if wm.group(2) == "l" else wide
                tmp = " ".join(f"uint64_t t{k} = {A64(k)} {op} {cast}c->vfp.s[{2 * b + k}];" for k in range(2))
                return [f"{{ {tmp} c->vfp.d[{D // 2}] = t0; c->vfp.d[{D // 2 + 1}] = t1; }}"]
            if es == 2:
                e = lambda k: f"({A_(k)} {op} {narrow(b, k)})"
            else:
                e = lambda k: f"PACK16({A_(2 * k)} {op} {narrow(b, 2 * k)}, {A_(2 * k + 1)} {op} {narrow(b, 2 * k + 1)})"
            tmp = " ".join(f"uint32_t t{k} = {e(k)};" for k in range(4))
            return [f"{{ {tmp} " + " ".join(f"c->vfp.s[{D + k}] = t{k};" for k in range(4)) + " }"]
        if any(r and r[0] and r[0] in "dq" and (r[0] == "q") != q for r in regs[1:]) and mn != "vdup":
            return None
        base = lambda r: (4 if r[0] == "q" else 2) * r[1]
        D = base(regs[0])

        def lanes(expr):   # expr(k) -> C expression of lane k (a uint32_t)
            tmp = " ".join(f"uint32_t t{k} = {expr(k)};" for k in range(n))
            put = " ".join(f"c->vfp.s[{D + k}] = t{k};" for k in range(n))
            return [f"{{ {tmp} {put} }}"]
        A = lambda k: f"c->vfp.s[{base(regs[1]) + k}]"
        AF = lambda k: f"FZF(c->vfp.f[{base(regs[1]) + k}])"
        imm0 = len(ops) == 3 and ops[2].type in (ARM_OP_IMM, ARM_OP_FP)
        if len(ops) == 3 and not imm0 and regs[2] and regs[2][0] and regs[2][0] in "dq":
            B = lambda k: f"c->vfp.s[{base(regs[2]) + k}]"
            BF = lambda k: f"FZF(c->vfp.f[{base(regs[2]) + k}])"
        else:
            B, BF = (lambda k: "0u"), (lambda k: "0.0f")
        if mn in ("vpadd", "vpmax", "vpmin") and len(ops) == 3 and not q and dt in ("f32", "i32"):
            # pairwise (d only): lane 0 from N's two lanes, lane 1 from M's
            if dt == "f32":
                e = {"vpadd": lambda x, y: f"F2B(FZF(FZF(c->vfp.f[{x}]) + FZF(c->vfp.f[{y}])))",
                     "vpmax": lambda x, y: f"FMAX_F32(c->vfp.s[{x}], c->vfp.s[{y}])",
                     "vpmin": lambda x, y: f"FMIN_F32(c->vfp.s[{x}], c->vfp.s[{y}])"}[mn]
            elif mn == "vpadd":
                e = lambda x, y: f"c->vfp.s[{x}] + c->vfp.s[{y}]"
            else:
                op = ">" if mn == "vpmax" else "<"
                e = lambda x, y: f"((int32_t)c->vfp.s[{x}] {op} (int32_t)c->vfp.s[{y}] ? c->vfp.s[{x}] : c->vfp.s[{y}])"
            a0, b0 = base(regs[1]), base(regs[2])
            return lanes(lambda k: e(a0, a0 + 1) if k == 0 else e(b0, b0 + 1))
        if mn in ("vsli", "vsri") and dt == "32" and ops[2].type == ARM_OP_IMM:
            # sh, not n: lanes() reads the lane count n when it runs. Named n, the shift became the
            # lane count -- #1 left lane 1 stale, #31 wrote 31 lanes over the neighbouring registers
            # (Uncharted's vsli.32 d16, d17, #1 and d5, d8, #31; insnsweep.py found them)
            sh = ops[2].imm
            if mn == "vsli":
                keep = (1 << sh) - 1
                return lanes(lambda k: f"(({A(k)} << {sh}) | (c->vfp.s[{D + k}] & 0x{keep:x}u))" if sh < 32 else f"c->vfp.s[{D + k}]")
            keep = (~(0xffffffff >> sh)) & 0xffffffff if sh < 32 else 0xffffffff
            return lanes(lambda k: f"(({A(k)} >> {sh}) | (c->vfp.s[{D + k}] & 0x{keep:x}u))" if sh < 32 else f"c->vfp.s[{D + k}]")
        if mn == "vmul" and dt == "i32" and len(ops) == 3 and not imm0:
            return lanes(lambda k: f"{A(k)} * {B(k)}")
        if mn == "vrsqrte" and dt == "f32":
            return lanes(lambda k: f"RSQRTE_F32({A(k)})")
        if mn == "vrsqrts" and dt == "f32":
            return lanes(lambda k: f"F2B(RSQRTS_F32(c->vfp.f[{base(regs[1]) + k}], c->vfp.f[{base(regs[2]) + k}]))")
        if mn == "vmvn" and len(ops) == 2 and ops[1].type == ARM_OP_REG:
            return lanes(lambda k: f"~{A(k)}")
        if mn == "vclz" and dt == "i32":
            return lanes(lambda k: f"CLZ({A(k)})")
        if mn == "vtst" and dt == "32" and len(ops) == 3:
            return lanes(lambda k: f"(({A(k)} & {B(k)}) ? 0xffffffffu : 0u)")
        if mn == "vshl" and dt in ("u32", "s32", "i32") and len(ops) == 3 and not imm0:
            # by register: the signed low byte of each lane of the shift operand; negative shifts right
            sr = "(uint32_t)((int32_t)" if dt == "s32" else "("
            return lanes(lambda k: f"({{ int8_t sh = (int8_t){B(k)}; sh >= 32 || sh <= -32 ? {'((int32_t)' + A(k) + ' < 0 && sh < 0 ? 0xffffffffu : 0u)' if dt == 's32' else '0u'}"
                                   f" : sh >= 0 ? {A(k)} << sh : {sr}{A(k)} >> -sh); }})")
        cmp = {"vcgt": ">", "vcge": ">=", "vclt": "<", "vcle": "<=", "veq": "==", "vceq": "=="}
        if mn in cmp and (len(ops) == 3) and dt == "f32":
            op = cmp[mn]
            return lanes(lambda k: f"({AF(k)} {op} {BF(k)} ? 0xffffffffu : 0u)")
        # integer elements of 8, 16 or 32 bits (signed, unsigned or either, as dt says): every element
        # of the result is computed as a uint32_t (narrow sources sign- or zero-extended, LANE8/LANE16)
        # and the elements of each 32-bit word packed back (PACK8/PACK16, which truncate), so the
        # 32-bit-lane emitter above carries the narrow forms too
        em = re.fullmatch(r"([usi]?)(8|16|32)", dt)
        if em and regs[1] and regs[1][0] in "dq" and "[" not in i.op_str:   # (by-scalar forms keep the lane in the text)
            sg, bits = em.group(1) == "s", int(em.group(2))
            es, per = bits // 8, 32 // bits
            mask = (1 << bits) - 1
            def E(r, j):   # element j of register r as a uint32_t (sign-extended when signed)
                w = f"c->vfp.s[{base(r) + j // per}]"
                raw = {1: f"LANE8({w}, {j % 4})", 2: f"LANE16({w}, {j % 2})", 4: w}[es]
                return f"(uint32_t)(int{bits}_t){raw}" if sg and es < 4 else raw
            def pack(expr):   # expr(j) -> element j of the result; words of them into the destination
                if es == 4: return lanes(expr)
                if es == 2: return lanes(lambda k: f"PACK16({expr(2 * k)}, {expr(2 * k + 1)})")
                return lanes(lambda k: f"PACK8({', '.join(expr(4 * k + x) for x in range(4))})")
            sa = (lambda j: f"(int32_t){E(regs[1], j)}") if sg else (lambda j: E(regs[1], j))
            two = len(ops) == 3 and not imm0 and regs[2] and regs[2][0] in "dq"
            sb = ((lambda j: f"(int32_t){E(regs[2], j)}") if sg else (lambda j: E(regs[2], j))) if two else (lambda j: "0")
            sd = (lambda j: f"(int32_t){E(regs[0], j)}") if sg else (lambda j: E(regs[0], j))
            wide = "(int64_t)" if sg else "(uint64_t)"   # a sum that may need 33 bits
            if mn in cmp and len(ops) == 3 and (two or (imm0 and ops[2].imm == 0)):   # also the "#0" forms
                return pack(lambda j: f"({sa(j)} {cmp[mn]} {sb(j)} ? 0x{mask:x}u : 0u)")
            if mn in ("vmax", "vmin") and two:
                return pack(lambda j: f"({sa(j)} {'>' if mn == 'vmax' else '<'} {sb(j)} ? {E(regs[1], j)} : {E(regs[2], j)})")
            if mn in ("vpmax", "vpmin") and two and not q:   # pairwise: N's pairs then M's
                op = ">" if mn == "vpmax" else "<"
                h = 4 // es   # elements per d
                pr = lambda j: ((regs[1], 2 * j) if j < h else (regs[2], 2 * (j - h)))
                def e(j):
                    r, x = pr(j)
                    c0, c1 = (f"(int32_t){E(r, x)}", f"(int32_t){E(r, x + 1)}") if sg else (E(r, x), E(r, x + 1))
                    return f"({c0} {op} {c1} ? {E(r, x)} : {E(r, x + 1)})"
                return pack(e)
            if mn == "vpadd" and two and not q:
                h = 4 // es
                pr = lambda j: ((regs[1], 2 * j) if j < h else (regs[2], 2 * (j - h)))
                return pack(lambda j: f"({E(pr(j)[0], pr(j)[1])} + {E(pr(j)[0], pr(j)[1] + 1)})")
            if mn in ("vadd", "vsub", "vmul") and two:
                return pack(lambda j: f"({E(regs[1], j)} {dict(vadd='+', vsub='-', vmul='*')[mn]} {E(regs[2], j)})")
            if mn in ("vmla", "vmls") and two:   # d +/- a * b, modulo the element
                return pack(lambda j: f"({E(regs[0], j)} {'+' if mn == 'vmla' else '-'} {E(regs[1], j)} * {E(regs[2], j)})")
            if mn in ("vhadd", "vhsub") and two:   # halving: the exact sum or difference, then floor(x / 2)
                op = "+" if mn == "vhadd" else "-"
                return pack(lambda j: f"(uint32_t)(({wide}{sa(j)} {op} {wide}{sb(j)}) >> 1)")
            if mn in ("vqadd", "vqsub") and two:   # saturating to the element's range
                op = "+" if mn == "vqadd" else "-"
                sat = "SATS" if sg else "SATU"
                return pack(lambda j: f"(uint32_t){sat}((int64_t){sa(j)} {op} (int64_t){sb(j)}, {bits})")
            if mn == "vtst" and two:
                return pack(lambda j: f"(({E(regs[1], j)} & {E(regs[2], j)}) ? 0x{mask:x}u : 0u)")
            if mn == "vneg" and len(ops) == 2 and sg:
                return pack(lambda j: f"(uint32_t)(0 - {sa(j)})")
            if mn == "vabs" and len(ops) == 2 and sg:
                return pack(lambda j: f"(uint32_t)({sa(j)} < 0 ? 0 - {sa(j)} : {sa(j)})")
            if mn in ("vshl", "vqshl") and two:   # by register: the signed low byte of M's element
                if mn == "vshl":
                    f_ = f"SHLR_{'S' if sg else 'U'}"
                    return pack(lambda j: f"{f_}({sa(j)}, (int8_t){E(regs[2], j)}, {bits})")
                return pack(lambda j: f"(uint32_t)QSHL{'S' if sg else 'U'}({sa(j)}, (int8_t){E(regs[2], j)}, {bits})")
            if mn in ("vshr", "vshl", "vsra", "vrshr", "vrsra", "vqshl") and len(ops) == 3 and ops[2].type == ARM_OP_IMM:
                n_ = ops[2].imm   # vshl 0..bits-1, the right shifts 1..bits
                if mn == "vshl":
                    return pack(lambda j: f"({E(regs[1], j)} << {n_})" if n_ < bits else "0u")
                if mn == "vqshl":
                    return pack(lambda j: f"(uint32_t)QSHL{'S' if sg else 'U'}({sa(j)}, {n_}, {bits})")
                rnd = mn in ("vrshr", "vrsra")   # rounding: plus the last bit shifted out (ARM's RoundingShiftRight)
                if sg:   # arithmetic; a full 32-bit shift leaves the sign (rounded: 0)
                    if not rnd: shr = lambda j: f"(uint32_t)({sa(j)} >> {min(n_, 31)})"
                    else: shr = lambda j: f"(uint32_t)(({sa(j)} >> {n_}) + (({sa(j)} >> {n_ - 1}) & 1))" if n_ < 32 else "0u"
                else:
                    if not rnd: shr = lambda j: f"({E(regs[1], j)} >> {n_})" if n_ < 32 else "0u"
                    else: shr = lambda j: f"(({E(regs[1], j)} >> {n_}) + (({E(regs[1], j)} >> {n_ - 1}) & 1))" if n_ < 32 else f"(({E(regs[1], j)} >> 31) & 1)"
                if mn in ("vsra", "vrsra"):
                    return pack(lambda j: f"({E(regs[0], j)} + {shr(j)})")
                return pack(shr)
        if mn in ("vacgt", "vacge") and dt == "f32":
            op = ">" if mn == "vacgt" else ">="
            return lanes(lambda k: f"(fabsf({AF(k)}) {op} fabsf({BF(k)}) ? 0xffffffffu : 0u)")
        # float lanes: NEON arithmetic flushes denormal inputs and results, rounds to nearest, and
        # multiply-accumulate rounds twice (not fused)
        if dt == "f32":
            fz = lambda e: f"F2B(FZF({e}))"
            if mn in ("vadd", "vsub", "vmul") and len(ops) == 3 and not imm0:
                op = {"vadd": "+", "vsub": "-", "vmul": "*"}[mn]
                return lanes(lambda k: fz(f"{AF(k)} {op} {BF(k)}"))
            if mn in ("vmla", "vmls") and len(ops) == 3 and not imm0:
                op = "+" if mn == "vmla" else "-"
                DF = lambda k: f"FZF(c->vfp.f[{D + k}])"
                return lanes(lambda k: fz(f"{DF(k)} {op} FZF({AF(k)} * {BF(k)})"))
            if mn in ("vmax", "vmin") and len(ops) == 3 and not imm0:
                return lanes(lambda k: f"{'FMAX_F32' if mn == 'vmax' else 'FMIN_F32'}({A(k)}, {B(k)})")
            if mn in ("vneg", "vabs") and len(ops) == 2:
                return lanes(lambda k: f"{A(k)} ^ 0x80000000u" if mn == "vneg" else f"{A(k)} & 0x7fffffffu")
            if mn == "vmov" and len(ops) == 2 and ops[1].type == ARM_OP_FP:
                import struct as _st
                bits = _st.unpack("<I", _st.pack("<f", ops[1].fp))[0]
                return lanes(lambda k: f"0x{bits:x}u")
        if mn == "vcvt" and len(ops) >= 2:
            # vector conversions, per 32-bit lane: f32 <- s32/u32 (round to nearest), s32/u32 <- f32
            # (toward zero, saturating); "#n" fixed-point forms scale by 2^n; NEON flushes denormals
            to, frm = i.mnemonic.split(".")[1:3]
            fb = ops[2].imm if len(ops) == 3 and ops[2].type == ARM_OP_IMM else 0
            sc = f" * {1.0 / (1 << fb)!r}f" if fb else ""
            if to == "f32" and frm in ("s32", "u32"):
                cast = "(int32_t)" if frm == "s32" else ""
                return lanes(lambda k: f"F2B(FZF((float)({cast}{A(k)}){sc}))")
            if frm == "f32" and to in ("s32", "u32"):
                mul = f" * {float(1 << fb)!r}" if fb else ""
                conv = "(uint32_t)F2S" if to == "s32" else "F2U"
                return lanes(lambda k: f"{conv}((double)FZF(c->vfp.f[{base(regs[1]) + k}]){mul})")
        if mn in ("vbsl", "vbit", "vbif"):
            d = lambda k: f"c->vfp.s[{D + k}]"
            e = {"vbsl": lambda k: f"({d(k)} & {A(k)}) | (~{d(k)} & {B(k)})",
                 "vbit": lambda k: f"({A(k)} & {B(k)}) | ({d(k)} & ~{B(k)})",
                 "vbif": lambda k: f"({d(k)} & {B(k)}) | ({A(k)} & ~{B(k)})"}[mn]
            return lanes(e)
        if mn == "vdup" and dt == "32" and regs[1] and regs[1][0] == "d":
            x = max(ops[1].vector_index, 0)
            return lanes(lambda k: f"c->vfp.s[{2 * regs[1][1] + x}]")
        if mn == "vrecpe" and dt == "f32":
            return lanes(lambda k: f"RECPE_F32({A(k)})")
        if mn == "vrecps" and dt == "f32":
            return lanes(lambda k: f"F2B(RECPS_F32(c->vfp.f[{base(regs[1]) + k}], c->vfp.f[{base(regs[2]) + k}]))")
        if mn in ("vadd", "vsub") and dt == "i32":
            return lanes(lambda k: f"{A(k)} {'+' if mn == 'vadd' else '-'} {B(k)}")
        if mn in ("vshr", "vshl") and dt in ("u32", "s32", "i32") and ops[2].type == ARM_OP_IMM:
            sh = ops[2].imm
            if mn == "vshl":
                return lanes(lambda k: f"({A(k)} << {sh})" if sh < 32 else "0u")
            if dt == "s32":
                return lanes(lambda k: f"(uint32_t)((int32_t){A(k)} >> {min(sh, 31)})")
            return lanes(lambda k: f"({A(k)} >> {sh})" if sh < 32 else "0u")
        if mn in ("vmvn",) and dt == "i32" and ops[1].type == ARM_OP_IMM:
            return lanes(lambda k: f"0x{~ops[1].imm & 0xffffffff:x}u")
        return None

    def fval(self, rid):
        k, n = self.vreg(rid)
        return f"c->vfp.f[{n}]" if k == "s" else f"c->vfp.fd[{n}]"

    def vfp(self, i, mn, dt, ops):
        regs = [self.vreg(o.reg) if o.type == ARM_OP_REG else None for o in ops]
        lane = self.neon_lanes(i, mn, dt, ops, regs)
        if lane is not None:
            return lane
        if mn in ("vldr", "vstr"):
            k, n = regs[0]
            a = self.addr(ops[1], i)
            if k == "s":
                return [f"c->vfp.s[{n}] = RD32({a});" if mn == "vldr" else f"WR32({a}, c->vfp.s[{n}]);"]
            return [f"c->vfp.d[{n}] = RD64({a});" if mn == "vldr" else f"WR64({a}, c->vfp.d[{n}]);"]
        if mn in ("vpush", "vpop", "vldmia", "vstmia", "vldmdb", "vstmdb", "vldm", "vstm"):
            if mn in ("vpush", "vpop"):
                base, vr, wb, before, load = 13, regs, True, mn == "vpush", mn == "vpop"
            else:
                base, vr, wb = GPR[md.reg_name(ops[0].reg)], regs[1:], i.writeback
                before, load = mn.endswith("db"), mn.startswith("vld")
            sz = 4 if vr[0][0] == "s" else 8
            tot = sz * len(vr)
            out = [f"{{ uint32_t ea = c->r[{base}]{f' - {tot}' if before else ''};"]
            for k, (kind, n) in enumerate(vr):
                f = "s" if kind == "s" else "d"
                rw = ("RD32" if sz == 4 else "RD64") if load else ("WR32" if sz == 4 else "WR64")
                out.append(f" c->vfp.{f}[{n}] = {rw}(ea + {sz * k});" if load else f" {rw}(ea + {sz * k}, c->vfp.{f}[{n}]);")
            if wb:
                out.append(f" c->r[{base}] {'-' if before else '+'}= {tot};")
            out.append(" }")
            return ["".join(out)]
        if mn == "vmov" and len(ops) == 2 and regs[0] and regs[0][0] in "dq" and ops[1].type == ARM_OP_IMM:
            # vmov.i32/i16/i8/i64 dN|qN, #imm: the immediate replicated into every lane
            bits = int(dt[1:]) if dt[1:].isdigit() else 32
            if bits < 64:
                lane = ops[1].imm & ((1 << bits) - 1)
            else:   # capstone sign-extends a 64-bit immediate from 32 bits: the text has the real value
                lm = re.search(r"#(0x[0-9a-f]+|\d+)$", i.op_str)
                lane = int(lm.group(1), 0) & 0xffffffffffffffff if lm else ops[1].imm & 0xffffffffffffffff
            rep64 = 0
            for k in range(64 // bits):
                rep64 |= lane << (k * bits)
            nd = [2 * regs[0][1], 2 * regs[0][1] + 1] if regs[0][0] == "q" else [regs[0][1]]
            return [" ".join(f"c->vfp.d[{x}] = 0x{rep64:x}ull;" for x in nd)]
        if mn == "vmov" and len(ops) == 2 and regs[0] and regs[0][0] == "d" and regs[1] and regs[1][0] == "r":
            # vmov.32 dN[x], rT (capstone gives the lane in the operand's vector_index)
            lane = ops[0].vector_index if ops[0].vector_index >= 0 else 0
            sh = 32 * lane
            return [f"c->vfp.d[{regs[0][1]}] = (c->vfp.d[{regs[0][1]}] & ~(0xffffffffull << {sh})) | ((uint64_t)c->r[{regs[1][1]}] << {sh});"]
        if mn == "vmov" and dt in ("u8", "s8", "u16", "s16") and len(ops) == 2 and regs[0] and regs[0][0] == "r" and regs[1] and regs[1][0] == "d" and ops[1].vector_index >= 0:
            # vmov.u16 rT, dN[x]: one element of a D register to a core register, zero- or sign-extended
            bits = int(dt[1:]); sh = bits * ops[1].vector_index
            raw = f"(uint32_t)((c->vfp.d[{regs[1][1]}] >> {sh}) & 0x{(1 << bits) - 1:x}u)"
            return [f"c->r[{regs[0][1]}] = " + (f"(uint32_t)(int32_t)(int{bits}_t){raw};" if dt[0] == "s" else f"{raw};")]
        if mn == "vmov":
            kinds = [r[0] if r else "imm" for r in regs]
            if kinds == ["s", "r"]:
                return [f"c->vfp.s[{regs[0][1]}] = c->r[{regs[1][1]}];"]
            if kinds == ["r", "s"]:
                return [f"c->r[{regs[0][1]}] = c->vfp.s[{regs[1][1]}];"]
            if kinds == ["d", "r", "r"]:
                return [f"c->vfp.d[{regs[0][1]}] = ((uint64_t)c->r[{regs[2][1]}] << 32) | c->r[{regs[1][1]}];"]
            if kinds == ["r", "r", "d"]:
                return [f"c->r[{regs[0][1]}] = (uint32_t)c->vfp.d[{regs[2][1]}]; c->r[{regs[1][1]}] = (uint32_t)(c->vfp.d[{regs[2][1]}] >> 32);"]
            if kinds == ["s", "s"]:
                return [f"c->vfp.s[{regs[0][1]}] = c->vfp.s[{regs[1][1]}];"]
            if kinds == ["d", "d"]:
                return [f"c->vfp.d[{regs[0][1]}] = c->vfp.d[{regs[1][1]}];"]
            if kinds == ["q", "q"]:
                a, b = regs[0][1], regs[1][1]
                return [f"c->vfp.d[{2 * a}] = c->vfp.d[{2 * b}]; c->vfp.d[{2 * a + 1}] = c->vfp.d[{2 * b + 1}];"]
            if kinds in (["s", "imm"], ["d", "imm"]) and ops[1].type == ARM_OP_FP:
                return [f"{self.fval(ops[0].reg)} = {ops[1].fp!r};"]
            raise Unsupported(f"vmov {kinds}")
        if mn in ("vadd", "vsub", "vmul", "vdiv", "vnmul") and dt in ("f32", "f64") and regs[0][0] in "sd":
            a, b = self.fval(ops[1].reg), self.fval(ops[2].reg)
            op = {"vadd": "+", "vsub": "-", "vmul": "*", "vdiv": "/", "vnmul": "*"}[mn]
            e = f"{a} {op} {b}"
            if mn == "vnmul":
                e = f"-({e})"
            return [f"{self.fval(ops[0].reg)} = {e};"]
        if mn in ("vmla", "vmls", "vnmla", "vnmls") and regs[0][0] in "sd":
            d, a, b = self.fval(ops[0].reg), self.fval(ops[1].reg), self.fval(ops[2].reg)
            e = {"vmla": f"{d} + {a} * {b}", "vmls": f"{d} - {a} * {b}", "vnmla": f"-{d} - {a} * {b}",
                 "vnmls": f"-{d} + {a} * {b}"}[mn]
            return [f"{d} = {e};"]
        if mn in ("vneg", "vabs", "vsqrt") and regs[0][0] in "sd":
            a = self.fval(ops[1].reg)
            fl = "f" if regs[0][0] == "s" else ""
            e = {"vneg": f"-{a}", "vabs": f"fabs{fl}({a})", "vsqrt": f"sqrt{fl}({a})"}[mn]
            return [f"{self.fval(ops[0].reg)} = {e};"]
        if mn in ("vcmp", "vcmpe"):
            a = self.fval(ops[0].reg)
            b = self.fval(ops[1].reg) if ops[1].type == ARM_OP_REG else "0.0"
            return [f"VCMP(c, {a}, {b});"]
        if mn == "vmrs":
            if md.reg_name(ops[0].reg) in ("apsr_nzcv", "apsr"):
                return ["VMRS_APSR(c);"]
            return [f"{self.dst(ops[0].reg)} = c->fpscr;"]
        if mn == "vmsr":
            return [f"c->fpscr = {self.r(ops[1].reg, i)};"]
        if mn in ("vcvtb", "vcvtt") and len(regs) == 2 and regs[0][0] == "s" and regs[1][0] == "s":
            # half in the bottom (b) or top (t) 16 bits of a single register
            sh = 16 if mn == "vcvtt" else 0
            if i.mnemonic.endswith(".f32.f16"):
                return [f"c->vfp.s[{regs[0][1]}] = H2F((uint16_t)(c->vfp.s[{regs[1][1]}] >> {sh}));"]
            if i.mnemonic.endswith(".f16.f32"):
                return [f"c->vfp.s[{regs[0][1]}] = (c->vfp.s[{regs[0][1]}] & ~(0xffffu << {sh})) | ((uint32_t)F2H(c->vfp.s[{regs[1][1]}]) << {sh});"]
        if mn == "vcvt":
            parts = i.mnemonic.split(".")[1:]
            if len(ops) != 2:
                raise Unsupported("vcvt fixed-point")
            to, frm = parts[0], parts[1]
            d, s = regs[0], regs[1]
            if "f16" in (to, frm) or frm not in ("s32", "u32", "f32", "f64") or to not in ("s32", "u32", "f32", "f64"):
                raise Unsupported(f"vcvt {to}.{frm}")
            src = {"s32": f"(double)(int32_t)c->vfp.s[{s[1]}]", "u32": f"(double)c->vfp.s[{s[1]}]",
                   "f32": f"(double)c->vfp.f[{s[1]}]", "f64": f"c->vfp.fd[{s[1]}]"}[frm]
            if to == "s32":
                return [f"c->vfp.s[{d[1]}] = (uint32_t)F2S({src});"]
            if to == "u32":
                return [f"c->vfp.s[{d[1]}] = F2U({src});"]
            if to == "f32":
                return [f"c->vfp.f[{d[1]}] = (float)({src});"]
            if to == "f64":
                return [f"c->vfp.fd[{d[1]}] = {src};"]
        if mn in ("vorr", "vand", "veor", "vbic", "vorn") and len(regs) == 3 and all(r and r[0] in "dq" for r in regs):
            op = {"vorr": "a | b", "vand": "a & b", "veor": "a ^ b", "vbic": "a & ~b", "vorn": "a | ~b"}[mn]
            n = 2 if regs[0][0] == "q" else 1
            out = []
            for k in range(n):
                d, a, b = (r[1] * n + k for r in regs)
                out.append(f"{{ uint64_t a = c->vfp.d[{a}], b = c->vfp.d[{b}]; c->vfp.d[{d}] = {op}; }}")
            return [" ".join(out)]
        if mn == "vshl" and dt in ("i64", "u64", "s64") and regs[0][0] == "d":
            d, a = regs[0][1], regs[1][1]
            if ops[2].type == ARM_OP_IMM:
                return [f"c->vfp.d[{d}] = c->vfp.d[{a}] << {ops[2].imm};"]
            # by register: the signed low byte of the shift operand, negative shifts right
            b = regs[2][1]
            sr = "(int64_t)" if dt == "s64" else ""
            fill = "(uint64_t)((int64_t)v >> 63)" if dt == "s64" else "0"
            return [f"{{ int8_t sh = (int8_t)c->vfp.d[{b}]; uint64_t v = c->vfp.d[{a}];"
                    f" c->vfp.d[{d}] = sh >= 64 ? 0 : sh <= -64 ? {fill} : sh >= 0 ? v << sh : (uint64_t)({sr}v >> -sh); }}"]
        if mn == "vshr" and dt in ("u64", "s64") and regs[0][0] == "d" and ops[2].type == ARM_OP_IMM:
            d, a, n = regs[0][1], regs[1][1], ops[2].imm
            if dt == "s64":
                return [f"c->vfp.d[{d}] = (uint64_t)((int64_t)c->vfp.d[{a}] >> {min(n, 63)});"]
            return [f"c->vfp.d[{d}] = {'0' if n >= 64 else f'c->vfp.d[{a}] >> {n}'};"]
        if mn in ("vsra", "vrshr", "vrsra") and dt in ("u64", "s64") and ops[2].type == ARM_OP_IMM:
            # 64-bit lanes shifted right by 1..64 (vrshr/vrsra round by the last bit shifted out,
            # vsra/vrsra add to the destination), per d of a d or q register
            n, sg = ops[2].imm, dt == "s64"
            cnt = 2 if regs[0][0] == "q" else 1
            d0, a0 = (2 * regs[0][1], 2 * regs[1][1]) if cnt == 2 else (regs[0][1], regs[1][1])
            def sh(a):
                if mn == "vsra":
                    return f"(uint64_t)((int64_t)c->vfp.d[{a}] >> {min(n, 63)})" if sg else ("0" if n >= 64 else f"(c->vfp.d[{a}] >> {n})")
                return f"(uint64_t)RSHR64_S((int64_t)c->vfp.d[{a}], {n})" if sg else f"RSHR64_U(c->vfp.d[{a}], {n})"
            acc = lambda d: f"c->vfp.d[{d}] + " if mn != "vrshr" else ""
            tmp = " ".join(f"uint64_t t{k} = {acc(d0 + k)}{sh(a0 + k)};" for k in range(cnt))
            return [f"{{ {tmp} " + " ".join(f"c->vfp.d[{d0 + k}] = t{k};" for k in range(cnt)) + " }"]
        if mn == "vdup" and dt == "32" and regs[0][0] in "dq" and regs[1][0] == "r":
            nd = [2 * regs[0][1], 2 * regs[0][1] + 1] if regs[0][0] == "q" else [regs[0][1]]
            return [" ".join(f"c->vfp.d[{x}] = (uint64_t)c->r[{regs[1][1]}] * 0x100000001ull;" for x in nd)]
        if mn in ("vld2", "vst2", "vld3", "vst3", "vld4", "vst4"):
            # n-element structures, interleaved in memory (ARM ARM VLDn/VSTn): element e of every
            # register of a structure in turn, structure by structure; the register list is explicit
            # (any spacing: {d0, d2, d4, d6}); vld2/vst2 with 4 registers is two structures
            # (d/d+2, d+1/d+3). The single-lane forms {dA[x], dB[x], ...} move one element of each
            # register. Writeback: "!" advances by the bytes moved, ", rM" by rM
            nst = int(mn[-1])
            m_op = [o for o in ops if o.type == ARM_OP_MEM][0]
            ds = [self.vreg(o.reg)[1] for o in ops if o.type == ARM_OP_REG and md.reg_name(o.reg)[0] == "d"]
            es = int(dt) // 8 if dt.isdigit() else 4
            lm = re.search(r"\{d\d+\[(\d+)\]", i.op_str)
            load = mn.startswith("vld")
            if lm:
                moves = [(int(lm.group(1)), dd) for dd in ds]
            else:
                if nst == 2 and len(ds) == 4: groups = [(ds[0], ds[2]), (ds[1], ds[3])]
                elif len(ds) == nst: groups = [tuple(ds)]
                else: raise Unsupported(f"{mn} with {len(ds)} registers")
                moves = [(e, dd) for g in groups for e in range(8 // es) for dd in g]
            base = GPR[md.reg_name(m_op.mem.base)]
            out = [f"{{ uint32_t ea = c->r[{base}], at = ea;"]
            for e, dd in moves:
                pv = f"((uint8_t *)&c->vfp.d[{dd}]) + {e * es}"
                out.append(f" memcpy({pv}, G(at), {es});" if load else f" memcpy(G(at), {pv}, {es});")
                out.append(f" at += {es};")
            post = [o for o in ops if o.type == ARM_OP_REG and md.reg_name(o.reg) in GPR and o is not ops[0]]
            if i.writeback or post:
                out.append(f" c->r[{base}] = ea + {es * len(moves)};" if not post else f" c->r[{base}] = ea + {self.r(post[-1].reg, i)};")
            out.append(" }")
            return ["".join(out)]
        if mn in ("vld1", "vst1"):
            m_op = [o for o in ops if o.type == ARM_OP_MEM][0]
            vr = [self.vreg(o.reg) for o in ops if o.type == ARM_OP_REG and md.reg_name(o.reg)[0] in "dq"]
            ds = []
            for k, n in vr:
                ds += [2 * n, 2 * n + 1] if k == "q" else [n]
            base = GPR[md.reg_name(m_op.mem.base)]
            out = [f"{{ uint32_t ea = c->r[{base}];"]
            vops = [o for o in ops if o.type == ARM_OP_REG and md.reg_name(o.reg)[0] in "dq"]
            # capstone leaves vector_index at -1 for these: the lane comes from the text ({d16[1]})
            lm = re.search(r"\{[dq]\d+\[(\d+)\]", i.op_str)
            lane, alld = (int(lm.group(1)) if lm else -1), "[]" in i.op_str
            if lane >= 0 or alld:
                # one element: {dN[x]} loads/stores lane x only; {dN[]} loads it into every lane --
                # and {dN[], dN+1[]} loads that ONE element into every lane of both registers (the
                # ARM ARM's VLD1 single-element-to-all-lanes: Replicate(MemU[address, ebytes]) into
                # D[d+r] for each register, writeback by ebytes). Register k used to take the element
                # at ea + k * size: Unit 13's heading-to-stick rotation (fn_812eaf7c, 0x812eb274)
                # got the next word in d7, and NPCs whose heading went through it never walked.
                es = int(dt) // 8 if dt.isdigit() else 4
                for k, dn in enumerate(ds):
                    p_ = f"((uint8_t *)&c->vfp.d[{dn}])"
                    if alld:
                        out.append(f" {{ uint8_t e[{es}]; memcpy(e, G(ea), {es});"
                                   f" for (int j = 0; j < 8; j += {es}) memcpy({p_} + j, e, {es}); }}")
                    elif mn == "vld1":
                        out.append(f" memcpy({p_} + {lane * es}, G(ea + {es * k}), {es});")
                    else:
                        out.append(f" memcpy(G(ea + {es * k}), {p_} + {lane * es}, {es});")
                post = [o for o in ops if o.type == ARM_OP_REG and md.reg_name(o.reg) in GPR and o is not ops[0]]
                if i.writeback:   # one element transferred either way
                    out.append(f" c->r[{base}] = ea + {es if alld else es * len(ds)};" if not post else f" c->r[{base}] = ea + {self.r(post[-1].reg, i)};")
                out.append(" }")
                return ["".join(out)]
            for k, dn in enumerate(ds):
                out.append(f" c->vfp.d[{dn}] = RD64(ea + {8 * k});" if mn == "vld1" else f" WR64(ea + {8 * k}, c->vfp.d[{dn}]);")
            post = [o for o in ops if o.type == ARM_OP_REG and md.reg_name(o.reg) in GPR and o is not ops[0]]
            if i.writeback:
                out.append(f" c->r[{base}] = ea + {8 * len(ds)};" if not post else f" c->r[{base}] = ea + {self.r(post[-1].reg, i)};")
            out.append(" }")
            return ["".join(out)]
        if mn == "vrhadd" and dt in ("u8", "s8", "u16", "s16", "u32", "s32"):
            bits, sg = int(dt[1:]), dt[0] == "s"
            k = regs[0][0]
            nd = 2 if k == "q" else 1
            d0, a0, b0 = (r[1] * (2 if k == "q" else 1) for r in regs[:3])
            ty = f"{'int' if sg else 'uint'}{bits}_t"
            return [f"{{ uint8_t a[16], b[16], r[16]; memcpy(a, &c->vfp.d[{a0}], {8 * nd}); memcpy(b, &c->vfp.d[{b0}], {8 * nd});"
                    f" for (int k = 0; k < {8 * nd // (bits // 8)}; k++) {{ {ty} x, y, z; memcpy(&x, a + k * {bits // 8}, {bits // 8});"
                    f" memcpy(&y, b + k * {bits // 8}, {bits // 8}); z = ({ty})(((int64_t)x + (int64_t)y + 1) >> 1);"
                    f" memcpy(r + k * {bits // 8}, &z, {bits // 8}); }} memcpy(&c->vfp.d[{d0}], r, {8 * nd}); }}"]
        raise Unsupported(f"{mn}.{dt}")

    # ------------------------------------------------------------------ whole function
    def build(self):
        lines = [f"void fn_{self.entry:08x}(Cpu *c) {{"]
        lines.append(f"\tENTER(c, 0x{self.entry:x}u);")
        hook = HOOKS.get(self.entry)
        if hook:
            lines.append(f"\trecomp_hook_{hook}(c);   /* --hooks: before the body, can change c->r / c->vfp */")
        if self.insns and self.insns[0].address != self.entry:
            lines.append(f"\tgoto L_{self.entry:08x};   /* an entry inside another function's code */")
        for i in self.insns:
            lines.append(f"L_{i.address:08x}: /* {i.mnemonic} {i.op_str} */")
            lines.append(f"\tTR(0x{i.address:x}u);")
            try:
                for s in self.translate(i):
                    lines.append("\t" + s)
            except Unsupported as e:
                self.unimpl.append((i.address, f"{i.mnemonic} {i.op_str}", str(e)))
                lines.append(f"\tUNIMPL(c, 0x{i.address:x}u);")
            ih = IHOOKS.get(i.address)
            if ih:
                ops = i.operands
                reg = GPR.get(i.reg_name(ops[0].reg), -1) if ops and ops[0].type == ARM_OP_REG else -1
                lines.append(f"\trecomp_ihook_{ih}(c, {reg});   /* --insn-hooks: after this instruction */")
        lines.append("\treturn;")
        # computed jumps (tbb/tbh) land on any instruction of the function
        lines.append("jump:")
        lines.append("\tswitch (c->r[15]) {")
        for i in self.insns:
            lines.append(f"\tcase 0x{i.address:x}u: goto L_{i.address:08x};")
        lines.append("\tdefault: recomp_unimplemented(c, c->r[15]); return;")
        lines.append("\t}")
        lines.append("}")
        return lines


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    ORDERED_LOADS[0] = "--unordered-loads" not in sys.argv
    for flag, table in (("--hooks", HOOKS), ("--insn-hooks", IHOOKS)):
        if flag in sys.argv:
            for tok in sys.argv[sys.argv.index(flag) + 1].split():
                addr, name = tok.split(":", 1)
                table[int(addr, 0)] = name
    d = json.load(open(sys.argv[1]))
    blocks = {b["start"]: bytes.fromhex(b["bytes"]) for b in d["blocks"] if b.get("init")}

    def code(lo, hi):
        for s, bb in blocks.items():
            if s <= lo and hi <= s + len(bb):
                return bb[lo - s:hi - s]
        return None
    entries = sorted({v & ~1 for v in d["functions"].values()})
    # body ranges: [first, last byte] (Ghidra), or [first, last, owner entry] (analyze.py, which knows)
    bodies = sorted(tuple(b) for b in d["bodies"])
    out = ['#include "rt.h"', "#define JUMP(t) do { c->r[15] = (t); goto jump; } while (0)"]
    fns, total, bad = [], 0, []
    # a function can be several code chunks: each chunk belongs to its owner when the export says,
    # else (Ghidra's ranges) to itself if it's an entry, else the nearest entry before it
    # import stubs hold a placeholder until the loader patches them (SCE's linker leaves ARM-mode
    # "mvn r0, #0; bx lr", i.e. return -1; some files leave zeros): they are calls out
    # (recomp_call_external), not code
    names = {v & ~1: n for n, v in d["functions"].items()}
    stubs = set()
    for lo, hi, *_ in bodies:
        b = code(lo, hi + 1)
        if lo in names and b is not None and (not any(b) or b[:8] == bytes.fromhex("0000e0e31eff2fe1")):
            stubs.add(lo)
    entries = [e for e in entries if e not in stubs]
    bodies = [b for b in bodies if b[0] not in stubs]
    SETJMP_STUBS.update(a for a in stubs if names[a] in ("setjmp", "_setjmp"))
    eset, chunks, fspans = set(entries), {}, {}

    def decode(spans, arm_mode):
        """linear decode of a function's spans; undecodable bytes (literal pools) are skipped"""
        dis, step = (md_arm, 4) if arm_mode else (md, 2)
        insns = []
        for lo, hi in spans:
            c = code(lo, hi + 1)
            pos = 0
            while c and pos < len(c):
                got = list(dis.disasm(c[pos:], lo + pos))
                insns += got
                pos = (got[-1].address + got[-1].size - lo) if got else pos
                if pos < len(c) and (not got or got[-1].address + got[-1].size - lo == pos):
                    pos += step
        return insns

    def junk(insns):
        for i in insns[:64]:
            if i.mnemonic.split(".")[0] in ("stc", "ldc", "cdp", "mcrr", "mrrc", "stc2", "ldc2", "cdp2", "udf", "mcr", "mrc"):
                cp = re.match(r"p(\d+)", i.op_str)
                if cp and int(cp.group(1)) not in (10, 11, 14, 15):
                    return True
        return False
    sorted_entries = sorted(entries)
    for lo, hi, *own in bodies:
        if own:
            owner = own[0]
        else:
            k = bisect.bisect_right(sorted_entries, lo) - 1
            owner = lo if lo in eset else (sorted_entries[k] if k >= 0 else None)
        if owner is not None and owner in eset:
            chunks.setdefault(owner, []).append((lo, hi))
    for entry in sorted(chunks):
        if entry not in {lo for lo, hi in chunks[entry]}:
            continue
        # Ghidra leaves the targets of a computed jump (mov pc, rN into an unrolled sequence) out of
        # the body: fill the holes between a function's own chunks (no other entry can sit there,
        # or the later chunk would belong to it); undecodable bytes in a hole (literal pools) are skipped
        # (each chunk and each hole is its own span: decoding restarts at every chunk, so a literal
        # pool in a hole that happens to decode as a 4-byte instruction can't run into the next chunk)
        spans = []
        for lo, hi in sorted(set(chunks[entry])):
            k = bisect.bisect_right(sorted_entries, spans[-1][1]) if spans else 0
            other = k < len(sorted_entries) and sorted_entries[k] < lo and sorted_entries[k] != entry
            if spans and lo <= spans[-1][1] + 1:
                spans[-1][1] = max(spans[-1][1], hi)
                continue
            if spans and lo - spans[-1][1] < 0x1000 and not other:
                spans.append([spans[-1][1] + 1, lo - 1])   # the hole
            spans.append([lo, hi])
        fspans[entry] = spans
        fns.append(Fn(entry, decode(spans, False), eset))
    # ARM-mode functions: a Thumb "blx #imm" always switches to ARM, so its targets are ARM; from
    # ARM code, "bl" stays ARM and "blx #imm" goes back to Thumb (propagated to a fixed point); a
    # function only reached through pointers is ARM when its Thumb decode holds instructions no Vita
    # code has (coprocessors other than VFP/NEON/system) and its ARM decode doesn't
    by_entry = {f.entry: f for f in fns}
    arm = set()
    def calls(f, mnem):
        for i in f.insns:
            if i.mnemonic == mnem and i.operands and i.operands[0].type == ARM_OP_IMM:
                yield i.operands[0].imm & 0xffffffff
    todo = [t for f in fns for t in calls(f, "blx") if t in by_entry]
    todo += [a for a in d.get("arm", []) if a in by_entry]   # the analysis decoded these as ARM (its exports and calls say so)
    for f in fns:
        if junk(f.insns) and not junk(decode(fspans[f.entry], True)):
            todo.append(f.entry)
    while todo:
        e = todo.pop()
        if e in arm or e not in by_entry:
            continue
        arm.add(e)
        f = Fn(e, decode(fspans[e], True), eset, arm=True)
        by_entry[e] = f
        todo += [t for t in calls(f, "bl") if t in by_entry]
    fns = [by_entry[f.entry] for f in fns]
    if arm:
        print(f"{len(arm)} ARM-mode functions")
    # code reached only through a pointer (a callback the module installs for itself) can sit inside
    # what Ghidra made one function: any Thumb code pointer in the image's words that lands on an
    # instruction of a function, not at its entry, gets its own entry (that function, entered there)
    starts = {i.address: f for f in fns for i in f.insns}
    arm_starts = {a for a, f in starts.items() if f.arm}
    alt = set()
    # a word that is itself a decoded ARM instruction holds no pointer, however it looks: an ARM
    # "bhi" encodes as 0x8Axxxxxx, which inside a module loaded at 0x8A000000 reads as a code pointer
    # into its first pages (libgxm's division helper grew phantom Thumb entries that way)
    def is_insn(w): return w in arm_starts
    for s0, bb in blocks.items():
        for o in range((-s0) % 4, len(bb) - 3, 4):
            if is_insn(s0 + o):
                continue
            v = int.from_bytes(bb[o:o + 4], "little")
            a = v & ~1
            if ((v & 1) or v in arm_starts) and a in starts and a not in eset and a not in stubs:
                alt.add(a)
    for a in sorted(alt):
        eset.add(a)
        fns.append(Fn(a, starts[a].insns, eset, arm=starts[a].arm))
    # small functions only reached through a pointer (a virtual method in a .rodata vtable) can sit
    # in a gap no Ghidra body covers: a Thumb pointer into such a gap gets a function decoded from
    # there to the next body, when that decodes cleanly to a return
    los = sorted(b[0] for b in d["bodies"])
    covered = sorted((b[0], b[1]) for b in d["bodies"])
    text = [(s0, s0 + len(bb)) for s0, bb in blocks.items() if s0 in {b["start"] for b in d["blocks"] if b.get("name") == ".text"}]
    def in_gap(a):
        i = bisect.bisect_right(los, a) - 1
        return (i < 0 or covered[i][1] < a) and any(lo <= a < hi for lo, hi in text)
    gap = set()
    for s0, bb in blocks.items():
        for o in range((-s0) % 4, len(bb) - 3, 4):
            if is_insn(s0 + o):
                continue
            v = int.from_bytes(bb[o:o + 4], "little")
            a = v & ~1
            if (v & 1) and a not in starts and a not in eset and in_gap(a):
                gap.add(a)
    ngap = 0
    for a in sorted(gap):
        nxt = los[bisect.bisect_right(los, a)] if bisect.bisect_right(los, a) < len(los) else a + 0x40
        insns = decode([(a, min(nxt, a + 0x400) - 1)], False)
        end = next((k for k, i in enumerate(insns) if (i.mnemonic == "bx" and i.op_str == "lr")
                    or (i.mnemonic in ("pop", "pop.w") and "pc" in i.op_str) or i.mnemonic in ("b", "b.w")), None)
        if not insns or insns[0].address != a or end is None or junk(insns[:end + 1]):
            continue
        eset.add(a); ngap += 1
        fns.append(Fn(a, insns[:end + 1], eset))
    if ngap:
        print(f"{ngap} functions in gaps between Ghidra's bodies (reached through pointers)")
    for a in sorted(set(HOOKS) - {f.entry for f in fns}):   # a typo, or an address that moved: say so now, not after a run shows nothing changed
        print(f"warning: --hooks {a:#x}:{HOOKS[a]} is not a function entry; that hook will never run")
    insn_addrs = {i.address for f in fns for i in f.insns}
    for a in sorted(set(IHOOKS) - insn_addrs):
        print(f"warning: --insn-hooks {a:#x}:{IHOOKS[a]} is not an instruction of any function; that hook will never run")
    decls = [f"extern void recomp_hook_{n}(Cpu *c);" for n in sorted(set(HOOKS.values()))]
    decls += [f"extern void recomp_ihook_{n}(Cpu *c, int reg);" for n in sorted(set(IHOOKS.values()))]
    decls += [f"void fn_{f.entry:08x}(Cpu *c);" for f in fns]
    # --split N: a game is ~100x a firmware library; N part files (plus this one, with the tables)
    # compile in parallel and keep each translation unit a size compilers handle well
    nsplit = int(sys.argv[sys.argv.index("--split") + 1]) if "--split" in sys.argv else 0
    parts = [[] for _ in range(nsplit)]
    if nsplit:
        base = sys.argv[2][:-2] if sys.argv[2].endswith(".c") else sys.argv[2]
        open(base + ".h", "w").write("\n".join(out + decls) + "\n")
        out = [f'#include "{os.path.basename(base)}.h"']
    else:
        out += decls
    per = (sum(len(f.insns) for f in fns) // max(nsplit, 1)) + 1
    k = acc = 0
    for f in fns:
        body = f.build()
        total += len(f.insns)
        bad += f.unimpl
        if nsplit:
            parts[k] += body
            acc += len(f.insns)
            if acc >= per and k < nsplit - 1:
                k, acc = k + 1, 0
        else:
            out += body
    for n, part in enumerate(parts):
        open(f"{base}_part{n}.c", "w").write("\n".join([f'#include "{os.path.basename(base)}.h"'] + part) + "\n")
    # --module NAME: a module loaded with the main program (a .suprx): its tables are suffixed and
    # register themselves with the host (main.c's module registry) before main() runs
    mod = sys.argv[sys.argv.index("--module") + 1] if "--module" in sys.argv else None
    sfx = f"_{mod}" if mod else ""
    out.append(f"const char *recomp_import_name{sfx}(uint32_t a) {{\n\tswitch (a) {{")
    out += [f'\tcase 0x{a:x}u: return "{names[a]}";' for a in sorted(stubs)]
    out.append("\tdefault: return 0;\n\t}\n}")
    out.append(f"int recomp_is_arm{sfx}(uint32_t a) {{\n\tswitch (a) {{")
    out += [f"\tcase 0x{f.entry:x}u:" for f in fns if f.arm]
    out.append("\t\treturn 1;" if any(f.arm for f in fns) else "")
    out.append("\tdefault: return 0;\n\t}\n}")
    out.append(f"GuestFn recomp_lookup{sfx}(uint32_t a) {{")
    out.append("\tswitch (a) {")
    out += [f"\tcase 0x{f.entry:x}u: return fn_{f.entry:08x};" for f in fns]
    out.append("\tdefault: return 0;\n\t}\n}")
    if mod:
        out.append("void recomp_register_module(const char *name, GuestFn (*lookup)(uint32_t), const char *(*import_name)(uint32_t), int (*is_arm)(uint32_t));")
        out.append(f'__attribute__((constructor)) static void register_{mod}(void) {{ recomp_register_module("{mod}", recomp_lookup{sfx}, recomp_import_name{sfx}, recomp_is_arm{sfx}); }}')
    open(sys.argv[2], "w").write("\n".join(out) + "\n")
    print(f"{len(fns)} functions, {len(stubs)} imports, {total} instructions, {len(bad)} unsupported -> {sys.argv[2]}")
    from collections import Counter
    for (k, n) in Counter(u[1].split()[0] for u in bad).most_common(15):
        print(f"  {n:4} {k}")


if __name__ == "__main__":
    main()
