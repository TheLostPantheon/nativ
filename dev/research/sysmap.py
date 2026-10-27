#!/usr/bin/env python3
"""The system-call map: for every system call the given games need (platmap.py's list), its kernel
implementation -- module, size, the kernel services it reaches -- classified LOGIC (it reaches only
bookkeeping services: memory, threads, sync, string/clib helpers -- portable from Sony's own logic)
or DEVICE (it reaches interrupts, DMA, the GPU, syscon, display, audio or other hardware drivers --
emulated), and the SCE error codes it can return (0x80xxxxxx constants in its code, named from the
SDK headers), so stubs can return what the console returns instead of 0.

  sysmap.py <game elf dir> [...] [--out syscalls.md]

Derived from Sony code: keep the map local (under dump/).
"""
import glob, os, re, struct, sys
from collections import defaultdict
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, "..", "..", "recompiler"))   # (platmap beside this; analyze in the recompiler)
from platmap import build_index, classifier, nid_names, host_names, Mod, DEC
from analyze import Module
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
from capstone import arm as A

# kernel libraries that are bookkeeping, not hardware (a call reaching only these is LOGIC)
LOGIC = re.compile(r"^Sce(Sysmem|Threadmgr|Sysclib|SysclibFor|Cpu|ModulemgrFor|Processmgr|Iofilemgr|KernelUtils|Debug|Excpmgr|Sblacmgr|SblAimgr|SblAuthMgr|DebugLed|Rtc|SysrootFor|Sysroot|Kernel|Intrmgr(?!For))", re.I)
# (interrupt masking and DMA memcpy are trivial to emulate: not counted as devices)
DEVICE_HINT = re.compile(r"Gpu|Syscon|Display|Lcd|Oled|Hdmi|Audio|Codec|Camera|Ctrl|Touch|Motion|Power|Pervasive|Gpio|I2c|Spi|Uart|Msif|Sdif|Usb|Wlan|Bt|Compat|Avcodec|Ngs|Mgvideo|Pcbc|Udcd|Lowio", re.I)


def error_names(root=os.path.expanduser("~/Dev/vita/vita-headers/include")):
    """0x80xxxxxx -> SCE_..._ERROR_... name, from the headers' enums and defines"""
    out = {}
    for p in glob.glob(os.path.join(root, "**", "*.h"), recursive=True):
        for m in re.finditer(r"\b(SCE_\w*ERROR\w*)\s*(?:=|\s)\s*\(?\s*(?:\(int\)\s*)?(0x8[0-9A-Fa-f]{7})", open(p, errors="replace").read()):
            out.setdefault(int(m.group(2), 16), m.group(1))
    return out


