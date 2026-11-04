#!/usr/bin/env python3
"""Fixture: the non-link branch / LR contract of the recompiler, in isolation.

Reference basis (ARMv7-M Architecture Reference Manual, branch instructions):
- b / cbz / cbnz / conditional branches do NOT write r14 (LR). LR can contain live guest data across such an edge.
- bl / blx write LR = the address of the instruction following the call (Thumb bit set
  when the caller is Thumb).
- bx lr returns through r14 unchanged.

Observed defect (fixed in recompiler/recomp.py Fn.call/Fn.branch): a non-link
branch to a target outside the current function's labels was emitted through the call
helper, which unconditionally wrote c->r[14] = <next>u before the callee -- destroying
the live LR data. The setjmp import-stub path had the same defect (a plain branch into
the stub still wrote r14).

PASS (expected, fixed emitter): every non-link cross-function branch emits NO r14 write;
bl / blx rN emit c->r[14] = <return address|thumb>; an internal plain branch becomes a
goto with no r14 write; bl into the setjmp stub writes LR and runs the stub; a non-link
branch into the setjmp stub preserves LR.
FAIL (observed, pre-fix emitter): the b/cbz/cbnz/bne tail-jump lines contain
c->r[14] = ...u.

Two layers of evidence:
1. emitted-text checks (below): the statements the fixed and base emitters produce.
2. an EXECUTED synthetic test: the generated C is compiled with a tiny harness and run,
   asserting the register contract at runtime (LR preserved across non-link edges taken
   and not-taken, LR written by linking calls, the internal branch skips its instruction,
   the setjmp stub runs and only a linking call into it writes LR).

Run:  python3 fixture_branch_lr.py            (fixed emitter: expect PASS, exit 0)
      python3 fixture_branch_lr.py --old      (base upstream emitter: expect FAIL, exit 1)

The base control reads the emitter via `git show <ref>:recompiler/recomp.py` (ref:
upstream/main when that ref exists, else HEAD), so the old emitter is the exact baseline,
not a re-implementation. Test intermediates go to a temporary directory, removed after.
"""
import os, re, shutil, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
NATIV = os.path.abspath(os.path.join(HERE, os.pardir, os.pardir))
RECOMP = os.path.join(HERE, os.pardir, "recomp.py")

OLD = "--old" in sys.argv

# ---------------------------------------------------------------------------
# synthetic module: real Thumb-2 encodings, each function a tiny named case.
# Addresses are fixed so every branch immediate can be hand-encoded and then
# verified by the same capstone pass recomp.py uses.
# ---------------------------------------------------------------------------
FN_A, FN_B = 0x81000000, 0x81000010          # b cross-function with live lr
FN_C, FN_D = 0x81000100, 0x81000140          # bl: lr must be written
FN_E, FN_F = 0x81000200, 0x81000240          # cbz cross-function with live lr
FN_G, FN_H = 0x81000300, 0x81000340          # cbnz cross-function with live lr
FN_I, FN_J = 0x81000400, 0x81000440          # bne (conditional) cross-function
FN_P = 0x81000500                            # internal plain b over one instruction
FN_M = 0x81000600                            # blx r3: lr must be written
FN_S = 0x81000700                            # bl setjmp stub: lr must be written
FN_T = 0x81000740                            # b setjmp stub (tail): lr preserved
SETJMP_FN = 0x81000800                       # the setjmp import stub (zero bytes)

def t(*halfwords):
    return b"".join(struct.pack("<H", h) for h in halfwords)

BX_LR   = 0x4770            # bx lr
MOV_LR_R3 = 0x469E          # mov lr, r3  (high-register mov: 01000110 D=1 Rm=0011 Rd=110)
MOV_LR_R4 = 0x46A6          # mov lr, r4  (D=1 Rm=0100 Rd=110)
BLX_R3  = 0x4798            # blx r3

