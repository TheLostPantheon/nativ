#!/usr/bin/env python3
"""The recompiler, end to end, with no game: C programs (recomp_exec/progs.c) compiled for the Vita's CPU (Thumb-2, Cortex-A9)
by clang, recompiled by recompiler/recomp.py, built with dev/glue.c, and every t_* function run on random inputs. Each result
(r0, and a checksum of the program's data) must match Unicorn running the original code. The recompiled code runs twice:
as built (registers as C locals, recomp.py's REG_LOCALS) and with --no-reg-locals, so a change to how functions are
emitted is checked against the plain translation and the CPU itself.

    dev/tests/recomp_exec_test.py [cases per function]      (default 100; needs clang with the ARM target, unicorn,
                                                             pyelftools and capstone: requirements.txt)
Prints "N checks, M failed" and exits 1 on any mismatch. Thumb at -O1 and -O2, and ARM at -O2, give three code shapes."""
import os, random, struct, subprocess, sys, tempfile, json
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UcError
from unicorn.arm_const import *

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CC = os.environ.get("CC", "clang")
BASES = {".text": 0x81000000, ".rodata": 0x81100000, ".data": 0x81200000, ".bss": 0x81300000}
STACK_TOP, RETURN = 0x82000000, 0x7fff0000   # (the stack grows down from STACK_TOP; a return to RETURN ends a run)


def compile_progs(mode, opt, out):
    subprocess.run([CC, "--target=armv7a-none-eabi", "-mcpu=cortex-a9", mode, "-mfpu=neon", "-mfloat-abi=softfp", opt, "-mno-movt",
                    "-ffreestanding", "-fno-builtin", "-fno-pic", "-fno-stack-protector", "-fno-unwind-tables",
                    "-fno-asynchronous-unwind-tables", "-fno-common", "-c", os.path.join(HERE, "recomp_exec", "progs.c"), "-o", out], check=True)


def section_key(name):
    for k in BASES:
        if name == k or name.startswith(k + "."): return k
    return None


def link(obj):
    """the object's sections laid out at BASES, its relocations applied: {base: bytes}, {function: (address|1, size)}"""
    elf = ELFFile(open(obj, "rb"))
    place, data, cursor = {}, {k: bytearray() for k in BASES}, {}
    for i, s in enumerate(elf.iter_sections()):
        k = section_key(s.name)
        if not k or not s["sh_size"]: continue
        buf = data[k]; align = max(s["sh_addralign"], 4)
        buf.extend(b"\0" * ((-len(buf)) % align))
        place[i] = BASES[k] + len(buf)
        buf.extend(b"\0" * s["sh_size"] if s["sh_type"] == "SHT_NOBITS" else s.data())
    symtab = elf.get_section_by_name(".symtab")
    def sym_addr(sym):
        sec = sym["st_shndx"]
        if isinstance(sec, str): raise SystemExit(f"undefined symbol {sym.name}")
        return place[sec] + sym["st_value"]   # (a Thumb function's value carries bit 0)
    fns = {}
    for sym in symtab.iter_symbols():
        if sym["st_info"]["type"] == "STT_FUNC" and sym["st_shndx"] in place:
            fns[sym.name] = (sym_addr(sym), sym["st_size"])
    for rs in elf.iter_sections():
        if not isinstance(rs, RelocationSection) or rs["sh_info"] not in place: continue
        base = place[rs["sh_info"]]; k = section_key(elf.get_section(rs["sh_info"]).name); buf = data[k]
        for r in rs.iter_relocations():
            at = base + r["r_offset"] - BASES[k]; t = r["r_info_type"]
            sym = symtab.get_symbol(r["r_info_sym"]); s = sym_addr(sym)
            if t == 2:   # R_ARM_ABS32: S + A (| T, carried by a Thumb symbol's value)
                a, = struct.unpack_from("<I", buf, at); struct.pack_into("<I", buf, at, (s + a) & 0xffffffff)
            elif t in (28, 29):   # R_ARM_CALL / R_ARM_JUMP24: an ARM BL / B to S (same mode: no BLX here)
                w, = struct.unpack_from("<I", buf, at); p = base + r["r_offset"]
                struct.pack_into("<I", buf, at, (w & 0xff000000) | ((((s & ~1) - (p + 8)) >> 2) & 0xffffff))
            elif t in (10, 30):   # R_ARM_THM_CALL / THM_JUMP24: a BL / B.W to S
                hi, lo = struct.unpack_from("<HH", buf, at)
                p = base + r["r_offset"]; off = ((s & ~1) - (p + 4)) & 0x1ffffff
                sgn = off >> 24; i1 = (off >> 23) & 1; i2 = (off >> 22) & 1; j1 = (~(i1 ^ sgn)) & 1; j2 = (~(i2 ^ sgn)) & 1
                hi = (hi & 0xf800) | (sgn << 10) | ((off >> 12) & 0x3ff)
                lo = (lo & 0xd000) | (j1 << 13) | (j2 << 11) | ((off >> 1) & 0x7ff)
                if t == 10 and not (s & 1): lo &= ~0x1000   # (BLX to ARM code: none here)
                struct.pack_into("<HH", buf, at, hi, lo)
            elif t == 102:   # R_ARM_THM_JUMP11: a 16-bit B to S
                hw, = struct.unpack_from("<H", buf, at); p = base + r["r_offset"]
                struct.pack_into("<H", buf, at, (hw & 0xf800) | ((((s & ~1) - (p + 4)) >> 1) & 0x7ff))
            elif t == 51:   # R_ARM_THM_JUMP19: a conditional B.W to S
                hi, lo = struct.unpack_from("<HH", buf, at); p = base + r["r_offset"]; off = ((s & ~1) - (p + 4)) & 0x1fffff
                hi = (hi & 0xfbc0) | ((off >> 20) << 10) | ((off >> 12) & 0x3f)
                lo = (lo & 0xd000) | (((off >> 18) & 1) << 13) | (((off >> 19) & 1) << 11) | ((off >> 1) & 0x7ff)
                struct.pack_into("<HH", buf, at, hi, lo)
            else:
                raise SystemExit(f"relocation type {t} at {r['r_offset']:#x} ({sym.name}): not handled")
    return {BASES[k]: bytes(v) for k, v in data.items() if v}, fns


