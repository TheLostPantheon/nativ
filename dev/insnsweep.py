#!/usr/bin/env python3
"""Instruction-form sweep: every distinct instruction form in a recompiled module, one instruction at
a time, recompiled (recomp.py's own translation) against the original executed by Unicorn, from many
random states. A function-level check (difftest.py, the in-place checker) sees an instruction only
with the inputs its function happened to give it; a translation that is wrong only for some operand
values or one operand shape -- Unit 13's `vld1.32 {d6[], d7[]}, [r1]` loaded d7 from the next word,
and only NPCs whose heading went through it stood still -- passes thousands of those checks. Here
every form meets random registers, special floats and random flags, alone.

A form is the instruction with its registers named by first appearance ("vld1.32 {D0[], D1[]},
[R0]" -- so rd==rn, a writeback base reused inside a register list, or two operands sharing a
register is a DIFFERENT form from the same shape on distinct registers, not something a random
per-form register happens to reproduce) and its immediates and lane indices classified (exact for
shift/rotate amounts and other small values that bend behaviour one at a time -- 0..8, 16, 24, 31,
32 -- else a bucket: small/large/neg/rot for a rotated-immediate's carry). sp, lr and pc stay
literal: the sweep gives them fixed, special values, not a random GPR's. The mnemonic (condition,
S bit, NEON dt -- vmovl.s8 and vmovl.s16 are different element sizes) is never touched by the
register regexes, so a dt specifier can't be folded into a register. Up to K instances of each form
(different encodings and registers) are tested, from `trials` random states apiece. Memory operands
point into a scratch area both sides start from the same random bytes in. Control flow (branches,
calls, IT, writes to pc), exclusive monitors and coprocessor reads are left out: those are the
function-level checks' business.

Each form's trials run in a forked child (INSNSWEEP_BATCH forms per fork, default 32): some NEON
encodings crash Unicorn's JIT outright on Apple Silicon hosts (EXC_BAD_INSTRUCTION, recomp-tool-
quirks.md), and that is the host's problem, not a reason to lose every untested form after it.

usage: insnsweep.py <analysis.data.json> <generated .c (the base: game.c, mod_libc.c)> [trials] [instances per form]
INSNSWEEP_ONLY=<substring>: only forms containing it. INSNSWEEP_FORM_CAP=<n>: a module with far
more forms than this (default 20000) is sampled down to n rather than silently taking forever.
INSNSWEEP_TRACE=1: print each batch of forms before testing it, to localise a crash. INSNSWEEP_BATCH=<n>:
forms per fork (default 32; 1 isolates each form at the cost of a fork per form).
"""
import ctypes, glob, json, mmap, os, random, re, struct, subprocess, sys, tempfile
from collections import defaultdict
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_PROT_ALL, UcError
from unicorn.arm_const import *

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "recompiler"))   # (recomp)
import recomp   # its capstone instances (md, md_arm) and Fn.translate
from capstone.arm import ARM_OP_MEM, ARM_OP_REG

SCRATCH, STACK = 0x90000000, 0xA0000000
SCRATCH_SIZE, STACK_SIZE = 0x10000, 0x10000
FPSCR0 = 0x03000000   # RunFast: flush-to-zero, default NaN, as on the console


class Cpu(ctypes.Structure):
    _fields_ = [("r", ctypes.c_uint32 * 16), ("n", ctypes.c_uint32), ("z", ctypes.c_uint32),
                ("c", ctypes.c_uint32), ("v", ctypes.c_uint32), ("q", ctypes.c_uint32), ("_pad", ctypes.c_uint32),
                ("s", ctypes.c_uint32 * 64), ("fpscr", ctypes.c_uint32), ("stop", ctypes.c_uint32)]


SKIP_STEMS = {"b", "bl", "blx", "bx", "cbz", "cbnz", "tbb", "tbh", "svc", "bkpt", "udf", "nop", "yield", "wfe", "wfi",
              "sev", "hint", "isb", "dsb", "dmb", "pld", "pli", "pldw", "cps", "cpsid", "cpsie", "setend", "smc",
              "ldrex", "strex", "ldrexb", "strexb", "ldrexh", "strexh", "ldrexd", "strexd", "clrex",
              "mrc", "mcr", "mrc2", "mcr2", "mrrc", "mcrr", "cdp", "cdp2", "ldc", "stc", "ldc2", "stc2", "ldc2l", "stc2l",
              "vmsr", "msr", "srs", "rfe", "eret", "subs"}   # subs pc, lr is the exception return; plain subs gets through below