def constants(mod, seen):
    """the 0x80xxxxxx values an instruction set builds: literal-pool loads and movw/movt pairs"""
    code, base, hi = mod.m.mem[mod.m.loads[0][2]], mod.m.loads[0][2], mod.m.loads[0][2] + mod.m.loads[0][4]
    out = set()
    by_mode = defaultdict(list)
    for a, (size, arm) in seen.items():
        by_mode[arm].append(a)
    for arm, addrs in by_mode.items():
        md = Cs(CS_ARCH_ARM, CS_MODE_ARM if arm else CS_MODE_THUMB); md.detail = True
        movw = {}
        for a in sorted(addrs):
            ins = list(md.disasm(bytes(code[a - base:a - base + 4]), a, 1))
            if not ins: continue
            i = ins[0]; ops = i.operands
            if i.mnemonic.startswith("ldr") and len(ops) == 2 and ops[1].type == A.ARM_OP_MEM and ops[1].mem.base == A.ARM_REG_PC:
                lit = ((a + (8 if arm else 4)) & ~3) + ops[1].mem.disp
                if base <= lit < hi - 4:
                    v, = struct.unpack_from("<I", code, lit - base)
                    if v >> 24 == 0x80: out.add(v)
            elif i.mnemonic.startswith("movw") and ops[1].type == A.ARM_OP_IMM:
                movw[ops[0].reg] = ops[1].imm & 0xffff
            elif i.mnemonic.startswith("movt") and ops[1].type == A.ARM_OP_IMM and ops[0].reg in movw:
                v = (ops[1].imm & 0xffff) << 16 | movw.pop(ops[0].reg)
                if v >> 24 == 0x80: out.add(v)
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    outp = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else None
    if outp in args: args.remove(outp)
    if not args:
        print(__doc__); sys.exit(1)
    names, host, errs = nid_names(), host_names(), error_names()
    nm = lambda k: names.get(k, f"{k[0]}_{k[1]:08X}")
    kernel, users = build_index()
    classify, umod = classifier(kernel, users)

    # the system calls the games need (as platmap.py computes them)
    need = defaultdict(set)
    for gdir in args:
        game = os.path.basename(os.path.dirname(os.path.abspath(gdir)))
        elves = glob.glob(os.path.join(gdir, "*.elf"))
        own = {(lib, nid) for p in elves for lib, nid, a, var in Module(p).export_table() if lib}
        imps = {(lib, nid) for p in elves for lib, fns, _ in Module(p).imports() for nid, _ in fns} - own
        for key in imps:
            if key in kernel: need[key].add(game)
            elif key in users:
                for k in classify(key)[0]: need[k].add(game)

    # kernel module paths by name, and each kernel export's address
    kpaths = {}
    for p in glob.glob(os.path.join(DEC, "**", "*.skprx.elf"), recursive=True):
        try: kpaths.setdefault(Module(p).name, p)
        except Exception: pass
    kmods = {}
    def kmod(name):
        if name not in kmods:
            try: kmods[name] = Mod(kpaths[name]) if name in kpaths else None
            except Exception as e: kmods[name] = None; print(f"  {name}: {e}", file=sys.stderr)
        return kmods[name]

    rows = []
    for key in sorted(need, key=lambda k: (kernel[k], nm(k))):
        m = kmod(kernel[key])
        if m is None or key not in m.exports:
            rows.append((key, kernel[key], 0, "?", [], [])); continue
        a = m.exports[key]
        # its whole body within the module, and the kernel services (imports) it reaches
        seen_all, imps, todo, visited = {}, set(), [(a & ~1, not a & 1)], set()
        while todo:
            f, arm = todo.pop()
            if f in visited: continue
            visited.add(f)
            if f in m.stub: imps.add(m.stub[f]); continue
            r = m.an.function(f, arm, m.entries)
            if r is None: continue
            for x, sz in r[0].items(): seen_all[x] = (sz, arm)
            todo += [(t & ~1, tarm) for t, tarm in r[1]]
        libs = sorted({lib for lib, nid in imps})
        device = [l for l in libs if DEVICE_HINT.search(l) and not LOGIC.search(l)]
        cls = "DEVICE" if device else "LOGIC"
        codes = sorted(constants(m, seen_all))
        rows.append((key, kernel[key], sum(s for s, _ in seen_all.values()), cls, device or libs, codes))

    w = (open(outp, "w") if outp else sys.stdout).write
    n_logic = sum(r[3] == "LOGIC" for r in rows)
    w(f"# System-call map\n\nGenerated by `dev/research/sysmap.py`. {len(rows)} system calls the games need: **{n_logic} LOGIC** "
      f"(only bookkeeping services underneath: port Sony's logic), **{sum(r[3] == 'DEVICE' for r in rows)} DEVICE** (they reach hardware drivers: emulate), "
      f"{sum(r[3] == '?' for r in rows)} unknown (kernel module not in the dump).\n\n")
    w("| system call | kernel module | size | class | services reached (device ones, if any) | error codes | games | host today |\n|---|---|---|---|---|---|---|---|\n")
    for key, kname, size, cls, libs, codes in rows:
        ec = ", ".join(f"`{errs.get(c, hex(c))}`" for c in codes[:8]) + (f" +{len(codes) - 8}" if len(codes) > 8 else "")
        w(f"| `{nm(key)}` | {kname} | {size} | {cls} | {', '.join(libs[:6])} | {ec or '-'} | {len(need[key])} | {'yes' if nm(key) in host else '-'} |\n")


if __name__ == "__main__":
    main()
