#!/usr/bin/env python3
"""fixture_neon_emitters.py -- the NEON/ARM emitter additions of recompiler/recomp.py in
isolation, EXECUTED: ssat/usat, vzip.8, vqmovn/vqmovun, vqshrn/vqshrun, vmlal/vmlsl, vqabs.s8.

Upstream's emitter raises Unsupported for these instructions, so the generated bodies hold
UNIMPL(addr) and a game that uses them runs into the unimplemented trap. The fixture builds a
synthetic module of one-instruction guest functions from clang-assembled Thumb-2 encodings
(self-verified with the capstone pass recomp.py uses), runs recomp.py over it, compiles the
generated C together with fixture_neon_harness.c and executes every case against hand-computed
results (saturation boundaries, lane order) -- the emitter semantics, not just its text.

Run:  python3 fixture_neon_emitters.py            (this tree's recomp.py: expect PASS, exit 0)
      python3 fixture_neon_emitters.py --old      (pre-fix emitter: expect FAIL, exit 1)

--old reads recompiler/recomp.py from the base commit through `git show`, so the control is the
exact upstream emitter, not a re-implementation.
"""
import glob, json, os, re, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
NATIV = os.path.abspath(os.path.join(HERE, os.pardir, os.pardir))
RECOMP = os.path.join(HERE, os.pardir, "recomp.py")
BASE = "4b1c428"   # --old: the pre-fix recompiler/recomp.py of this base
OLD = "--old" in sys.argv

# clang-assembled Thumb-2 encodings (llvm-mingw clang -target thumbv7a-none-eabi -mfpu=neon),
# each one instruction followed by bx lr (7047). The expected decode is capstone's text,
# normalized (hex immediates to decimal) for a stable comparison.
CASES = [
    (0x81000000, "01f30700", "ssat r0, #8, r1"),
    (0x81000010, "81f30800", "usat r0, #8, r1"),
    (0x81000020, "b2ff8202", "vqmovn.s16 d0, q1"),
    (0x81000030, "b2ff4202", "vqmovun.s16 d0, q1"),
    (0x81000040, "b2ffc202", "vqmovn.u16 d0, q1"),
    (0x81000050, "b2ff8101", "vzip.8 d0, d1"),
    (0x81000060, "92ef0308", "vmlal.s16 q0, d2, d3"),
    (0x81000070, "8cef1209", "vqshrn.s16 d0, q1, #4"),
    (0x81000080, "8cff1208", "vqshrun.s16 d0, q1, #4"),
    (0x81000090, "b0ff0107", "vqabs.s8 d0, d1"),
]
BX_LR = "7047"

def norm(s):
    s = s.lower().strip()
    s = re.sub(r"#0x([0-9a-f]+)", lambda m: "#" + str(int(m.group(1), 16)), s)
    return re.sub(r"\s+", " ", s)

# ---- self-verify the encodings with the same decoder recomp.py uses ------------------
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
enc_fail = 0
for addr, hx, want in CASES:
    blob = bytes.fromhex(hx + BX_LR)
    insns = list(md.disasm(blob, addr))
    got = norm(insns[0].mnemonic + " " + insns[0].op_str) if insns else "(undecoded)"
    ok = got == norm(want)
    if not ok:
        enc_fail += 1
        print(f"ENCODING MISMATCH at {addr:#x}: want '{want}' capstone says '{got}'")
if enc_fail:
    print(f"{enc_fail} encodings wrong -- fix CASES before trusting any result")
    sys.exit(2)

# ---- the synthetic module -----------------------------------------------------------
d = {"blocks": [], "functions": {}, "bodies": [], "relocs": [],
     "module": "fixture", "exports": [], "variables": [], "arm": []}
for addr, hx, _ in CASES:
    blob = bytes.fromhex(hx + BX_LR)
    d["blocks"].append({"start": addr, "size": len(blob), "init": True, "bytes": blob.hex()})
    d["functions"]["FUN_%08x" % addr] = addr
    d["bodies"].append([addr, addr + len(blob) - 1])
    d["blocks"].append({"start": 0x81C00000 + addr - 0x81000000, "size": 4, "init": True,
                        "bytes": struct.pack("<I", addr | 1).hex()})   # Thumb-bit mode truth
with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
    json.dump(d, f); js = f.name

# ---- the pre-fix control: the exact upstream emitter from the base commit ------------
recomp_run = RECOMP
if OLD:
    up = os.path.join(HERE, "_upstream_recomp_tmp.py")
    r = subprocess.run(["git", "-C", NATIV, "show", f"{BASE}:recompiler/recomp.py"],
                       capture_output=True, text=True)
    if r.returncode == 0:
        open(up, "w").write(r.stdout)
    elif os.path.exists(up):
        print("note: git cannot read the tree here; using the pre-extracted", up)
    else:
        print("cannot read base recomp.py:", r.stderr.strip()); sys.exit(2)
    recomp_run = up

out_c = os.path.join(HERE, "fixture_neon_out.c")
for junk in [out_c, out_c[:-2] + ".h"] + glob.glob(out_c[:-2] + "_part*.c"):
    if os.path.exists(junk):
        os.remove(junk)
r = subprocess.run([sys.executable, recomp_run, js, out_c, "--split", "2"],
                   capture_output=True, text=True)
print((r.stdout + r.stderr).strip()[-400:])
if not os.path.exists(out_c):
    print("FAIL: the emitter produced no C")
    sys.exit(1)

# ---- compile the generated code with the harness and execute the cases ---------------
binary = os.path.join(tempfile.gettempdir(), "fixture_neon_emitters")
cc = ["cc", "-O1", "-g", "-Wall", "-D_GNU_SOURCE", "-I", os.path.join(HERE, os.pardir),
      os.path.join(HERE, "fixture_neon_harness.c"), out_c] + sorted(glob.glob(out_c[:-2] + "_part*.c")) \
     + ["-o", binary]
r = subprocess.run(cc, capture_output=True, text=True)
if r.returncode != 0:
    print("FAIL: the generated C + harness did not compile:\n" + (r.stdout + r.stderr)[-2000:])
    sys.exit(1)
r = subprocess.run([binary], capture_output=True, text=True)
print((r.stdout + r.stderr).strip())
print("EMITTER:", "pre-fix upstream (%s)" % BASE if OLD else "this tree")
sys.exit(r.returncode)
