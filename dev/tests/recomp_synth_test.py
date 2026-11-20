#!/usr/bin/env python3
"""The recompiler on small hand-written assembly (no game, no firmware): each case is built into an image, recompiled,
compiled with dev/glue.c and run beside Unicorn running the original code; r0 and the data checksum must agree. This is
for contracts a compiler never produces in a C program (recomp_exec_test.py's inputs), like a branch out of a function
that must leave LR alone.

    dev/tests/recomp_synth_test.py       (needs clang with the ARM target, unicorn, pyelftools and capstone: requirements.txt)
Prints "N checks, M failed" and exits 1 on any mismatch."""
import json, os, random, re, struct, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import recomp_exec_test as rx

PRELUDE = r'''
.syntax unified
.macro TFN name
 .global \name
 .thumb_func
 .type \name,%function
\name:
.endm
.macro AFN name
 .global \name
 .type \name,%function
\name:
.endm
.macro END name
 .size \name, .-\name
.endm
.text
'''

# a tail jump (b, cbz, cbnz, a conditional b) out of a function leaves LR alone: the callee returns to the caller's caller
# through it, and reads it as data (g_lr hands it back in r0). Thumb and ARM.
LR_TAIL = PRELUDE + r'''
.thumb
TFN g_lr
 mov r0, lr
 bx lr
END g_lr
TFN t_tail_b
 movs r1, #7
 b g_lr
END t_tail_b
TFN t_tail_bcond
 cmp r0, #3
 beq g_lr
 movs r0, #9
 bx lr
END t_tail_bcond
TFN t_tail_cbz
 cbz r0, .Lg_lr2
 movs r0, #5
 bx lr
END t_tail_cbz
TFN t_tail_cbnz
 cbnz r0, .Lg_lr2
 movs r0, #6
 bx lr
END t_tail_cbnz
.Lg_lr2:
TFN g_lr2
 mov r0, lr
 bx lr
END g_lr2
TFN t_call_links
 push {r4, lr}
 bl g_lr2
 pop {r4, pc}
END t_call_links
.arm
.p2align 2
AFN ag_lr
 mov r0, lr
 bx lr
END ag_lr
AFN t_atail_b
 mov r1, #7
 b ag_lr
END t_atail_b
AFN t_atail_bcond
 cmp r0, #3
 beq ag_lr
 mov r0, #9
 bx lr
END t_atail_bcond
AFN t_acall_links
 push {r4, lr}
 bl ag_lr
 pop {r4, pc}
END t_acall_links
'''

# a table case that is another function's entry (a split-off entry reached by tbb) runs it as a tail call
JUMP_ENTRY = PRELUDE + r'''
.thumb
TFN t_tbb
 cmp r0, #1
 bhi 9f
 tbb [pc, r0]
.Ltbl:
 .byte (.Lcase0 - .Ltbl) / 2, (.Lother - .Ltbl) / 2
.Lcase0:
 movs r0, #10
 bx lr
9:
 movs r0, #99
 bx lr
END t_tbb
.p2align 1
.Lother:
TFN g_other
 movs r0, #20
 bx lr
END g_other
'''

# the harness of a module build (recomp.py --module): its functions are in its own table, which a module registers with
# the host; the main program's lookup knows none of them
MODULE_HARNESS = rx.HARNESS.replace("int main(", r'''static GuestFn (*modlk)(uint32_t);
void recomp_register_module(const char *n, GuestFn (*lk)(uint32_t), const char *(*in)(uint32_t), int (*ar)(uint32_t)) { modlk = lk; }
GuestFn recomp_lookup(uint32_t a) { return 0; }
int main(''', 1).replace("recomp_run(&c, fn & ~1u);", "{ GuestFn mf = modlk(fn & ~1u); if (mf) mf(&c); else recomp_unimplemented(&c, fn); }", 1)
assert "modlk(fn" in MODULE_HARNESS

fails = checks = 0


def build_image(src, tmp, name):
    s = os.path.join(tmp, name + ".s"); open(s, "w").write(src)
    obj = os.path.join(tmp, name + ".o")
    subprocess.run([rx.CC, "--target=armv7a-none-eabi", "-mcpu=cortex-a9", "-c", s, "-o", obj], check=True)
    return rx.link(obj)


