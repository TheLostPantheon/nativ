#!/usr/bin/env python3
"""recompiler/analyze.py on synthetic code (no game bytes): where a function ends, and which mode its entries are.
Hand-encoded Thumb and ARM words laid out in a fake text segment, run through the Analyser as the pipeline does.

usage: dev/tests/analyze_test.py      (from the repository root, with .venv's python)
"""
import os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "recompiler"))
import analyze as an

LO = 0x81000000
hw = lambda *v: b"".join(struct.pack("<H", x) for x in v)

class Mod:   # the Module surface Analyser reads: mem {text start: bytes}
    def __init__(self, code):
        self.mem = {LO: bytearray(code + b"\0" * 32)}

fails = 0
def check(what, ok):
    global fails
    print(("ok   " if ok else "FAIL ") + what)
    fails += not ok

def body(a, entry, entries, arm=False):
    r = a.function(entry, arm, entries)
    return sorted(r[0]) if r else None

def it_return():
    # A: subs r2,r1,#1; it eq; bx lr; movs r0,#5; bx lr      (the EABI divide helper's shape)
    # B: movs r0,#1; bx lr                                    (control: ends at its bx)
    # C: itt ne; movne r0,#2; bxne lr; movs r0,#3; bx lr      (a two-slot block)
    # D: it eq; moveq r0,#1; bx lr; nop                       (the bx is past the block: unconditional)
    # E: ite eq; bxeq lr; movne r0,#4; bx lr                  (the else slot)
    blocks = {"A": hw(0x1e4a, 0xbf08, 0x4770, 0x2005, 0x4770),
              "B": hw(0x2001, 0x4770),
              "C": hw(0xbf1c, 0x2002, 0x4770, 0x2003, 0x4770),
              "D": hw(0xbf08, 0x2001, 0x4770, 0xbf00),
              "E": hw(0xbf0c, 0x4770, 0x2004, 0x4770)}
    code, ent = b"", {}
    for n, blk in blocks.items():
        ent[n] = LO + len(code)
        code += blk
    a = an.Analyser(Mod(code), LO, LO + len(code))
    entries = set(ent.values())
    span = lambda n, k: [ent[n] + 2 * j for j in range(k)]
    check("IT: bxeq in `it eq` does not end the body", body(a, ent["A"], entries) == span("A", 5))
    check("IT: a plain bx lr ends it", body(a, ent["B"], entries) == span("B", 2))
    check("IT: bxne in the second slot of `itt ne`", body(a, ent["C"], entries) == span("C", 5))
    check("IT: a bx after the block's last slot is unconditional", body(a, ent["D"], entries) == span("D", 3))
    check("IT: bxeq in the first slot of `ite eq`", body(a, ent["E"], entries) == span("E", 4))
    check("IT: ends_function false after A's bxeq", not a.ends_function(ent["A"] + 6))
    check("IT: ends_function true after B's bx lr", a.ends_function(ent["B"] + 4))

def main():
    it_return()
    print("all passed" if not fails else f"{fails} failed")
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