def recompile(image, fns, out_c, flags):
    d = {"blocks": [{"start": a, "size": len(b), "init": True, "bytes": b.hex()} for a, b in image.items()],
         "functions": {n: a for n, (a, _) in fns.items()}, "bodies": [[a & ~1, (a & ~1) + max(sz, 2) - 1] for a, sz in fns.values()],
         "relocs": [], "module": "progs", "exports": [], "variables": [], "arm": [a for a, _ in fns.values() if not a & 1]}
    js = out_c + ".json"; json.dump(d, open(js, "w"))
    py = os.path.join(ROOT, ".venv", "bin", "python"); py = py if os.path.exists(py) else sys.executable
    r = subprocess.run([py, os.path.join(ROOT, "recompiler", "recomp.py"), js, out_c] + flags, capture_output=True, text=True)
    if r.returncode: raise SystemExit("recomp.py failed:\n" + r.stdout[-2000:] + r.stderr[-2000:])


HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "rt.h"
void recomp_set_mem(uint8_t *m); void recomp_runfast(void); void recomp_run(Cpu *c, uint32_t entry);
extern uint32_t recomp_unimpl_at;
int main(int argc, char **argv) {
	uint8_t *mem = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	if (mem == MAP_FAILED) return 2;
	recomp_set_mem(mem); recomp_runfast();
	FILE *img = fopen(argv[1], "rb"), *cases = fopen(argv[2], "r");
	static uint8_t pristine[0x400000]; uint32_t base[8], len[8]; int nb = 0;
	while (nb < 8 && fread(&base[nb], 4, 1, img) == 1 && fread(&len[nb], 4, 1, img) == 1) { if (fread(mem + base[nb], 1, len[nb], img) != len[nb]) return 3; nb++; }
	uint32_t dlo = 0x81200000u, dhi = 0x81400000u; memcpy(pristine, mem + dlo, dhi - dlo);
	unsigned fn, a, b, c3, d;
	while (fscanf(cases, "%x %x %x %x %x", &fn, &a, &b, &c3, &d) == 5) {
		memcpy(mem + dlo, pristine, dhi - dlo);
		Cpu c; memset(&c, 0, sizeof c); c.r[0] = a; c.r[1] = b; c.r[2] = c3; c.r[3] = d; c.r[13] = 0x82000000u; c.r[14] = 0x7fff0001u; c.fpscr = 0x03000000;
		recomp_unimpl_at = 0;
		recomp_run(&c, fn & ~1u);
		uint32_t h = 2166136261u;   /* (the data and bss blocks, as written) */
		for (int k = 0; k < nb; k++) if (base[k] >= dlo) for (uint32_t i = base[k]; i < base[k] + len[k]; i++) h = (h ^ mem[i]) * 16777619u;
		printf("%08x %08x%s\n", c.r[0], h, recomp_unimpl_at ? " unimplemented" : "");
	}
	return 0;
}
'''


def build(c_file, tmp, name):
    h = os.path.join(tmp, name + "_harness.c"); open(h, "w").write(HARNESS)
    exe = os.path.join(tmp, name)
    subprocess.run([CC, "-O1", "-ffp-contract=off", "-w", "-I", os.path.join(ROOT, "recompiler"), c_file, os.path.join(ROOT, "dev", "glue.c"), h,
                    "-o", exe, "-lm"], check=True)
    return exe


def run_recompiled(exe, img_file, cases, tmp):
    cf = os.path.join(tmp, "cases.txt")
    open(cf, "w").write("".join(f"{fn:x} {a:x} {b:x} {c:x} {d:x}\n" for fn, a, b, c, d in cases))
    out = subprocess.run([exe, img_file, cf], capture_output=True, text=True, timeout=600)
    if out.returncode: raise SystemExit(f"{exe} failed ({out.returncode}): {out.stderr[-500:]}")
    return out.stdout.split("\n")[:len(cases)]


def run_unicorn(image, cases):
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    for a in BASES.values(): uc.mem_map(a, 0x100000)
    uc.mem_map(STACK_TOP - 0x100000, 0x100000); uc.mem_map(RETURN, 0x1000)
    uc.reg_write(UC_ARM_REG_C1_C0_2, uc.reg_read(UC_ARM_REG_C1_C0_2) | (0xf << 20)); uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
    out = []
    for fn, a, b, c, d in cases:
        for addr, data in image.items(): uc.mem_write(addr, data)
        for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), (a, b, c, d)): uc.reg_write(r, v)
        uc.reg_write(UC_ARM_REG_SP, STACK_TOP); uc.reg_write(UC_ARM_REG_LR, RETURN | 1); uc.reg_write(UC_ARM_REG_FPSCR, 0x03000000)
        try: uc.emu_start(fn, RETURN, count=50_000_000)   # (bit 0: Thumb)
        except UcError as e: out.append(f"unicorn error {e}"); continue
        mem = b"".join(bytes(uc.mem_read(base, len(data))) for base, data in sorted(image.items()) if base >= 0x81200000)
        h = 2166136261
        for x in mem: h = ((h ^ x) * 16777619) & 0xffffffff
        out.append(f"{uc.reg_read(UC_ARM_REG_R0):08x} {h:08x}")
    return out


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 100
    checks = failed = 0
    with tempfile.TemporaryDirectory(prefix="recomp_exec_") as tmp:
        for mode, opt in (("-mthumb", "-O1"), ("-mthumb", "-O2"), ("-marm", "-O2")):
            obj = os.path.join(tmp, "progs" + mode + opt + ".o"); compile_progs(mode, opt, obj); opt = mode[2:] + " " + opt
            image, fns = link(obj)
            tests = sorted((name, a) for name, (a, _) in fns.items() if name.startswith("t_"))
            rng = random.Random(1234)
            cases = [(a, *(rng.choice((0, 1, 2, 0xffffffff, 0x80000000, rng.getrandbits(8), rng.getrandbits(32))) for _ in range(4)))
                     for _, a in tests for _ in range(n)]
            img_file = os.path.join(tmp, "image_" + opt.replace(" ", "_") + ".bin")
            with open(img_file, "wb") as f:
                for a, b in image.items(): f.write(struct.pack("<II", a, len(b)) + b)
            results = {"unicorn": run_unicorn(image, cases)}
            for variant, flags in (("locals", []), ("plain", ["--no-reg-locals"])):
                tag = opt.replace(" ", "_").replace("-", "")
                c_file = os.path.join(tmp, f"progs_{tag}_{variant}.c"); recompile(image, fns, c_file, flags)
                results[variant] = run_recompiled(build(c_file, tmp, f"progs_{tag}_{variant}"), img_file, cases, tmp)
            names = {a: name for name, a in tests}
            bad = {}
            for k, case in enumerate(cases):
                for variant in ("locals", "plain"):
                    checks += 1
                    if results[variant][k] != results["unicorn"][k]:
                        failed += 1; bad.setdefault((names[case[0]], variant), (case, results[variant][k], results["unicorn"][k]))
            for (name, variant), (case, got, want) in sorted(bad.items()):
                print(f"FAIL {opt} {name} ({variant}): args {' '.join(f'{x:#x}' for x in case[1:])} -> {got}, the CPU gives {want}")
            print(f"{opt}: {len(tests)} functions x {n} inputs: {'ok' if not bad else str(len(bad)) + ' function(s) differ'}")
    print(f"{checks} checks, {failed} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