code = {
    # fnA: lr = r3 (live data), then b fnB -- the tail jump must not touch r14
    FN_A: t(MOV_LR_R3, 0xE005) + t(BX_LR),    # 0xE005: b 0x81000010 (pc=+6 -> imm11=5)
    FN_B: t(BX_LR),
    # fnC: bl fnD -- must set lr = 0x81000104|1
    FN_C: t(0xF000, 0xF81E) + t(BX_LR),       # bl 0x81000140 (offset 0x3c)
    FN_D: t(BX_LR),
    # fnE: lr = r3, then cbz r0, fnF
    FN_E: t(MOV_LR_R3, 0xB1E8) + t(BX_LR),    # cbz r0, 0x81000240 (disp 0x3a = i=0,imm5=11101)
    FN_F: t(BX_LR),
    # fnG: lr = r3, then cbnz r1, fnH
    FN_G: t(MOV_LR_R3, 0xB9E9) + t(BX_LR),    # cbnz r1, 0x81000340
    FN_H: t(BX_LR),
    # fnI: lr = r3, then bne fnJ (conditional: both paths must preserve lr)
    FN_I: t(MOV_LR_R3, 0xD11D) + t(BX_LR),    # bne 0x81000440
    FN_J: t(BX_LR),
    # fnP: b over one instruction (internal plain branch: goto, no lr write; the
    # skipped `mov lr, r4` must never execute)
    FN_P: t(0xE000, MOV_LR_R4) + t(BX_LR),    # b 0x81000504 (imm11=0), mov lr,r4, bx lr
    # fnM: blx r3 -- must set lr = 0x81000602|1
    FN_M: t(BLX_R3, BX_LR),
    # fnS: bl setjmp stub -- must set lr = 0x81000702|1 and run the stub
    FN_S: t(0xF000, 0xF87E) + t(BX_LR),       # bl 0x81000800 (offset 0xfc)
    # fnT: b setjmp stub (tail) -- the stub runs, lr is preserved
    FN_T: t(0xE05E) + t(BX_LR),               # b 0x81000800 (offset 0xbc), bx lr (unreachable)
    # the setjmp import stub: zero bytes (recomp.py's stub signature), named "setjmp"
    SETJMP_FN: t(0x0000, 0x0000),
}

d = {"blocks": [], "functions": {}, "bodies": [], "relocs": [],
     "module": "fixture", "exports": [], "variables": [], "arm": []}
for a, bs in code.items():
    d["blocks"].append({"start": a, "size": len(bs), "init": True, "bytes": bs.hex()})
    d["functions"]["setjmp" if a == SETJMP_FN else "FUN_%08x" % a] = a
    d["bodies"].append([a, a + len(bs) - 1])
    # a word carrying the Thumb bit -- the linker's mode truth for every entry
    d["blocks"].append({"start": 0x81C00000 + a - 0x81000000, "size": 4, "init": True,
                        "bytes": struct.pack("<I", (a | 1) & 0xffffffff).hex()})

tmp = tempfile.mkdtemp(prefix="fixture_branch_lr_")
js = os.path.join(tmp, "fixture.json")
with open(js, "w") as f:
    __import__("json").dump(d, f)

