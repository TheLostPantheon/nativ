#!/usr/bin/env python3
"""Thumb disassembly of a game's code from its analysis JSON (dump/decrypted/<TITLE>/analysis/<module>.data.json),
with the module's known function and import names on calls, and literal-pool loads and movw/movt pairs resolved to
the value they build. For reading a function a stack, a stall report or an import log points at (fn_<addr> = addr+1).

  .venv/bin/python dev/thumbdis.py <analysis.json> <hex addr> [instructions, default 80]
"""
import json, struct, sys
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB

def main():
    d = json.load(open(sys.argv[1]))
    names = {v: k for k, v in d['functions'].items()}
    mem = {b['start']: bytes.fromhex(b['bytes']) for b in d['blocks'] if b.get('bytes')}
    def rd(a, n):
        for s, by in mem.items():
            if s <= a < s + len(by): return by[a - s:a - s + n]
        return None
    def name(v): return names.get(v, names.get(v & ~1, ''))
    a = int(sys.argv[2], 16) & ~1; n = int(sys.argv[3]) if len(sys.argv) > 3 else 80
    code = rd(a, n * 4 + 64)
    if code is None: sys.exit('%#x is not in the module' % a)
    movw = {}
    for i, ins in enumerate(Cs(CS_ARCH_ARM, CS_MODE_THUMB).disasm(code, a)):
        if i >= n: break
        op, extra = ins.op_str, ''
        if ins.mnemonic in ('b', 'bl', 'blx') or ins.mnemonic.startswith('b.'):
            try: extra = name(int(op.lstrip('#'), 16))
            except ValueError: pass
        elif ins.mnemonic.startswith('ldr') and '[pc' in op:
            off = int(op.split('#')[-1].rstrip(']'), 16) if '#' in op else 0
            lit = rd(((ins.address + 4) & ~3) + off, 4)
            if lit: v = struct.unpack('<I', lit)[0]; extra = '=%#x %s' % (v, name(v))
        elif ins.mnemonic.startswith('movw'): movw[op.split(',')[0]] = int(op.split('#')[-1], 16)
        elif ins.mnemonic.startswith('movt') and op.split(',')[0] in movw:
            v = movw[op.split(',')[0]] | int(op.split('#')[-1], 16) << 16; extra = '=%#x %s' % (v, name(v))
        print('%08x: %-8s %-32s %s' % (ins.address, ins.mnemonic, op, extra))

if __name__ == '__main__': main()