def stem(mn):
    return mn.split(".")[0]


IMM_EXACT = frozenset((0, 1, 2, 3, 4, 5, 6, 7, 8, 16, 24, 31, 32))   # shift/rotate amounts that change behaviour one value at a time


def imm_class(v):
    """an integer immediate's class: exact for the small/boundary values a shift, rotate or lane
    index actually bends behaviour on, otherwise a bucket (a rotated 8-bit constant carries C on
    ARM-mode flag-setting logical ops -- 2d0aa85's bug -- a plain small or large one does not)"""
    if v in IMM_EXACT:
        return str(v)
    if 0 <= v <= 0xff:
        return "small"
    for k in range(2, 32, 2):   # the ARM modified-immediate encoding only rotates by even amounts
        if ((v >> k) | (v << (32 - k))) & 0xffffffff <= 0xff:
            return "rot"
    return "large"


def imm_repl(m):
    s = m.group(0)[1:]
    neg = s.startswith("-")
    if neg:
        s = s[1:]
    if s[:2] == "0x":                        # hex int (checked before the float case: a hex digit can be 'e')
        return f"#{'neg' if neg else ''}{imm_class(int(s, 16))}"
    if re.fullmatch(r"\d+", s):               # decimal int
        return f"#{'neg' if neg else ''}{imm_class(int(s))}"
    return f"#f{'-' if neg else ''}{s}"       # a VMOV float immediate: only 256 encodings exist, exact is cheap


def lane_repl(m):
    v = int(m.group(1))
    return f"[{v}]" if v <= 8 else "[n]"   # vmov.8 dN[k] can take k up to 7; keep it exact, same as any other small immediate


_ALIAS = {"sb": "r9", "sl": "r10", "fp": "r11", "ip": "r12", "r13": "sp", "r14": "lr", "r15": "pc"}


def rename_regs(ops):
    """fold registers to R0,R1,.../D0,D1,.../Q0,Q1,.../S0,S1,... by first appearance in the operand
    text, so rd==rn, a writeback base reused inside a register list, or two halves of a vld2 pair
    sharing a register become a DIFFERENT form from the same instruction with distinct registers --
    the aliasing a per-form random register never reproduces on its own. sp, lr and pc stay literal:
    the sweep gives them fixed, special values (sp a real stack, lr a return-address sentinel, pc the
    instruction's own address), unlike the random GPRs, so a form that names one is not interchangeable
    with the same shape on an ordinary register."""
    ops = re.sub(r"\b(sb|sl|fp|ip|r1[345])\b", lambda m: _ALIAS[m.group(0)], ops)
    for prefix, pat in (("R", r"\br(\d+)\b"), ("D", r"\bd(\d+)\b"), ("Q", r"\bq(\d+)\b"), ("S", r"\bs(\d+)\b")):
        seen = {}
        ops = re.sub(pat, lambda m, seen=seen, prefix=prefix: prefix + str(seen.setdefault(m.group(1), len(seen))), ops)
    return ops


def form_of(mnemonic, op_str):
    """the instruction with its registers named by the equality pattern above, its immediates and
    lane indices classified, and its mnemonic (condition, S bit, NEON dt: vmovl.s8 vs vmovl.s16 are
    different element sizes, not the same form) left exactly as capstone printed it -- the mnemonic
    is never run through the register regexes, so a dt specifier like .s8 can't be mistaken for s8"""
    ops = op_str.lower()
    ops = re.sub(r"#-?(0x[0-9a-f]+|\d+(\.\d+)?(e[+-]?\d+)?)", imm_repl, ops)
    ops = re.sub(r"\[(\d+)\]", lane_repl, ops)
    ops = rename_regs(ops)
    ops = re.sub(r"\s+", " ", ops).strip()
    return f"{mnemonic.lower()} {ops}".strip()


