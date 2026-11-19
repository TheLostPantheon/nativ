#!/usr/bin/env python3
"""The recompiler on small hand-written assembly (no game, no firmware): each case is built into an image, recompiled,
compiled with dev/glue.c and run beside Unicorn running the original code; r0 and the data checksum must agree. This is
for contracts a compiler never produces in a C program (recomp_exec_test.py's inputs), like a branch out of a function
that must leave LR alone.

    dev/tests/recomp_synth_test.py       (needs clang with the ARM target, unicorn, pyelftools and capstone: requirements.txt)
Prints "N checks, M failed" and exits 1 on any mismatch."""
import os, random, struct, subprocess, sys, tempfile
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


def main():
    with tempfile.TemporaryDirectory(prefix="recomp_synth_") as tmp:
        run_case("lr_tail", LR_TAIL, tmp)
    print(f"{checks} checks, {fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
