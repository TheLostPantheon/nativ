#!/usr/bin/env python3
"""Mode-precedence fixture: a Thumb-bit pointer into an image block is the entry's mode.

A "blx #imm" immediate only ever lands in ARM state, so the recompiler infers its targets are
ARM-mode functions. But the image's own function-pointer words carry the mode too: a word
entry|1 in an image block is the linker saying Thumb. The two can contradict each other; the
pointer wins -- the table walk through those words is the code that actually runs, and a blx
disassembly line may be a misdecode of a literal pool nearby. Classifying the entry ARM decodes
its Thumb bytes as ARM garbage and derails everything that walks the table.

Everything here is synthetic: the addresses (0x3000....), the Thumb-2 bytes and the blx
immediate are chosen by this file (the blx encoding is verified against capstone before use).

The contract check: the entry a Thumb-bit pointer names must be emitted in Thumb mode, even
though a caller's disassembly holds a "blx #<entry>" immediate. The fixture runs recomp.py on
a tiny synthetic module (the entry, the caller, and the pointer word) and compares the first
instruction generated for the entry against capstone's Thumb decode of its bytes.

Usage: fixture_mode_precedence.py [path/to/recomp.py]     (default: ../recomp.py)
Prints one line per check and `N checks, M failed`, then RESULT PASS/FAIL (exit 0/1).
"""
import json, os, re, struct, subprocess, sys, tempfile

from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_ARM

HERE = os.path.dirname(os.path.abspath(__file__))
RECOMP = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, os.pardir, "recomp.py")

TARGET = 0x30001000                      # a Thumb-2 function, named by a Thumb-bit pointer
CALLER = 0x30002000                      # its disassembly holds "blx #0x30001000"
TABLE = 0x30003000                       # the pointer word: TARGET | 1
target_bytes = bytes.fromhex("0720") + bytes.fromhex("7047")    # movs r0, #7 ; bx lr


def encode_blx_imm(at, target):
    """Thumb BLX (immediate), T2: hw1 = 11110 S imm10H, hw2 = 11 J1 0 J2 imm10L 0."""
    off = target - ((at + 4) & ~3)       # word-aligned
    assert off % 4 == 0 and -(1 << 24) <= off < (1 << 24)
    S = (off >> 24) & 1
    I1, I2 = (off >> 23) & 1, (off >> 22) & 1
    imm10H, imm10L = (off >> 12) & 0x3FF, (off >> 2) & 0x3FF
    J1, J2 = (~(I1 ^ S)) & 1, (~(I2 ^ S)) & 1
    return struct.pack("<HH", 0xF000 | (S << 10) | imm10H,
                       0xC000 | (J1 << 13) | (J2 << 11) | (imm10L << 1))


checks, failed = 0, 0
def check(cond, name):
    global checks, failed
    checks += 1
    if not cond:
        failed += 1
        print("FAIL", name)

def first_text(md, code, addr):
    for i in md.disasm(code, addr):
        return f"{i.mnemonic} {i.op_str}".strip()
    return "(undecodable)"


# ---- self-verify the synthetic blx encoding before relying on it
caller_bytes = encode_blx_imm(CALLER, TARGET) + bytes.fromhex("7047")   # blx #TARGET ; bx lr
md_t, md_a = Cs(CS_ARCH_ARM, CS_MODE_THUMB), Cs(CS_ARCH_ARM, CS_MODE_ARM)
blx_text = first_text(md_t, caller_bytes[:4], CALLER)
check(blx_text == f"blx #0x{TARGET:x}", f"synthetic caller disassembles as {blx_text!r}, expected 'blx #0x{TARGET:x}'")

expect_thumb = first_text(md_t, target_bytes, TARGET)
expect_arm = first_text(md_a, target_bytes, TARGET)
check(expect_thumb != expect_arm, f"the two decodes must differ to discriminate ({expect_thumb!r} vs {expect_arm!r})")

# ---- the synthetic module: the entry, the caller and the pointer word
d = {
    "blocks": [
        {"start": TARGET, "size": len(target_bytes), "init": True, "bytes": target_bytes.hex()},
        {"start": CALLER, "size": len(caller_bytes), "init": True, "bytes": caller_bytes.hex()},
        {"start": TABLE, "size": 4, "init": True, "bytes": struct.pack("<I", TARGET | 1).hex()},
    ],
    "functions": {"FUN_target": TARGET, "FUN_caller": CALLER},
    "bodies": [[TARGET, TARGET + len(target_bytes) - 1], [CALLER, CALLER + len(caller_bytes) - 1]],
    "relocs": [], "module": "fixture", "exports": [], "variables": [], "arm": [],
}

with tempfile.TemporaryDirectory() as td:
    js = os.path.join(td, "fixture.json")
    json.dump(d, open(js, "w"))
    out_c = os.path.join(td, "fixture_out.c")
    r = subprocess.run([sys.executable, RECOMP, js, out_c, "--split", "2"],
                       capture_output=True, text=True)
    check(r.returncode == 0, f"recomp.py exited {r.returncode}: {r.stderr[-300:]}")

    src = ""
    for f in [out_c, out_c[:-2] + ".h"] + [out_c[:-2] + f"_part{i}.c" for i in range(4)]:
        if os.path.exists(f):
            src += open(f).read()

m = re.search(r"L_%08x: /\* ([^*]+) \*/" % TARGET, src)
first = m.group(1).strip() if m else "(no label)"
check(m is not None, f"the entry's label was emitted (got {first!r})")
check(first == expect_thumb,
      f"entry 0x{TARGET:08x} must be emitted Thumb: first instruction {first!r} == thumb decode {expect_thumb!r}")
check(first != expect_arm,
      f"first instruction {first!r} must not be the ARM misdecode {expect_arm!r}")

print(f"{checks} checks, {failed} failed")
if failed:
    print("RESULT: FAIL (the Thumb-bit pointer did not win the mode)")
    sys.exit(1)
print(f"RESULT: PASS (0x{TARGET:08x} emitted Thumb: {expect_thumb!r}, not the ARM misdecode)")
