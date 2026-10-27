#!/usr/bin/env python3
"""Differential test of a recompiled module (recompiler/recomp.py) against the original: each
function runs natively (recompiled, over a 4 GiB guest arena) and in Unicorn (the original
bytes) from the same random state -- registers pointing into random scratch memory or holding
random integers -- and every register, flag, VFP register and the scratch/stack memory must end
up identical. Unmapped guest memory reads as zero on both sides; imports return 0 on both.
Each trial runs in a forked child, so a hang or crash costs one result, not the run. Unicorn is capped
at 5 s per trial (a random state can send the original through a junk pointer: skipped, not compared).

usage: difftest.py <export.data.json> <recompiled.c> [trials per function] [function hex ...]
DIFFTEST_STEP=1 builds with per-instruction tracing and, for each diff, names the first instruction
whose result differs from Unicorn's (registers and flags in lockstep).
DIFFTEST_NOFORK=1 runs trials in-process, to backtrace a native crash under lldb.
"""
import ctypes, json, mmap, os, random, struct, subprocess, sys, tempfile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_MEM_UNMAPPED, UC_HOOK_CODE, UC_PROT_ALL
from unicorn.arm_const import *

HERE = os.path.dirname(os.path.abspath(__file__))
SCRATCH, STACK, RET = 0x90000000, 0xA0000000, 0xB0000000
SCRATCH_SIZE, STACK_SIZE = 0x10000, 0x10000


class Cpu(ctypes.Structure):
    _fields_ = [("r", ctypes.c_uint32 * 16), ("n", ctypes.c_uint32), ("z", ctypes.c_uint32),
                ("c", ctypes.c_uint32), ("v", ctypes.c_uint32), ("q", ctypes.c_uint32), ("_pad", ctypes.c_uint32),
                ("s", ctypes.c_uint32 * 64), ("fpscr", ctypes.c_uint32), ("stop", ctypes.c_uint32)]


def build(src, step=False):
    """compile the recompiled C (and its _partN.c files, from recomp.py --split) with the glue, in
    parallel, into one shared library"""
    import glob
    from concurrent.futures import ThreadPoolExecutor
    tag = os.path.basename(src) + ("_step" if step else "")
    out = os.path.join(tempfile.gettempdir(), "recomp_" + tag + ".dylib")
    srcs = [src] + sorted(glob.glob(src[:-2] + "_part*.c")) + [os.path.join(HERE, "glue.c")]
    flags = ["-O1", "-ffp-contract=off", "-g", "-w", "-fPIC", "-I", HERE, "-I", os.path.join(HERE, "..", "recompiler")] + (["-DRECOMP_TRACE"] if step else [])
    objs = [os.path.join(tempfile.gettempdir(), f"recomp_{tag}_{k}.o") for k in range(len(srcs))]
    with ThreadPoolExecutor(os.cpu_count()) as ex:
        def cc(so):   # a part whose object is newer than its source (and the runtime header) is reused
            src_, obj = so
            dep = max(os.path.getmtime(src_), os.path.getmtime(os.path.join(HERE, "..", "recompiler", "rt.h")))
            if os.path.exists(obj) and os.path.getmtime(obj) > dep:
                return
            subprocess.run(["cc", "-c"] + flags + [src_, "-o", obj], check=True)
        list(ex.map(cc, zip(srcs, objs)))
    subprocess.run(["cc", "-shared", "-o", out] + objs, check=True)
    return ctypes.CDLL(out)


def load_blocks(d):
    return [(b["start"], bytes.fromhex(b["bytes"])) for b in d["blocks"] if b.get("init") and b["start"] < 0xF0000000]


def state(rng):
    regs = []
    for k in range(13):
        regs.append(SCRATCH + rng.randrange(0, SCRATCH_SIZE - 0x100, 4) if rng.random() < 0.5 else rng.choice(
            [0, 1, 2, rng.randrange(0, 256), rng.getrandbits(32)]))
    scratch = bytes(rng.getrandbits(8) for _ in range(SCRATCH_SIZE))
    sregs = [rng.getrandbits(32) for _ in range(64)]
    flags = [rng.getrandbits(1) for _ in range(4)]
    return regs, scratch, sregs, flags


ARM_ENTRIES = set()   # from the analysis' "arm" list (system modules are mostly ARM code)