def run_case(name, src, tmp, flags=(), harness=None, n=12):
    """every t_* function of src on n inputs: the recompiled code (as built and with --no-reg-locals) against Unicorn"""
    global fails, checks
    image, fns = build_image(src, tmp, name)
    tests = sorted((fn, a) for fn, (a, _) in fns.items() if fn.startswith("t_"))
    rng = random.Random(77)
    cases = [(a, *(rng.choice((0, 1, 2, 3, 0xffffffff, rng.getrandbits(8), rng.getrandbits(32))) for _ in range(4)))
             for _, a in tests for _ in range(n)]
    img_file = os.path.join(tmp, name + ".bin")
    with open(img_file, "wb") as f:
        for a, b in image.items(): f.write(struct.pack("<II", a, len(b)) + b)
    want = rx.run_unicorn(image, cases)
    saved = rx.HARNESS
    if harness: rx.HARNESS = harness
    try:
        for variant, extra in (("locals", []), ("plain", ["--no-reg-locals"])):
            c_file = os.path.join(tmp, f"{name}_{variant}.c"); rx.recompile(image, fns, c_file, list(flags) + extra)
            got = rx.run_recompiled(rx.build(c_file, tmp, f"{name}_{variant}"), img_file, cases, tmp)
            names = {a: fn for fn, a in tests}
            bad = {}
            for k, case in enumerate(cases):
                checks += 1
                if got[k] != want[k]:
                    fails += 1; bad.setdefault(names[case[0]], (case, got[k], want[k]))
            for fn, (case, g, w) in sorted(bad.items()):
                print(f"FAIL {name} {fn} ({variant}): args {' '.join(f'{x:#x}' for x in case[1:])} -> {g}, the CPU gives {w}")
            print(f"{name} ({variant}): {len(tests)} functions x {n} inputs: {'ok' if not bad else str(len(bad)) + ' function(s) differ'}")
    finally:
        rx.HARNESS = saved


def thumb_branch(at, target, link_arm):
    """a Thumb bl (link_arm: blx #imm, which switches to ARM and rounds the base to a word) at address at"""
    off = target - ((at + 4) & ~3 if link_arm else at + 4)
    sgn, i1, i2 = (off >> 24) & 1, (off >> 23) & 1, (off >> 22) & 1
    j1, j2 = ~(i1 ^ sgn) & 1, ~(i2 ^ sgn) & 1
    lo = 0xc000 | (((off >> 2) & 0x3ff) << 1) if link_arm else 0xd000 | ((off >> 1) & 0x7ff)
    return struct.pack("<HH", 0xf000 | sgn << 10 | ((off >> 12) & 0x3ff), lo | j1 << 13 | j2 << 11)


def thumb_bl(at, target): return thumb_branch(at, target, False)
def thumb_bw(at, target):
    """a Thumb b.w (T4, a tail call: no link, no change of instruction set) at address at"""
    bl = thumb_branch(at, target, False)
    hi, lo = struct.unpack("<HH", bl)
    return struct.pack("<HH", hi, lo & ~0x4000)   # (bit 14 of the second halfword: BL 1, B.W 0)
def thumb_bcc(at, target, cond):
    """a narrow conditional Thumb branch (T1, b<cond>) at address at"""
    return struct.pack("<H", 0xd000 | cond << 8 | (((target - (at + 4)) >> 1) & 0xff))
def thumb_blx(at, target): return thumb_branch(at, target, True)


def arm_set(c_text):
    body = c_text[c_text.index("int recomp_is_arm("):]
    body = body[:body.index("default:")]
    return {int(x, 16) for x in re.findall(r"case 0x([0-9a-f]+)u:", body)}


def check(what, ok):
    global fails, checks
    checks += 1; fails += not ok
    print(("ok   " if ok else "FAIL ") + what)


