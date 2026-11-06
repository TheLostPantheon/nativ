#!/usr/bin/env python3
"""Synthetic tests for dev/hookmap.py's exact / fuzzy / missing contract.

Two hand-assembled Thumb builds of one toy program:

  func_a  moved by +0x40 between builds (its `bl` to func_h re-encodes; hook_a's instruction
          itself is unchanged) -> the hook must map EXACTLY to the same instruction
  func_b  edited by one instruction in the new build -> the hook must map FUZZY, with a similarity
  func_c  absent from the new build -> the hook must report MISSING

Each build is a synthetic analysis.json (blocks / functions / bodies / arm -- the format
hookmap.py's Build class reads) plus a pack.toml carrying the HOOKS. Run with a Python that has
capstone (the tool needs it):

    python3 dev/test_hookmap.py

It prints `all passed (4 ok)` and exits 0.
"""
import json, os, re, struct, subprocess, sys, tempfile

HOOKMAP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hookmap.py")

def hw(x): return struct.pack("<H", x)

def bl(addr, target):
    """Thumb-2 BL (T1): hw1=11110 S imm10, hw2=11 J1 1 J2 imm11; S=0, J1=J2=1 for these offsets."""
    off2 = (target - (addr + 4)) >> 1
    assert 0 <= off2 < (1 << 22), "fixture offset out of the simple-encoded range"
    return hw(0xf000 | (off2 >> 11)) + hw(0xf800 | (off2 & 0x7ff))

# the three functions' hand-assembled bodies (Thumb, little-endian halfwords)
def body_a(bl_addr):  # push {lr}; movs r0,#0; adds r0,#1  <- hook_a; bl func_h; pop {pc}
    return hw(0xb5b0) + hw(0x2000) + hw(0x3001) + bl(bl_addr, 0x10005000) + hw(0xbd00)
BODY_B0 = hw(0x2100) + hw(0x3101) + hw(0x3101) + hw(0x4770)   # movs r1,#0; adds r1,#1 <- hook_b; adds r1,#1; bx lr
BODY_B1 = hw(0x2100) + hw(0x3101) + hw(0x3102) + hw(0x4770)   # ... edited: adds r1,#2
BODY_C  = hw(0x2200) + hw(0x3201) + hw(0x4770)                # movs r2,#0; adds r2,#1 <- hook_c; bx lr
BODY_H  = hw(0x4770)                                          # bx lr

def build_json(blocks, functions, bodies):
    return {"blocks": [{"start": s, "bytes": b.hex()} for s, b in blocks],
            "functions": functions, "bodies": [list(x) for x in bodies], "arm": []}

def main():
    old = build_json(
        [(0x10000000, body_a(0x10000006)), (0x10001000, BODY_B0), (0x10002000, BODY_C), (0x10005000, BODY_H)],
        {"func_a": 0x10000000, "func_b": 0x10001000, "func_c": 0x10002000, "func_h": 0x10005000},
        [(0x10000000, 0x1000000B, 0x10000000), (0x10001000, 0x10001007, 0x10001000),
         (0x10002000, 0x10002005, 0x10002000), (0x10005000, 0x10005001, 0x10005000)])
    new = build_json(
        [(0x10000040, body_a(0x10000046)), (0x10001000, BODY_B1), (0x10005000, BODY_H)],
        {"func_a": 0x10000040, "func_b": 0x10001000, "func_h": 0x10005000},
        [(0x10000040, 0x1000004B, 0x10000040), (0x10001000, 0x10001007, 0x10001000),
         (0x10005000, 0x10005001, 0x10005000)])
    with tempfile.TemporaryDirectory() as td:
        op, np_, pp = os.path.join(td, "old.json"), os.path.join(td, "new.json"), os.path.join(td, "pack.toml")
        json.dump(old, open(op, "w")); json.dump(new, open(np_, "w"))
        open(pp, "w").write('NAME = "Synthetic"\n[versions."1.00"]\nsha256 = "%s"\n'
                            'HOOKS = "0x10000004:hook_a 0x10001002:hook_b 0x10002002:hook_c"\n' % ("0" * 64))
        r = subprocess.run([sys.executable, HOOKMAP, op, np_, pp], capture_output=True, text=True)
        out = r.stdout
        checks = []

        m = re.search(r"hook_a\s+0x10000004 -> (0x[0-9a-f]+)\s+exact\s+\(fn (0x[0-9a-f]+) -> (0x[0-9a-f]+)\)", out)
        checks.append(("func_a moved +0x40 maps EXACTLY (hook 0x10000004 -> 0x10000044, fn -> 0x10000040)",
                       m and m.group(1) == "0x10000044" and m.group(2) == "0x10000000" and m.group(3) == "0x10000040"))

        m = re.search(r"hook_b\s+0x10001002 -> (0x[0-9a-f]+)\s+fuzzy ([0-9.]+)", out)
        checks.append(("func_b edited maps FUZZY with its similarity (hook -> 0x10001002, 0.5 < sim < 1)",
                       m and m.group(1) == "0x10001002" and 0.5 < float(m.group(2)) < 1.0))

        m = re.search(r"hook_c\s+0x10002002 -> \?\s+([a-z]+)", out)
        checks.append(("func_c absent reports MISSING", m and m.group(1) == "missing"))

        m = re.search(r'HOOKS="([^"]*)"', out)
        checks.append(("the output HOOKS= carries hook_a + hook_b and drops hook_c",
                       m and "hook_a" in m.group(1) and "hook_b" in m.group(1) and "hook_c" not in m.group(1)))

        for name, ok in checks:
            print(f"{'ok  ' if ok else 'FAIL'}  {name}")
        failed = sum(1 for _, ok in checks if not ok)
        if failed:
            print("--- hookmap output ---"); print(out); print(r.stderr)
            print(f"{failed} checks FAILED"); sys.exit(1)
        print(f"all passed ({len(checks)} ok)")

if __name__ == "__main__":
    main()
