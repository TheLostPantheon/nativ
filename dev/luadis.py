#!/usr/bin/env python3
"""luadis.py: Lua 5.1 functions dumped from a running game, as readable bytecode.

The host's Lua trace (packs/PCSA00029/hooks.c uc_lua_call, RECOMP_UC_TOUCH_TRACE=1 for Uncharted) writes every Lua
function it sees -- code words, line numbers, constants, local and upvalue names -- to
~/uc_lua_protos.txt. This prints each one (or those named) with the constants and names filled in:

    luadis.py [protos.txt] [proto address ...]
"""
import os
import sys

OPS = ["MOVE", "LOADK", "LOADBOOL", "LOADNIL", "GETUPVAL", "GETGLOBAL", "GETTABLE", "SETGLOBAL",
       "SETUPVAL", "SETTABLE", "NEWTABLE", "SELF", "ADD", "SUB", "MUL", "DIV", "MOD", "POW", "UNM",
       "NOT", "LEN", "CONCAT", "JMP", "EQ", "LT", "LE", "TEST", "TESTSET", "CALL", "TAILCALL",
       "RETURN", "FORLOOP", "FORPREP", "TFORLOOP", "SETLIST", "CLOSE", "CLOSURE", "VARARG"]


def load(path):
    protos, cur = {}, None
    for line in open(path, encoding="latin-1"):
        line = line.rstrip("\n")
        if line.startswith("proto "):
            w = line.split()
            cur = {"addr": int(w[1], 16), "head": line, "code": [], "lines": [], "k": {}, "locals": [], "upvals": {}}
            protos[cur["addr"]] = cur
        elif cur is None:
            continue
        elif line.startswith("code"):
            cur["code"] = [int(x, 16) for x in line.split()[1:]]
        elif line.startswith("lines"):
            cur["lines"] = [int(x) for x in line.split()[1:]]
        elif line.startswith("k "):
            _, i, t, *rest = line.split(" ", 3) + [""]
            v = rest[0] if rest else ""
            cur["k"][int(i)] = repr(v) if t == "s" else (v if t in ("n", "b") else "nil")
        elif line.startswith("local "):
            w = line.split(); name, a, b = " ".join(w[2:-2]), w[-2], w[-1]
            cur["locals"].append((name, int(a), int(b)))
        elif line.startswith("upval "):
            _, i, name = line.split(" ", 2)
            cur["upvals"][int(i)] = name
    return protos


def rk(p, x):
    return "K" + str(x - 256) + "=" + p["k"].get(x - 256, "?") if x >= 256 else reg(p, x, None)


def reg(p, r, pc):
    names = [n for n, a, b in p["locals"] if pc is None or a <= pc < b]
    live = [n for n, a, b in p["locals"] if pc is not None and a <= pc < b]
    return "R%d%s" % (r, "(" + live[r] + ")" if pc is not None and r < len(live) else "")


def dis(p):
    print(p["head"])
    for pc, i in enumerate(p["code"]):
        op, a, c, b, bx = i & 0x3f, (i >> 6) & 0xff, (i >> 14) & 0x1ff, (i >> 23) & 0x1ff, i >> 14
        sbx = bx - 131071
        name = OPS[op] if op < len(OPS) else "op%d" % op
        R = lambda r: reg(p, r, pc)
        K = lambda x: rk(p, x)
        if name in ("LOADK", "GETGLOBAL", "SETGLOBAL"):
            arg = "%s %s" % (R(a), "K%d=%s" % (bx, p["k"].get(bx, "?")))
        elif name in ("JMP", "FORLOOP", "FORPREP"):
            arg = "%s -> %d" % (R(a) if name != "JMP" else "", pc + 1 + sbx)
        elif name in ("GETTABLE", "SELF"):
            arg = "%s %s %s" % (R(a), R(b), K(c))
        elif name == "SETTABLE":
            arg = "%s %s %s" % (R(a), K(b), K(c))
        elif name in ("ADD", "SUB", "MUL", "DIV", "MOD", "POW", "EQ", "LT", "LE"):
            arg = "%d %s %s" % (a, K(b), K(c)) if name in ("EQ", "LT", "LE") else "%s %s %s" % (R(a), K(b), K(c))
        elif name == "GETUPVAL" or name == "SETUPVAL":
            arg = "%s U%d%s" % (R(a), b, "(" + p["upvals"][b] + ")" if b in p["upvals"] else "")
        elif name in ("TEST", "TESTSET"):
            arg = "%s %s %d" % (R(a), R(b) if name == "TESTSET" else "", c)
        elif name == "CLOSURE":
            arg = "%s proto#%d" % (R(a), bx)
        else:
            arg = "%s %d %d" % (R(a), b, c)
        line = p["lines"][pc] if pc < len(p["lines"]) else ""
        print("  %4d [%s] %-10s %s" % (pc, line, name, arg))
    print()


def main():
    args = sys.argv[1:]
    path = args.pop(0) if args and not args[0].startswith("0x") else os.path.expanduser("~/uc_lua_protos.txt")
    protos = load(path)
    want = [int(x, 16) for x in args]
    for addr, p in protos.items():
        if not want or addr in want:
            dis(p)


if __name__ == "__main__":
    main()