def is_control(i, mn):
    s = stem(mn)
    if s.startswith("it") and len(s) <= 5 and set(s[2:]) <= set("te"):
        return True
    if re.fullmatch(r"b(eq|ne|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|cs|cc|al)?", s):   # conditional branches too
        return True
    if s in SKIP_STEMS and not (s == "subs" and "pc" not in i.op_str):
        return True
    ops = i.op_str
    # anything that writes pc (pop {..pc}, ldr pc, mov pc, add pc, ldm ..pc)
    if re.search(r"(^|\{|, )pc\b", ops) and (s in ("pop", "ldm", "ldmia", "ldmdb", "ldmda", "ldmib") or ops.startswith("pc")):
        return True
    return False


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    d = json.load(open(sys.argv[1]))
    base = sys.argv[2]
    trials = int(sys.argv[3]) if len(sys.argv) > 3 else 64
    per_form = int(sys.argv[4]) if len(sys.argv) > 4 else 3
    only = os.environ.get("INSNSWEEP_ONLY")
    form_cap = int(os.environ.get("INSNSWEEP_FORM_CAP", 20000))   # a module far past this gets sampled, not silently truncated
    blocks = [(b["start"], bytes.fromhex(b["bytes"])) for b in d["blocks"] if b.get("init") and b["start"] < 0xF0000000]

    def code(a, n):
        for s, b in blocks:
            if s <= a and a + n <= s + len(b):
                return b[a - s:a - s + n]
        return None

    srcs = [base] + sorted(glob.glob(base[:-2] + "_part*.c"))
    text = {}
    for f in srcs:
        for a, t in re.findall(r"^L_([0-9a-f]{8}): /\* (.*) \*/", open(f).read(), re.M):
            text[int(a, 16)] = t
    # the instruction as the recompiler saw it: decode where it says, in whichever mode reproduces the text
    forms = defaultdict(list)
    norm = lambda s: re.sub(r"\s+", " ", s.strip())
    for a in sorted(text):
        want = norm(text[a])
        if not want or want.startswith("."):
            continue
        for dis, arm in ((recomp.md, False), (recomp.md_arm, True)):
            b = code(a, 4)
            if b is None:
                break
            got = next(dis.disasm(b, a, count=1), None)
            if got and norm(f"{got.mnemonic} {got.op_str}") == want:
                if not is_control(got, got.mnemonic):
                    f = form_of(got.mnemonic, got.op_str)
                    if not only or only in f:
                        forms[f].append((a, arm))
                break
    total_forms = len(forms)
    print(f"{len(text)} instructions, {total_forms} forms to test", flush=True)
    if total_forms > form_cap * 1.5:   # no silent caps: say so, and say what ran instead
        rng = random.Random(0x5eed)
        forms = {k: forms[k] for k in rng.sample(sorted(forms), form_cap)}
        print(f"   {total_forms} is far past the ~{form_cap} target for one run; sampling {len(forms)} forms "
              f"(set INSNSWEEP_FORM_CAP to change)", flush=True)

    # one test function per chosen instance, translated exactly as recomp.py translates it in a function
    chosen, csrc = [], ['#include "rt.h"', "#define JUMP(t) do { c->r[15] = (t); goto jump; } while (0)"]
    unsupported = {}
    for f, inst in sorted(forms.items()):
        rng = random.Random(f)
        pick = inst if len(inst) <= per_form else rng.sample(inst, per_form)
        for a, arm in pick:
            dis = recomp.md_arm if arm else recomp.md
            i = next(dis.disasm(code(a, 4), a, count=1))
            fn = recomp.Fn(a, [i], {a}, arm)
            try:
                lines = fn.translate(i)
            except recomp.Unsupported as e:
                unsupported[f] = str(e)
                continue
            body = "\n\t".join(lines)
            if "goto L_" in body:
                continue
            csrc.append(f"static void t_{a:08x}(Cpu *c) {{\n\t{body}\n\treturn;\njump:\n\tc->stop = 2;\n}}")
            chosen.append((f, a, arm, f"{i.mnemonic} {i.op_str}", i))
    csrc.append("GuestFn recomp_lookup(uint32_t a) {\n\tswitch (a) {")
    csrc += [f"\tcase 0x{a:x}u: return t_{a:08x};" for _, a, _, _, _ in chosen]
    csrc.append("\t}\n\treturn 0;\n}")
    csrc.append("const char *recomp_import_name(uint32_t a) { (void)a; return 0; }")
    csrc.append("int recomp_is_arm(uint32_t a) { (void)a; return 0; }")
    tmp = tempfile.mkdtemp(prefix="insnsweep_")
    cpath = os.path.join(tmp, "sweep.c")
    open(cpath, "w").write("\n".join(csrc) + "\n")
    lib_path = os.path.join(tmp, "sweep.dylib")
    subprocess.run(["cc", "-O1", "-ffp-contract=off", "-w", "-fPIC", "-shared", "-I", HERE, "-I", os.path.join(HERE, "..", "recompiler"), cpath,
                    os.path.join(HERE, "glue.c"), "-o", lib_path], check=True)
    lib = ctypes.CDLL(lib_path)
    lib.recomp_run.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    arena = mmap.mmap(-1, 1 << 32, flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS)
    arena_addr = ctypes.addressof(ctypes.c_char.from_buffer(arena))
    lib.recomp_set_mem(ctypes.c_void_p(arena_addr))
    lib.recomp_runfast()
    for a, b in blocks:
        ctypes.memmove(arena_addr + a, b, len(b))

    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    mapped = set()
    for a, b in blocks + [(SCRATCH, bytes(SCRATCH_SIZE)), (STACK, bytes(STACK_SIZE))]:
        lo, hi = a & ~0xfff, (a + len(b) + 0xfff) & ~0xfff
        pages = [p for p in range(lo, hi, 0x1000) if p not in mapped]
        # map runs of new pages
        run = None
        for p in pages + [None]:
            if p is not None and (run is None or p == run[1]):
                run = run or [p, p]; run[1] = p + 0x1000
            else:
                if run:
                    uc.mem_map(run[0], run[1] - run[0], UC_PROT_ALL); mapped.update(range(run[0], run[1], 0x1000))
                run = [p, p + 0x1000] if p is not None else None
        uc.mem_write(a, b)
    uc.reg_write(UC_ARM_REG_C1_C0_2, uc.reg_read(UC_ARM_REG_C1_C0_2) | (0xf << 20))
    uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)

    SPECIAL = [0, 0x3f800000, 0xbf800000, 0x3f000000, 0x7fc00000, 0x7f800000, 0xff800000, 0x00000001, 0x80000000,
               0x007fffff, 0x7f7fffff, 0x40490fdb, 0xc0490fdb, 0x3fc90fdb, 0x4b000000, 0xcf000000, 0x4f000000]

    def state(rng, i):
        regs = [rng.choice([0, 1, 2, 0xffffffff, 0x80000000, 0x7fffffff, rng.randrange(256), rng.getrandbits(32),
                            SCRATCH + rng.randrange(0x2000, SCRATCH_SIZE - 0x2000, 4)]) for _ in range(13)]
        for o in i.operands:   # a memory operand's base in the scratch area, its index small
            if o.type == ARM_OP_MEM:
                bn = recomp.md.reg_name(o.mem.base)
                if bn in recomp.GPR and recomp.GPR[bn] < 13:
                    regs[recomp.GPR[bn]] = SCRATCH + rng.randrange(0x4000, SCRATCH_SIZE - 0x4000, 16)
                if o.mem.index:
                    xn = recomp.md.reg_name(o.mem.index)
                    if xn in recomp.GPR and recomp.GPR[xn] < 13:
                        regs[recomp.GPR[xn]] = rng.randrange(0, 64) * 4
        mn = stem(i.mnemonic)
        if mn in ("ldm", "ldmia", "ldmdb", "stm", "stmia", "stmdb", "vldm", "vldmia", "vldmdb", "vstm", "vstmia", "vstmdb",
                  "vld1", "vld2", "vld3", "vld4", "vst1", "vst2", "vst3", "vst4", "ldrd", "strd", "vldr", "vstr"):
            for o in i.operands[:1]:   # ldm/stm and the vector transfers name their base first or in the memory operand
                if o.type == ARM_OP_REG:
                    n = recomp.md.reg_name(o.reg)
                    if n in recomp.GPR and recomp.GPR[n] < 13 and mn.startswith(("ldm", "stm")):
                        regs[recomp.GPR[n]] = SCRATCH + rng.randrange(0x4000, SCRATCH_SIZE - 0x4000, 16)
        s = [rng.choice(SPECIAL) if rng.random() < 0.3 else rng.getrandbits(32) for _ in range(64)]
        scratch = bytes(rng.getrandbits(8) for _ in range(SCRATCH_SIZE))
        flags = [rng.getrandbits(1) for _ in range(4)]
        return regs, s, scratch, flags

    def run_uc(a, arm, st):
        regs, s, scratch, flags = st
        uc.mem_write(SCRATCH, scratch); uc.mem_write(STACK, bytes(STACK_SIZE))
        for k in range(13):
            uc.reg_write(UC_ARM_REG_R0 + k, regs[k])
        uc.reg_write(UC_ARM_REG_SP, STACK + STACK_SIZE - 0x200)
        uc.reg_write(UC_ARM_REG_LR, 0xB0000001)
        for k in range(32):
            uc.reg_write(UC_ARM_REG_D0 + k, s[2 * k] | (s[2 * k + 1] << 32))
        uc.reg_write(UC_ARM_REG_FPSCR, FPSCR0)
        cp = uc.reg_read(UC_ARM_REG_CPSR) & 0x0fffffff
        uc.reg_write(UC_ARM_REG_CPSR, cp | (flags[0] << 31) | (flags[1] << 30) | (flags[2] << 29) | (flags[3] << 28))
        try:
            uc.emu_start(a if arm else a | 1, 0xffffffff, count=1)
        except UcError as e:
            return None
        cp = uc.reg_read(UC_ARM_REG_CPSR)
        return ([uc.reg_read(UC_ARM_REG_R0 + k) for k in range(13)] + [uc.reg_read(UC_ARM_REG_SP)],
                [(cp >> 31) & 1, (cp >> 30) & 1, (cp >> 29) & 1, (cp >> 28) & 1],
                [uc.reg_read(UC_ARM_REG_D0 + k) for k in range(32)],
                bytes(uc.mem_read(SCRATCH, SCRATCH_SIZE)) + bytes(uc.mem_read(STACK, STACK_SIZE)))

    def run_native(a, st):
        regs, s, scratch, flags = st
        ctypes.memmove(arena_addr + SCRATCH, scratch, SCRATCH_SIZE)
        ctypes.memset(arena_addr + STACK, 0, STACK_SIZE)
        c = Cpu()
        for k in range(13):
            c.r[k] = regs[k]
        c.r[13] = STACK + STACK_SIZE - 0x200
        c.r[14] = 0xB0000001
        for k in range(64):
            c.s[k] = s[k]
        c.n, c.z, c.c, c.v = flags
        c.fpscr = FPSCR0
        lib.recomp_run(ctypes.byref(c), a)
        if c.stop:
            return None
        return (list(c.r[:14]), [c.n, c.z, c.c, c.v], [c.s[2 * k] | (c.s[2 * k + 1] << 32) for k in range(32)],
                ctypes.string_at(arena_addr + SCRATCH, SCRATCH_SIZE) + ctypes.string_at(arena_addr + STACK, STACK_SIZE))

    def diff(u, n):
        for k in range(14):
            if u[0][k] != n[0][k]:
                return f"{'sp' if k == 13 else 'r%d' % k} {n[0][k]:#x} vs original {u[0][k]:#x}"
        if u[1] != n[1]:
            return f"nzcv {''.join(map(str, n[1]))} vs original {''.join(map(str, u[1]))}"
        for k in range(32):
            if u[2][k] != n[2][k]:
                return f"d{k} {n[2][k]:#018x} vs original {u[2][k]:#018x}"
        if u[3] != n[3]:
            j = next(k for k in range(len(u[3])) if u[3][k] != n[3][k])
            at = SCRATCH + j if j < SCRATCH_SIZE else STACK + j - SCRATCH_SIZE
            return f"memory {at:#x}: {n[3][j]:#04x} vs original {u[3][j]:#04x}"
        return None

    def test_form(inst):
        verdict, compared = None, 0
        for a, arm, t, i in inst:
            rng = random.Random(a)
            for k in range(trials):
                st = state(rng, i)
                u = run_uc(a, arm, st)
                if u is None:
                    continue
                n = run_native(a, st)
                if n is None:
                    verdict = f"{a:#x} {t}: the translation stopped (UNIMPL/jump)"
                    break
                compared += 1
                why = diff(u, n)
                if why:
                    regs, s, _, flags = st
                    verdict = f"{a:#x} {t}: {why}  [trial {k}; r={[hex(x) for x in regs[:4]]} nzcv={''.join(map(str, flags))}]"
                    break
            if verdict:
                break
        return verdict, compared

    BATCH = max(1, int(os.environ.get("INSNSWEEP_BATCH", 32)))   # forms per fork: Unicorn's JIT crashes outright
    # (EXC_BAD_INSTRUCTION) on some NEON encodings on Apple Silicon hosts (recomp-tool-quirks.md), and
    # that's a host/Unicorn limitation to route around, not a reason to lose every untested form after
    # it. A fork per batch bounds the loss to one batch instead of the whole run, and amortizes the
    # fork's cost (copying page tables over the 4 GiB arena) over many forms instead of paying it once
    # each; the child streams one JSON line per finished form so a crash mid-batch still keeps the
    # verdicts for the forms that finished before it. Every trial resets its own registers and memory,
    # so nothing a child computes needs to reach the parent except those verdicts.
    cur = [None]   # the form a child is testing, for its exception report

    def run_batch(batch, w):
        for f, inst in batch:
            cur[0] = f
            verdict, compared = test_form(inst)
            os.write(w, (json.dumps([f, verdict, compared]) + "\n").encode())

    def run_batch_isolated(batch):
        r, w = os.pipe()
        pid = os.fork()
        if pid == 0:
            os.close(r)
            try:
                run_batch(batch, w)
            except Exception as e:   # against the form that raised, not the batch's first (it may have a verdict already)
                os.write(w, (json.dumps([cur[0] or batch[0][0], f"the sweep itself raised: {e}", 0]) + "\n").encode())
                os._exit(3)
            os._exit(0)
        os.close(w)
        buf = b""
        results = {}
        while True:
            chunk = os.read(r, 65536)
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                f, verdict, compared = json.loads(line)
                results[f] = (verdict, compared)
        os.close(r)
        _, status = os.waitpid(pid, 0)
        if len(results) < len(batch):   # the child died before finishing the batch
            why = (f"signal {os.WTERMSIG(status)} -- likely the host's Unicorn JIT on a NEON encoding "
                    f"(recomp-tool-quirks.md), not this instruction's translation; re-run isolated or on "
                    f"an x86 host to tell the two apart" if os.WIFSIGNALED(status) else f"exit {os.WEXITSTATUS(status)}")
            for f, inst in batch:
                if f not in results:
                    results[f] = (f"CRASHED the sweep process ({why})", 0)
        return results

    bad, ok, untested, crashed = {}, 0, 0, 0
    by_form = defaultdict(list)
    for f, a, arm, t, i in chosen:
        by_form[f].append((a, arm, t, i))
    items = sorted(by_form.items())
    for lo in range(0, len(items), BATCH):
        batch = items[lo:lo + BATCH]
        if os.environ.get("INSNSWEEP_TRACE"):
            print(f"TRACE {batch[0][0]} .. {batch[-1][0]}", flush=True)
        for f, (verdict, compared) in run_batch_isolated(batch).items():
            if verdict and verdict.startswith("CRASHED"):
                crashed += 1
                print(f"CRASH {f}\n      {verdict}", flush=True)
            elif verdict:
                bad[f] = verdict
                print(f"DIFF  {f}\n      {verdict}", flush=True)
            elif compared:
                ok += 1
            else:
                untested += 1
    print(f"== {ok} forms identical, {len(bad)} differ, {untested} untested (every state faulted in the original), "
          f"{crashed} crashed the sweep process, "
          f"{len(unsupported)} unsupported by the translator")
    for f, why in sorted(unsupported.items())[:40]:
        print(f"   unsupported: {f} ({why})")


if __name__ == "__main__":
    main()