def mode_cases(tmp):
    """which functions come out ARM, from a hand-laid image and the analysis' ARM list (recomp.py main)"""
    base = 0x81000000
    text = bytearray(0x200)
    def put(off, data): text[off:off + len(data)] = data
    movs1_bx = struct.pack("<HH", 0x2001, 0x4770)   # movs r0, #1; bx lr
    bx_lr = struct.pack("<H", 0x4770)
    junk_fn = struct.pack("<HHH", 0xed90, 0x3300, 0x4770)   # Thumb: ldc p3, c3, [r0]; bx lr. ARM: an ordinary data-processing word
    F = {n: base + o for n, o in dict(
        ptr=0x00, c_ptr=0x20, noptr=0x40, c_noptr=0x60, odd=0x82, guess_bl=0xa0, c_bl=0xc0, guess=0xe0,
        noise_arm=0x100, noise_t=0x140, guess_b=0x160, c_b=0x180, guess_bcc=0x1a0, c_bcc=0x1c0).items()}
    put(0x00, movs1_bx)                              # F.ptr: Thumb, and a word in .rodata holds F.ptr | 1
    put(0x20, thumb_blx(F["c_ptr"], F["ptr"]) + bx_lr)   # calls it with blx #imm (which reads as ARM)
    put(0x40, movs1_bx)                              # F.noptr: the same, no pointer: ARM by its caller's blx
    put(0x60, thumb_blx(F["c_noptr"], F["noptr"]) + bx_lr)
    put(0x82, junk_fn)                               # F.odd: 2-aligned, so never ARM whatever it decodes like
    put(0xa0, junk_fn)                               # F.guess_bl: the guess says ARM; a Thumb bl caller says Thumb
    put(0xc0, thumb_bl(F["c_bl"], F["guess_bl"]) + bx_lr)
    put(0xe0, junk_fn)                               # F.guess: the same with no caller: the guess stands (ARM)
    # F.noise_arm is ARM (the analysis says so); read as Thumb, its first word is a blx #imm to F.noise_t, a Thumb function
    put(0x100, struct.pack("<I", 0)); w = thumb_blx(F["noise_arm"], F["noise_t"]); put(0x100, struct.pack("<I", w[0] | w[1] << 8 | w[2] << 16 | w[3] << 24))
    put(0x104, struct.pack("<I", 0xe12fff1e))        # bx lr
    put(0x140, movs1_bx)
    put(0x160, junk_fn)                              # F.guess_b: the guess says ARM; a Thumb b.w (a tail call) to it says Thumb
    put(0x180, thumb_bw(F["c_b"], F["guess_b"]) + bx_lr)
    put(0x1a0, junk_fn)                              # F.guess_bcc: the same through a conditional bhs
    put(0x1c0, thumb_bcc(F["c_bcc"], F["guess_bcc"], 0x2) + bx_lr)
    sizes = {"ptr": 4, "c_ptr": 6, "noptr": 4, "c_noptr": 6, "odd": 6, "guess_bl": 6, "c_bl": 6, "guess": 6, "noise_arm": 8, "noise_t": 4,
             "guess_b": 6, "c_b": 6, "guess_bcc": 6, "c_bcc": 4}
    rodata = struct.pack("<I", F["ptr"] | 1)
    d = {"blocks": [{"start": base, "size": len(text), "init": True, "bytes": bytes(text).hex(), "name": ".text"},
                    {"start": base + 0x1000, "size": len(rodata), "init": True, "bytes": rodata.hex(), "name": ".rodata"}],
         "functions": {n: a | (0 if n == "noise_arm" else 1) for n, a in F.items()},
         "bodies": [[a, a + sizes[n] - 1] for n, a in F.items()], "relocs": [], "module": "m", "exports": [], "variables": [],
         "arm": [F["noise_arm"]]}
    js = os.path.join(tmp, "mode.json"); json.dump(d, open(js, "w"))
    out = os.path.join(tmp, "mode.c")
    r = subprocess.run([sys.executable, os.path.join(rx.ROOT, "recompiler", "recomp.py"), js, out], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("recomp.py failed:\n" + r.stdout[-2000:] + r.stderr[-2000:])
    arm = arm_set(open(out).read())
    check("mode: a blx #imm to an entry that a word in the image points at with the Thumb bit stays Thumb", F["ptr"] not in arm)
    check("mode: ... and without such a word the blx makes it ARM", F["noptr"] in arm)
    check("mode: an entry that is not 4-aligned is never ARM", F["odd"] not in arm)
    check("mode: the coprocessor guess yields to a Thumb bl from certain Thumb code", F["guess_bl"] not in arm)
    check("mode: ... and stands without one", F["guess"] in arm)
    check("mode: the guess yields to a Thumb b.w tail call too (a B never changes the instruction set)", F["guess_b"] not in arm)
    check("mode: ... and to a conditional Thumb branch", F["guess_bcc"] not in arm)
    check("mode: a blx #imm read from a function the analysis knows as ARM does not make its target ARM", F["noise_t"] not in arm)
    check("mode: the analysis' own ARM list is kept", F["noise_arm"] in arm)


def prefix_case(tmp):
    """an entry inside a 32-bit instruction of its own function's decode: the analysis gives E a chunk overlapping the code
    before it, E's spans merge from F, and the linear decode reads mov.w across E (Luufen's fork, e5bd60d). Called at
    E, the CPU runs lsls r0, r0, #8 (the mov.w's second halfword) then the body's adds/bx: r0 = 1 -> 257"""
    global fails, checks
    F = rx.BASES[".text"]; E = F + 4
    code = struct.pack("<5H", 0x2005, 0xF04F, 0x0200, 0x3001, 0x4770)   # movs r0,#5; mov.w r2,#0; adds r0,#1; bx lr
    image = {F: code}
    d = {"blocks": [{"start": F, "size": len(code), "init": True, "bytes": code.hex(), "name": ".text"}],
         "functions": {"f": F | 1, "e": E | 1}, "bodies": [[F, F + 9, F], [F, F + 5, E], [E, F + 9, E]],
         "relocs": [], "module": "progs", "exports": [], "variables": [], "arm": []}
    img_file = os.path.join(tmp, "prefix.bin")
    with open(img_file, "wb") as f:
        for a, b in image.items(): f.write(struct.pack("<II", a, len(b)) + b)
    cases = [(E | 1, 1, 0, 0x55, 0), (E | 1, 3, 0, 0x55, 0), (F | 1, 0, 0, 0x55, 0)]
    want = rx.run_unicorn(image, cases)
    for variant, extra in (("locals", []), ("plain", ["--no-reg-locals"])):
        c_file = os.path.join(tmp, f"prefix_{variant}.c"); js = c_file + ".json"; json.dump(d, open(js, "w"))
        r = subprocess.run([sys.executable, os.path.join(rx.ROOT, "recompiler", "recomp.py"), js, c_file] + extra, capture_output=True, text=True)
        if r.returncode:
            check(f"prefix ({variant}): recomp.py", False); print(r.stdout[-800:] + r.stderr[-800:]); continue
        try:
            got = rx.run_recompiled(rx.build(c_file, tmp, f"prefix_{variant}"), img_file, cases, tmp)
        except (subprocess.CalledProcessError, SystemExit) as e:
            check(f"prefix ({variant}): an entry inside an instruction builds and runs", False); continue
        check(f"prefix ({variant}): an entry inside an instruction runs its own decode, then the body's", got == want)


def gap_cases(tmp):
    """functions no analysis body covers, reached only through pointers (Luufen's fork, 570a1a7 and cec40dc): G, a Thumb
    method whose switch case sits after its first unconditional b, and A, an ARM method only a vtable row reaches (an even
    word next to another code pointer)"""
    global fails, checks
    F, R = rx.BASES[".text"], rx.BASES[".rodata"]
    K, G, A = F, F + 0x10, F + 0x20
    text = bytearray(0x28)
    text[0x00:0x04] = struct.pack("<2H", 0x2001, 0x4770)                                # K: movs r0,#1; bx lr (a known function)
    text[0x10:0x1c] = struct.pack("<6H", 0x2800, 0xd001, 0x2001, 0xe000, 0x2002, 0x4770)  # G: cmp r0,#0; beq +8; movs r0,#1; b +10; movs r0,#2; bx lr
    text[0x20:0x28] = struct.pack("<2I", 0xe2800003, 0xe12fff1e)                       # A: add r0,r0,#3; bx lr (ARM)
    rodata = struct.pack("<3I", G | 1, K | 1, A)                                       # a Thumb pointer to G; a vtable row K, A
    image = {F: bytes(text), R: rodata}
    d = {"blocks": [{"start": F, "size": len(text), "init": True, "bytes": bytes(text).hex(), "name": ".text"},
                    {"start": R, "size": len(rodata), "init": True, "bytes": rodata.hex(), "name": ".rodata"}],
         "functions": {"k": K | 1}, "bodies": [[K, K + 3]], "relocs": [], "module": "progs", "exports": [], "variables": [], "arm": []}
    img_file = os.path.join(tmp, "gap.bin")
    with open(img_file, "wb") as f:
        for a, b in image.items(): f.write(struct.pack("<II", a, len(b)) + b)
    cases = [(G | 1, 0, 0, 0, 0), (G | 1, 5, 0, 0, 0), (A, 1, 0, 0, 0), (A, 0x40, 0, 0, 0)]
    want = rx.run_unicorn(image, cases)
    c_file = os.path.join(tmp, "gap.c"); js = c_file + ".json"; json.dump(d, open(js, "w"))
    r = subprocess.run([sys.executable, os.path.join(rx.ROOT, "recompiler", "recomp.py"), js, c_file], capture_output=True, text=True)
    if r.returncode:
        check("gap: recomp.py", False); print(r.stdout[-800:] + r.stderr[-800:]); return
    try:
        got = rx.run_recompiled(rx.build(c_file, tmp, "gap"), img_file, cases, tmp)
    except (subprocess.CalledProcessError, SystemExit):
        got = ["(build or run failed)"] * len(cases)
    check("gap: a pointer-only Thumb method keeps the switch case after its first b", got[:2] == want[:2])
    check("gap: an ARM method only a vtable row reaches is a function", got[2:] == want[2:])


def main():
    with tempfile.TemporaryDirectory(prefix="recomp_synth_") as tmp:
        run_case("lr_tail", LR_TAIL, tmp)
        run_case("jump_entry", JUMP_ENTRY, tmp)
        run_case("jump_entry_module", JUMP_ENTRY, tmp, flags=("--module", "m"), harness=MODULE_HARNESS)
        mode_cases(tmp)
        prefix_case(tmp)
        gap_cases(tmp)
    print(f"{checks} checks, {fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