def run_unicorn(blocks, imports, entry, st, trace=None):
    regs, scratch, sregs, flags = st
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    mapped = set()

    def mp(a, n):   # map the unmapped pages of [a, a+n) as few contiguous regions
        run = None
        for p in list(range(a & ~0xfff, min((a + n + 0xfff) & ~0xfff, 1 << 32), 0x1000)) + [None]:
            if p is not None and p not in mapped:
                run = run or [p, p]
                run[1] = p + 0x1000; mapped.add(p)
            elif run:
                uc.mem_map(run[0], run[1] - run[0], UC_PROT_ALL); run = None
    for a, b in blocks:
        mp(a, len(b)); uc.mem_write(a, b)
    mp(SCRATCH, SCRATCH_SIZE); uc.mem_write(SCRATCH, scratch)
    mp(STACK, STACK_SIZE)
    mp(RET, 0x1000); uc.mem_write(RET, b"\x00\xbe" * 8)

    def unmapped(uc, access, addr, size, value, data):
        if addr < 0x10000:   # the Vita never maps the first 64 KiB: a NULL access crashes there, so
            return False     # the trial isn't comparable (a skip), as the firmware decompilation's own difftest does
        # a whole MiB at a time: page-at-a-time mapping goes quadratic when code scans memory
        mp(addr & ~0xfffff, 0x100000); mp(addr, size); return True
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, unmapped)
    for a in imports:   # an import returns 0
        def ret0(uc, address, size, data):
            uc.reg_write(UC_ARM_REG_R0, 0); uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
        uc.hook_add(UC_HOOK_CODE, ret0, begin=a, end=a)
    if trace is not None:
        regs_ = [UC_ARM_REG_R0 + k for k in range(13)] + [UC_ARM_REG_SP, UC_ARM_REG_LR]

        def step(uc, address, size, data):
            if len(trace) < 100000:
                cp = uc.reg_read(UC_ARM_REG_CPSR)
                trace.append((address, [uc.reg_read(r) for r in regs_], (cp >> 28) & 0xf,
                              [uc.reg_read(UC_ARM_REG_D0 + k) for k in range(32)]))
        uc.hook_add(UC_HOOK_CODE, step)
    # FPSCR: RunFast (flush-to-zero, default NaN) as on the console; enable VFP
    uc.reg_write(UC_ARM_REG_C1_C0_2, uc.reg_read(UC_ARM_REG_C1_C0_2) | (0xf << 20))
    uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
    uc.reg_write(UC_ARM_REG_FPSCR, 0x03000000)
    for k in range(13):
        uc.reg_write(UC_ARM_REG_R0 + k, regs[k])
    uc.reg_write(UC_ARM_REG_SP, STACK + STACK_SIZE - 0x100)
    uc.reg_write(UC_ARM_REG_LR, RET | 1)
    for k in range(32):
        uc.reg_write(UC_ARM_REG_D0 + k, sregs[2 * k] | (sregs[2 * k + 1] << 32))
    cpsr = uc.reg_read(UC_ARM_REG_CPSR) & 0x0fffffff
    uc.reg_write(UC_ARM_REG_CPSR, cpsr | (flags[0] << 31) | (flags[1] << 30) | (flags[2] << 29) | (flags[3] << 28))
    err = None
    try:
        # random states can send the original through a junk function pointer: cap it in time as
        # well as instructions, so that is a skip rather than a watchdog "hang" on a slow host
        uc.emu_start(entry if entry in ARM_ENTRIES else entry | 1, RET, timeout=5_000_000, count=2_000_000)   # ARM-mode functions start in ARM state
    except Exception as e:
        err = str(e)
    pc = uc.reg_read(UC_ARM_REG_PC)
    if err is None and (pc & ~1) not in range(RET, RET + 16):
        err = f"did not return (pc {pc:#x})"
    cp = uc.reg_read(UC_ARM_REG_CPSR)
    out = {"r": [uc.reg_read(UC_ARM_REG_R0 + k) for k in range(13)] + [uc.reg_read(UC_ARM_REG_SP)],
           "f": [(cp >> 31) & 1, (cp >> 30) & 1, (cp >> 29) & 1, (cp >> 28) & 1],
           "d": [uc.reg_read(UC_ARM_REG_D0 + k) for k in range(32)],
           "mem": bytes(uc.mem_read(SCRATCH, SCRATCH_SIZE)) + bytes(uc.mem_read(STACK, STACK_SIZE)), "err": err}
    return out


