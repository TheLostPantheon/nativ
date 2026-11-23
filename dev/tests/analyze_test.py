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

def gap_functions():
    # K: push {r4,lr}; movs r0,#1; pop {r4,pc}           (a known body)
    # nop                                                 (padding)
    # M: mov ip,sp; push.w {r0-r3}; push.w {r4,r5,ip,lr}; movs r0,#2; pop.w {r4,r5,ip,lr}; add sp,#16; bx lr
    # N: push {r4,lr}; movs r0,#3; pop {r4,pc}           (a known body)
    # E: bx lr                                            (an empty method between N and P)
    # P: push {r4,lr}; movs r0,#4; pop {r4,pc}           (a known body)
    # Z: movs r0,#0; bx lr                                (a `return 0` right after P)
    # Q: push {r4,lr}; pop {r4,pc}; .word 0x81001235      (a known body, then a literal pool: no function)
    # nop; M2: mov ip,sp; push {r4,lr}; movs r0,#5; pop {r4,pc}   (one prologue, not two)
    blocks = [("K", hw(0xb510, 0x2001, 0xbd10), True), ("pad", hw(0xbf00), None),
              ("M", hw(0x46ec, 0xe92d, 0x000f, 0xe92d, 0x5030, 0x2002, 0xe8bd, 0x5030, 0xb004, 0x4770), False),
              ("N", hw(0xb510, 0x2003, 0xbd10), True), ("E", hw(0x4770), False), ("P", hw(0xb510, 0x2004, 0xbd10), True),
              ("Z", hw(0x2000, 0x4770), False), ("Q", hw(0xb510, 0xbd10), True), ("pool", struct.pack("<I", 0x81001235), None),
              ("pad2", hw(0xbf00), None), ("M2", hw(0x46ec, 0xb510, 0x2005, 0xbd10), False)]
    code, ent, bodies = b"", {}, []
    for n, blk, known in blocks:
        ent[n] = LO + len(code)
        if known: bodies.append([ent[n], ent[n] + len(blk) - 1, ent[n]])
        code += blk
    a = an.Analyser(Mod(code), LO, LO + len(code))
    found = {x for x, arm in an.prologues(a, bodies)}
    check("gap: mov ip, sp before the pushes is a prologue", ent["M"] in found)
    check("gap: a lone bx lr after a function's end is a function", ent["E"] in found)
    check("gap: a `return 0` leaf after a function's end is a function", ent["Z"] in found)
    check("gap: mov ip, sp; push {r4,lr} is one prologue", ent["M2"] in found and ent["M2"] + 2 not in found)
    check("gap: nothing inside M, and no function in the literal pool", not (found - {ent["M"], ent["E"], ent["Z"], ent["M2"]}))

def main():
    it_return()
    gap_functions()
    print("all passed" if not fails else f"{fails} failed")
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