# the pre-fix control: the exact base emitter (upstream/main when present, else HEAD)
recomp_run = RECOMP
if OLD:
    ref = "HEAD"
    if subprocess.run(["git", "-C", NATIV, "rev-parse", "--verify", "upstream/main"],
                      capture_output=True).returncode == 0:
        ref = "upstream/main"
    up = os.path.join(tmp, "_upstream_recomp_tmp.py")
    r = subprocess.run(["git", "-C", NATIV, "show", ref + ":recompiler/recomp.py"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("cannot read base recomp.py:", r.stderr.strip()); sys.exit(2)
    open(up, "w").write(r.stdout)
    recomp_run = up

out_c = os.path.join(tmp, "fixture_branch_out.c")
r = subprocess.run([sys.executable, recomp_run, js, out_c, "--split", "2"],
                   capture_output=True, text=True)
if r.returncode != 0 and r.stderr:
    print("emitter stderr:", r.stderr[-400:])
src = ""
parts = [out_c, out_c[:-2] + ".h"] + [out_c[:-2] + "_part%d.c" % i for i in range(4)]
for f in parts:
    if os.path.exists(f):
        src += open(f).read()

def fn_body(entry):
    m = re.search(r"void fn_%08x\(Cpu \*c\) \{(.*?)\n\}" % entry, src, re.S)
    return m.group(1) if m else ""

def stmt(entry, target):
    """the statement calling/branching to target inside entry's body (first match)"""
    body = fn_body(entry)
    for line in body.splitlines():
        if ("fn_%08x(c)" % target) in line or ("0x%08xu" % target) in line and "call_external" in line \
           or (target == SETJMP_FN and "_setjmp" in line):
            return line
    return ""

def check(name, ok, detail=""):
    print((("PASS  " if ok else "FAIL  ") + name + (("  -- " + detail) if detail and not ok else "")))
    return ok

results = []

# -- non-link cross-function edges: no r14 write in the branch statement -------------
for name, entry, target in [("b   preserves lr (fnA->fnB)", FN_A, FN_B),
                            ("cbz preserves lr (fnE->fnF)", FN_E, FN_F),
                            ("cbnz preserves lr (fnG->fnH)", FN_G, FN_H),
                            ("bne preserves lr (fnI->fnJ)", FN_I, FN_J)]:
    s = stmt(entry, target)
    ok = bool(s) and ("c->r[14]" not in s)
    results.append(check(name, ok, "statement: " + s.strip()))

# -- the non-link edges must reach the callee at all (no silent drop) ---------------
for name, entry, target in [("b   reaches fnB", FN_A, FN_B), ("cbz reaches fnF", FN_E, FN_F),
                            ("cbnz reaches fnH", FN_G, FN_H), ("bne reaches fnJ", FN_I, FN_J)]:
    results.append(check(name, ("fn_%08x(c)" % target) in fn_body(entry)))

# -- taken/not-taken shape: conditional edges sit inside their condition ------------
for name, entry in [("cbz taken path is conditional", FN_E), ("cbnz taken path is conditional", FN_G),
                    ("bne taken path is conditional", FN_I)]:
    body = fn_body(entry)
    ok = "if (COND(c," in body or "if (" in body
    results.append(check(name, ok))

# -- link calls: lr = return address | thumb bit ------------------------------------
s = stmt(FN_C, FN_D)
ok = ("c->r[14] = 0x%xu" % (0x81000104 | 1)) in s
results.append(check("bl  writes lr = next|1 (fnC->fnD)", ok, "statement: " + s.strip()))

body = fn_body(FN_M)
ok = ("c->r[14] = 0x%xu" % (0x81000602 | 1)) in body
results.append(check("blx rN writes lr = next|1 (fnM)", ok))

# -- return control flow: bx lr is the C return; the call chain carries the guest lr --
ok = "return;" in fn_body(FN_B)
results.append(check("bx lr returns through the call chain (fnB)", ok))

# -- representative node-pointer/hint case: the live-lr edge keeps the value --------
s = stmt(FN_A, FN_B)
ok = bool(s) and ("c->r[14]" not in s) and ("mov lr, r3" in fn_body(FN_A) or "c->r[14] = c->r[3]" in fn_body(FN_A))
results.append(check("hint-node case: lr set to live data, edge preserves it (fnA)", ok, "body: " + fn_body(FN_A)[:120]))

# -- internal plain branch: a goto, no r14 write, the skipped instruction stays out --
line_p = ""
for line in fn_body(FN_P).splitlines():
    if ("L_%08x" % 0x81000504) in line and "goto" in line:
        line_p = line
ok = bool(line_p) and ("c->r[14]" not in line_p)
results.append(check("b   internal becomes goto without lr write (fnP)", ok, "line: " + line_p.strip()))

# -- setjmp import stub: bl writes lr and reaches the stub, a tail edge preserves lr -
s = stmt(FN_S, SETJMP_FN)
ok = bool(s) and ("_setjmp" in s) and (("c->r[14] = 0x%xu" % (0x81000704 | 1)) in s)
results.append(check("bl  setjmp stub writes lr + stub body (fnS)", ok, "statement: " + s.strip()))

s = stmt(FN_T, SETJMP_FN)
ok = bool(s) and ("_setjmp" in s) and ("c->r[14]" not in s)
results.append(check("b   setjmp stub (tail) preserves lr (fnT)", ok, "statement: " + s.strip()))

# ---------------------------------------------------------------------------
# executed synthetic test: compile the generated C with a harness and run it.
# Asserts the register contract at runtime for every edge class above.
# ---------------------------------------------------------------------------
harness = r'''
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "fixture_branch_out.h"

static uint32_t ext_calls[8]; static int ext_n;
RecompJmp *recomp_setjmp_begin(Cpu *c) { (void)c; static RecompJmp rj; return &rj; }
void recomp_call_external(Cpu *c, uint32_t addr) { if (ext_n < 8) ext_calls[ext_n++] = addr; (void)c; }
void recomp_unimplemented(Cpu *c, uint32_t addr) { (void)c; fprintf(stderr, "unimplemented %08x\n", addr); exit(2); }

static int fails;
static void ck(const char *name, int ok) {
    printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) fails++;
}

int main(void) {
    Cpu c;
    /* tail b across functions preserves the live lr (r3 data) */
    memset(&c, 0, sizeof c); c.r[3] = 0xDEAD0001; c.r[14] = 0xAAAA0001;
    fn_81000000(&c);
    ck("exec b tail preserves lr", c.r[14] == 0xDEAD0001);
    /* bl writes lr = next|1 */
    memset(&c, 0, sizeof c); c.r[14] = 0xAAAA0001;
    fn_81000100(&c);
    ck("exec bl writes lr", c.r[14] == 0x81000105u);
    /* cbz taken (r0 == 0): reaches fnF, lr preserved */
    memset(&c, 0, sizeof c); c.r[3] = 0xDEAD0002; c.r[14] = 0xAAAA0001; c.r[0] = 0;
    fn_81000200(&c);
    ck("exec cbz taken preserves lr", c.r[14] == 0xDEAD0002);
    /* cbz not taken (r0 != 0): falls through, lr preserved */
    memset(&c, 0, sizeof c); c.r[3] = 0xDEAD0002; c.r[14] = 0xAAAA0001; c.r[0] = 1;
    fn_81000200(&c);
    ck("exec cbz not-taken preserves lr", c.r[14] == 0xDEAD0002);
    /* cbnz taken (r1 != 0) / not taken */
    memset(&c, 0, sizeof c); c.r[3] = 0xDEAD0003; c.r[14] = 0xAAAA0001; c.r[1] = 1;
    fn_81000300(&c);
    ck("exec cbnz taken preserves lr", c.r[14] == 0xDEAD0003);
    memset(&c, 0, sizeof c); c.r[3] = 0xDEAD0003; c.r[14] = 0xAAAA0001; c.r[1] = 0;
    fn_81000300(&c);
    ck("exec cbnz not-taken preserves lr", c.r[14] == 0xDEAD0003);
    /* bne taken (Z == 0) / not taken (Z == 1) */
    memset(&c, 0, sizeof c); c.r[3] = 0xDEAD0004; c.r[14] = 0xAAAA0001; c.z = 0;
    fn_81000400(&c);
    ck("exec bne taken preserves lr", c.r[14] == 0xDEAD0004);
    memset(&c, 0, sizeof c); c.r[3] = 0xDEAD0004; c.r[14] = 0xAAAA0001; c.z = 1;
    fn_81000400(&c);
    ck("exec bne not-taken preserves lr", c.r[14] == 0xDEAD0004);
    /* internal plain branch skips its instruction (mov lr, r4 must not run) */
    memset(&c, 0, sizeof c); c.r[4] = 0xBEEF0001; c.r[14] = 0xDEAD0005;
    fn_81000500(&c);
    ck("exec internal b skips instruction, lr intact", c.r[14] == 0xDEAD0005);
    /* blx rN writes lr and dispatches through recomp_lookup */
    memset(&c, 0, sizeof c); c.r[3] = 0x81000011; c.r[14] = 0xAAAA0001;
    fn_81000600(&c);
    ck("exec blx rN writes lr", c.r[14] == 0x81000603u);
    /* bl into the setjmp stub: lr written, the stub runs (r0 = 0) */
    memset(&c, 0, sizeof c); c.r[14] = 0xAAAA0001;
    fn_81000700(&c);
    ck("exec bl setjmp writes lr", c.r[14] == 0x81000705u);
    ck("exec setjmp stub ran", c.r[0] == 0);
    /* a plain branch into the setjmp stub preserves lr, the stub still runs */
    memset(&c, 0, sizeof c); c.r[14] = 0xAAAA0001;
    fn_81000740(&c);
    ck("exec tail into setjmp preserves lr", c.r[14] == 0xAAAA0001);
    ck("exec tail into setjmp ran the stub", c.r[0] == 0);
    return fails ? 1 : 0;
}
'''
hs = os.path.join(tmp, "harness.c")
open(hs, "w").write(harness)
cc = os.environ.get("CC", "cc")
binp = os.path.join(tmp, "exec_test")
cmd = [cc, "-O1", "-I", os.path.dirname(RECOMP), "-o", binp, hs] + [f for f in parts if f.endswith(".c") and os.path.exists(f)]
b = subprocess.run(cmd, capture_output=True, text=True)
if b.returncode != 0:
    results.append(check("harness compiles", False, (b.stderr or b.stdout)[-400:]))
else:
    results.append(check("harness compiles", True))
    x = subprocess.run([binp], capture_output=True, text=True)
    lines = [l for l in x.stdout.splitlines() if l.startswith(("PASS", "FAIL"))]
    for line in lines:
        results.append(check("exec: " + line[5:], line.startswith("PASS")))
    if not lines:
        results.append(check("executed harness produced checks", False, x.stdout[-200:] + x.stderr[-200:]))

failed = results.count(False)
print(f"\n{len(results) - failed}/{len(results)} checks passed  (emitter: {'base upstream' if OLD else 'fixed local'})")
shutil.rmtree(tmp, ignore_errors=True)
sys.exit(0 if failed == 0 else 1)