def run_native(lib, arena_addr, blocks, entry, st):
    regs, scratch, sregs, flags = st
    mem = (ctypes.c_uint8 * 1).from_address(arena_addr)

    def put(a, b):
        ctypes.memmove(arena_addr + a, b, len(b))
    for a, b in blocks:
        put(a, b)
    put(SCRATCH, scratch); put(STACK, bytes(STACK_SIZE))
    c = Cpu()
    for k in range(13):
        c.r[k] = regs[k]
    c.r[13] = STACK + STACK_SIZE - 0x100
    c.r[14] = RET | 1
    for k in range(64):
        c.s[k] = sregs[k]
    c.n, c.z, c.c, c.v = flags
    lib.recomp_runfast()
    lib.recomp_run(ctypes.byref(c), entry)
    unimpl = ctypes.c_uint32.in_dll(lib, "recomp_unimpl_at").value if c.stop else None
    return {"r": list(c.r[:14]), "f": [c.n, c.z, c.c, c.v],
            "d": [c.s[2 * k] | (c.s[2 * k + 1] << 32) for k in range(32)],
            "mem": ctypes.string_at(arena_addr + SCRATCH, SCRATCH_SIZE) + ctypes.string_at(arena_addr + STACK, STACK_SIZE),
            "unimpl": unimpl}


def lockstep(lib, arena_addr, blocks, imports, fn, st, insn_text):
    """first instruction whose incoming state differs: the instruction before it is the culprit"""
    ut = []
    run_unicorn(blocks, imports, fn, st, ut)
    ctypes.c_uint32.in_dll(lib, "recomp_trace_n").value = 0
    run_native(lib, arena_addr, blocks, fn, st)
    n = ctypes.c_uint32.in_dll(lib, "recomp_trace_n").value
    buf = (ctypes.c_uint32 * 81 * 100000).in_dll(lib, "recomp_trace_buf")
    nt = [(buf[k][0], list(buf[k][1:16]), buf[k][16],
           [buf[k][17 + 2 * j] | (buf[k][18 + 2 * j] << 32) for j in range(32)]) for k in range(n)]
    known = {x[0] for x in nt} & {x[0] for x in ut}
    full_u, native_addrs = ut, {x[0] for x in nt}
    ut = [x for x in ut if x[0] in known]
    nt = [x for x in nt if x[0] in known]
    prev = None
    for k, (u, v) in enumerate(zip(ut, nt)):
        names = [f"r{j}" for j in range(13)] + ["sp", "lr"]
        why = (f"pc {u[0]:#x} vs {v[0]:#x}" if u[0] != v[0] else
               next((f"{names[j]} {u[1][j]:#x} vs {v[1][j]:#x}" for j in range(15) if u[1][j] != v[1][j]), None) or
               (f"nzcv {u[2]:04b} vs {v[2]:04b}" if u[2] != v[2] else None) or
               next((f"d{j} {u[3][j]:#018x} vs {v[3][j]:#018x}" for j in range(32) if u[3][j] != v[3][j]), None))
        if why:
            p = f"{prev:#x} {insn_text.get(prev, '?')}" if prev is not None else "(entry)"
            only = sorted({x[0] for x in full_u[:full_u.index(u) if u in full_u else len(full_u)]} - native_addrs)
            extra = f"; only the original ran {len(only)} addresses, e.g. {[hex(x) for x in only[:4]]}" if only else ""
            return f"step {k}: before {u[0]:#x}: {why}; after executing {p}{extra}"
        prev = u[0]
    return f"traces agree over {min(len(ut), len(nt))} steps (lengths {len(ut)} / {len(nt)})"


