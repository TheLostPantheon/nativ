#!/usr/bin/env python3
"""recomp.py's spin_loads: which loads in a loop are read anew each time round (RD*V) because another thread may change
what they read. Hand-encoded Thumb loops, decoded by recomp.py's own capstone: a wait marks its load, a loop's work
doesn't, and every conditional branch counts as a loop's end (blo, bls, blt and ble were once taken for calls).

usage: dev/tests/spin_loads_test.py      (from the repository root, with .venv's python)
"""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "recompiler"))
import recomp

CASES = [   # (Thumb bytes from 0x1000, the loads that must be marked, what it is)
    ("2068 0028 fcd1", {0x1000}, "a wait: ldr r0, [r4]; cmp r0, #0; bne back (Resistance's loading screen)"),
    ("2068 0028 fcdd", {0x1000}, "a wait ending in ble"),
    ("0130 2068 0028 fbd0", {0x1002}, "a wait with its load at the bottom: adds; ldr r0, [r4]; cmp; beq back"),
    ("2168 0130 8842 fbd3", {0x1000}, "a bound read each time round: ldr r1, [r4]; adds r0, #1; cmp r0, r1; blo back"),
    ("2068 0028 fcd0", {0x1000}, "a wait for a flag to clear: beq back"),
    ("2168 4a58 0130 0a2b f9d3", set(), "the loop's work, not its end: ldr r1, [r4]; ldr r2, [r1, r1]; cmp r3, #10; blo"),
    ("2068 2060 0028 fbd1", set(), "a loop that stores: the compiler can't hoist its load"),
    ("2068 0028 00f0 00f8", set(), "no loop (a call after it)"),
    ("2068 0130 0028 fbd1", {0x1000}, "a load adjusted before its compare: ldr r0, [r4]; adds r0, #1; cmp r0, #0; bne back"),
    ("2168 0020 0028 fbd1", set(), "a load whose register isn't what's compared: ldr r1, [r4]; movs r0, #0; cmp r0, #0; bne"),
]

def main():
    fails = 0
    for hexcode, want, what in CASES:
        insns = list(recomp.md.disasm(bytes.fromhex(hexcode.replace(" ", "")), 0x1000))
        got = recomp.spin_loads(insns)
        ok = got == want
        fails += not ok
        print(f"{'ok  ' if ok else 'FAIL'} {what}" + ("" if ok else f": marked {sorted(map(hex, got))}, want {sorted(map(hex, want))}"))
    print("all passed" if not fails else f"{fails} failed")
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