def compare(a, b):
    for k in range(14):
        if a["r"][k] != b["r"][k]:
            return f"r{k}: {a['r'][k]:#x} vs {b['r'][k]:#x}"
    for k in range(32):
        if a["d"][k] != b["d"][k]:
            return f"d{k}: {a['d'][k]:#018x} vs {b['d'][k]:#018x}"
    if a["mem"] != b["mem"]:
        i = next(k for k in range(len(a["mem"])) if a["mem"][k] != b["mem"][k])
        where = SCRATCH + i if i < SCRATCH_SIZE else STACK + i - SCRATCH_SIZE
        return f"memory at {where:#x}: {a['mem'][i]:#04x} vs {b['mem'][i]:#04x}"
    return None


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    d = json.load(open(sys.argv[1]))
    ARM_ENTRIES.update(d.get("arm", []))
    trials = int(sys.argv[3]) if len(sys.argv) > 3 else 20
    only = {int(x, 16) for x in sys.argv[4:]}
    stepmode = bool(os.environ.get("DIFFTEST_STEP"))
    lib = build(sys.argv[2], stepmode)
    blocks = load_blocks(d)
    entries = sorted({v & ~1 for v in d["functions"].values()})
    import glob
    src = "".join(open(f).read() for f in [sys.argv[2]] + sorted(glob.glob(sys.argv[2][:-2] + "_part*.c")))
    recompiled = sorted(int(x, 16) for x in __import__("re").findall(r"^void fn_([0-9a-f]{8})\(Cpu \*c\) \{", src, __import__("re").M))
    insn_text = {int(a, 16): t for a, t in __import__("re").findall(r"^L_([0-9a-f]{8}): /\* (.*) \*/", src, __import__("re").M)}
    imports = {v & ~1 for n, v in d["functions"].items() if v & ~1 not in recompiled}
    lib.recomp_run.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.recomp_import_name.restype = ctypes.c_char_p
    arena = mmap.mmap(-1, 1 << 32, flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS)
    arena_addr = ctypes.addressof(ctypes.c_char.from_buffer(arena))
    lib.recomp_set_mem(ctypes.c_void_p(arena_addr))
    CB = ctypes.CFUNCTYPE(None, ctypes.POINTER(Cpu), ctypes.c_uint32)

    def external(cp, addr):
        if os.environ.get("RECOMP_TRACE"):
            nm = lib.recomp_import_name(addr)
            print(f"   external call {addr:#x} {nm.decode() if nm else ''}", file=sys.stderr)
        cp.contents.r[0] = 0
    cb = CB(external)
    lib.recomp_set_external(cb)
    ok = bad = skip = untested = 0
    results = {}
    for fn in recompiled:
        if only and fn not in only:
            continue
        rng = random.Random(fn)
        verdict = "ok"
        compared = 0
        for t in range(trials):
            st = state(rng)
            if os.environ.get("DIFFTEST_NOFORK"):   # debugging a native crash: run in-process (under lldb)
                a = run_unicorn(blocks, imports, fn, st)
                if not a["err"]:
                    print(fn, t, compare(a, run_native(lib, arena_addr, blocks, fn, st)))
                continue
            rd, wr = os.pipe()
            pid = os.fork()
            if pid == 0:
                os.close(rd)
                try:
                    a = run_unicorn(blocks, imports, fn, st)
                    if a["err"]:
                        res = ("skip", a["err"])
                    else:
                        b = run_native(lib, arena_addr, blocks, fn, st)
                        res = ("unimpl", f"{b['unimpl']:#x}") if b["unimpl"] is not None else (
                            ("diff", compare(a, b)) if compare(a, b) else ("ok", ""))
                except Exception as e:
                    res = ("error", str(e))
                os.write(wr, json.dumps(res).encode()); os._exit(0)
            os.close(wr)
            import select
            r, _, _ = select.select([rd], [], [], 60)   # well above the 5 s Unicorn cap: only a native hang gets here
            if r:
                data = os.read(rd, 1 << 16); os.waitpid(pid, 0)
                kind, msg = json.loads(data) if data else ("crash", "native code crashed")
            else:
                os.kill(pid, 9); os.waitpid(pid, 0); kind, msg = "hang", "timeout"
            os.close(rd)
            if kind in ("diff", "unimpl", "error", "crash", "hang"):
                verdict = f"{kind} (trial {t}): {msg}"
                if stepmode and kind == "diff":
                    verdict += "\n    " + lockstep(lib, arena_addr, blocks, imports, fn, st, insn_text)
                break
            if kind == "skip":
                skip += 1
                continue
            compared += 1
        if verdict == "ok" and not compared:
            verdict = "untested (every random state was skipped)"
        results[fn] = verdict
        if verdict == "ok":
            ok += 1
        elif verdict.startswith("untested"):
            untested += 1
        else:
            bad += 1
        print(f"fn_{fn:08x}: {verdict}", flush=True)
    print(f"{ok} functions identical over {trials} random states, {bad} not, {untested} untested"
          f" ({skip} trials skipped: the original didn't return from its random state)")


if __name__ == "__main__":
    main()
