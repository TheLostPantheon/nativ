#!/usr/bin/env python3
"""Carry a game pack's hook addresses from one build of a game to another (Killzone 1.00 -> its 1.12 update): every
HOOKS / IHOOKS address is found in the old build's function, that function is found again in the new build by its
code -- instructions with the absolute addresses in them (calls, movw/movt pairs, literal-pool values) masked, so a
function that only moved matches exactly -- and the hook lands on the same instruction there.

A function the update edited has no exact twin: the closest one of a similar size is taken when it's clearly the
closest (difflib over the masked instructions), the instruction aligned through the matching runs, and the line is
marked fuzzy with its similarity: check those by hand (thumbdis.py on both). "ambiguous" means several identical
functions (small helpers); "missing" that nothing came close.

  hookmap.py <old analysis.json> <new analysis.json> <packs/X/pack.toml> [--version <old build's>] [--var HOOKS --var IHOOKS]
            [--data 0x81..,...]

--version: the pack's version the old analysis is of (default: its first). --data: guest data addresses the host's hook code uses (a lock, a global) -- found as movw/movt or literal values in the
old build's hooked functions and read at the aligned instruction in the new one.
Output: one line per hook, then the new HOOKS= / IHOOKS= values (exact and fuzzy ones; the others left out).
"""
import bisect, difflib, hashlib, json, os, re, struct, sys
from array import array
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB

HEX = re.compile(r"#?0x([0-9a-f]+)")


class Build:
    def __init__(self, path):
        d = json.load(open(path))
        self.mem = [(b["start"], bytes.fromhex(b["bytes"])) for b in d["blocks"] if b.get("bytes")]
        self.names = {int(v): k for k, v in d["functions"].items()}
        self.arm = {int(a) for a in d.get("arm", [])}
        self.fn = {}   # entry -> [(start, end)] (end inclusive)
        for b in d["bodies"]:
            s, e, entry = (int(x) for x in (json.loads(b) if isinstance(b, str) else b))
            self.fn.setdefault(entry, []).append((s, e))
        for v in self.fn.values():
            v.sort()
        self.starts = sorted((s, e, entry) for entry, rs in self.fn.items() for s, e in rs)
        self.keys = [r[0] for r in self.starts]
        self.size = {entry: sum(e - s + 1 for s, e in rs) for entry, rs in self.fn.items()}
        self.by_size = {}
        for entry, n in self.size.items():
            self.by_size.setdefault(n, []).append(entry)
        self.cache = {}
        self.lo = min(s for s, _ in self.mem)
        self.hi = max(s + len(b) for s, b in self.mem)

    def rd(self, a, n):
        for s, b in self.mem:
            if s <= a < s + len(b):
                return b[a - s:a - s + n]
        return b""

    def containing(self, a):
        i = bisect.bisect_right(self.keys, a) - 1
        while i >= 0 and self.starts[i][0] <= a:
            s, e, entry = self.starts[i]
            if a <= e:
                return entry
            i -= 1
        return None

    def insns(self, entry):
        """[(address, masked token, literal values)] over the function's ranges."""
        if entry in self.cache:
            return self.cache[entry]
        md = Cs(CS_ARCH_ARM, CS_MODE_ARM if entry in self.arm else CS_MODE_THUMB)
        out = []
        for s, e in self.fn[entry]:
            code = self.rd(s, e - s + 1)
            for ins in md.disasm(code, s):
                op, vals = ins.op_str, []
                def mask(m):
                    v = int(m.group(1), 16)
                    if self.lo <= v < self.hi or v >= 0x10000:
                        vals.append(v)
                        lo, hi = min(r[0] for r in self.fn[entry]), max(r[1] for r in self.fn[entry])
                        if lo <= v <= hi:      # a branch inside the function: its offset is the same in a moved copy
                            return "L%x" % (v - entry)
                        n = self.names.get(v) or self.names.get(v & ~1)
                        return "I:" + n if n and not n.startswith(("FUN_", "fn_", "LAB_")) else "A"
                    return m.group(0)
                if ins.mnemonic in ("movw", "movt"):
                    v = int(HEX.search(op).group(1), 16) if HEX.search(op) else 0
                    tok = f"{ins.mnemonic} {op.split(',')[0]},imm"   # a half of an address (or a constant): masked
                    vals.append(v)
                else:
                    tok = ins.mnemonic + " " + HEX.sub(mask, op)
                if "[pc" in op:   # a literal pool load: the value, not its offset, is what moves
                    m = re.search(r"\[pc, #(-?0x[0-9a-f]+|-?\d+)\]", op)
                    if m:
                        la = ((ins.address + 4) & ~3) + int(m.group(1), 0)
                        lv = struct.unpack("<I", self.rd(la, 4) or b"\0\0\0\0")[0]
                        vals.append(lv)
                out.append((ins.address, tok, vals))
        self.cache[entry] = out
        return out

    def callers(self, target):
        """Thumb BL/BLX sites in .text whose target is `target` (a raw halfword scan, built once; data that happens to
        decode as a BL only adds candidates the caller match then has to confirm)."""
        if not hasattr(self, "_calls"):
            self._calls = {}
            start, b = self.mem[0]
            hw = array("H", b[:len(b) // 2 * 2])
            for i in range(len(hw) - 1):
                h1 = hw[i]
                if h1 & 0xf800 != 0xf000:
                    continue
                h2 = hw[i + 1]
                if h2 & 0xc000 != 0xc000:
                    continue
                S = (h1 >> 10) & 1
                off = (S << 24) | ((1 - (((h2 >> 13) & 1) ^ S)) << 23) | ((1 - (((h2 >> 11) & 1) ^ S)) << 22) \
                    | ((h1 & 0x3ff) << 12) | ((h2 & 0x7ff) << 1)
                if S:
                    off -= 1 << 25
                pc = start + 2 * i + 4
                tgt = pc + off if h2 & 0x1000 else (pc & ~3) + off
                self._calls.setdefault(tgt, []).append(start + 2 * i)
        return self._calls.get(target & ~1, []) + (self._calls.get(target | 1, []) if target & 1 == 0 else [])

    def digest(self, entry):
        return hashlib.sha1("\n".join(t for _, t, _ in self.insns(entry)).encode()).hexdigest()


def movw_pairs(insns):
    """{index of the movt: the 32-bit value its movw/movt pair builds} (same destination register, movw first)."""
    out, last = {}, {}
    for i, (_, tok, vals) in enumerate(insns):
        reg = tok.split()[1].rstrip(",") if len(tok.split()) > 1 else ""
        if tok.startswith("movw"):
            last[reg] = (i, vals[0] if vals else 0)
        elif tok.startswith("movt") and reg in last:
            out[i] = (last[reg][1] & 0xffff) | ((vals[0] if vals else 0) << 16)
    return out


def match(old, new, entry, depth=0):
    """(new entry, how, similarity, index map old->new) for an old function."""
    key = (entry, depth > 0)
    if key in MEMO:
        return MEMO[key]
    MEMO[key] = r = _match(old, new, entry, depth)
    return r


MEMO = {}


def _match(old, new, entry, depth):
    oi = old.insns(entry)
    n = old.size[entry]
    dg = old.digest(entry)
    same = [e for e in new.by_size.get(n, []) if new.digest(e) == dg]
    if len(same) == 1:
        return same[0], "exact", 1.0, {i: i for i in range(len(oi))}
    if len(same) > 1:   # identical copies (a small helper twice): the one whose neighbours match the old function's
        def around(b, e, k):
            es = sorted(b.fn)
            i = es.index(e)
            return [b.digest(es[j]) for j in range(max(0, i - k), min(len(es), i + k + 1)) if j != i]
        for k in (1, 2, 4):
            ctx = around(old, entry, k)
            fit = [e for e in same if around(new, e, k) == ctx]
            if len(fit) == 1:
                return fit[0], "exact", 1.0, {i: i for i in range(len(oi))}
        return same, "ambiguous", 1.0, None
    ot = [t for _, t, _ in oi]
    cands = [e for sz, es in new.by_size.items() if 0.8 * n <= sz <= 1.25 * n for e in es]
    # cheap prefilter: imports called and instruction mnemonics in common
    oset = set(ot)
    scored = []
    for e in cands:
        nt = [t for _, t, _ in new.insns(e)]
        q = len(oset & set(nt)) / max(len(oset), 1)
        if q > 0.6:
            scored.append((q, e, nt))
    scored.sort(reverse=True)
    best = []
    for q, e, nt in scored[:40]:
        sm = difflib.SequenceMatcher(None, ot, nt, autojunk=False)
        best.append((sm.ratio(), e, sm))
    best.sort(key=lambda x: -x[0])
    if not best or best[0][0] < 0.75 or (len(best) > 1 and best[0][0] - best[1][0] < 0.05):
        if depth < 2:   # through its callers: the call at the same place in each caller's new twin names the new function
            votes = {}
            for site in old.callers(entry):
                ce = old.containing(site)
                if ce is None or ce == entry:
                    continue
                cne, chow, _, cimap = match(old, new, ce, depth + 1)
                if chow not in ("exact", "fuzzy") or not cimap:
                    continue
                ci = next((i for i, (a, _, _) in enumerate(old.insns(ce)) if a == site), None)
                if ci is None or ci not in cimap:
                    continue
                for v in new.insns(cne)[cimap[ci]][2]:
                    t = new.containing(v & ~1)
                    if t is not None and (t == (v & ~1) or t == v):
                        votes[t] = votes.get(t, 0) + 1
            if votes:
                e = max(votes, key=votes.get)
                sm = difflib.SequenceMatcher(None, ot, [t for _, t, _ in new.insns(e)], autojunk=False)
                return e, f"callers({votes[e]}/{sum(votes.values())})", sm.ratio(), blocks_map(sm)
        return (best[0][1] if best else None), "missing", (best[0][0] if best else 0.0), None
    r, e, sm = best[0]
    return e, "fuzzy", r, blocks_map(sm)


def blocks_map(sm):
    imap = {}
    for a, b, size in sm.get_matching_blocks():
        for k in range(size):
            imap[a + k] = b + k
    return imap


def main():
    args = [a for a in sys.argv[1:]]
    vars_ = [args[i + 1] for i, a in enumerate(args) if a == "--var"] or ["HOOKS", "IHOOKS"]
    data = [int(x, 16) for i, a in enumerate(args) if a == "--data" for x in args[i + 1].split(",")]
    version = next((args[i + 1] for i, a in enumerate(args) if a == "--version"), "")
    pos = [a for i, a in enumerate(args) if not a.startswith("--") and (i == 0 or args[i - 1] not in ("--var", "--data", "--version"))]
    if len(pos) < 3:
        sys.exit(__doc__)
    old, new = Build(pos[0]), Build(pos[1])
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "pipeline"))
    import packconf
    pack = packconf.load(pos[2], version, {})
    results = {}
    for var in vars_:
        if not pack.get(var):
            continue
        outs = []
        for item in pack[var].split():
            addr_s, name = item.split(":", 1)
            a = int(addr_s, 16)
            entry = old.containing(a)
            if entry is None:
                print(f"{var} {name} {a:#x}: not in a known function"); continue
            ne, how, sim, imap = match(old, new, entry)
            oi = old.insns(entry)
            idx = next((i for i, (ia, _, _) in enumerate(oi) if ia == a), None)
            if (how in ("exact", "fuzzy") or how.startswith("callers")) and idx is not None and imap and idx in imap:
                na = new.insns(ne)[imap[idx]][0]
                outs.append(f"{na:#x}:{name}")
                print(f"{var} {name:16} {a:#x} -> {na:#x}  {how}{'' if how == 'exact' else f' {sim:.3f}'}  (fn {entry:#x} -> {ne:#x})")
            elif how == "ambiguous":
                print(f"{var} {name:16} {a:#x} -> ?  ambiguous: {len(ne)} identical ({', '.join(hex(x) for x in ne[:6])})")
            else:
                print(f"{var} {name:16} {a:#x} -> ?  {how}" + (f" (closest {ne:#x} at {sim:.3f})" if ne else "") + (" -- the hooked instruction itself changed" if how.startswith("callers") else "")
                      + ("" if idx is not None else " -- the address isn't an instruction start in the old build"))
            results.setdefault("fn", {})[entry] = (ne, how, imap)
        print(f'{var}="{" ".join(outs)}"\n')
    for d in data:
        found = False
        for entry, (ne, how, imap) in results.get("fn", {}).items():
            if not (how in ("exact", "fuzzy") or how.startswith("callers")) or not imap:
                continue
            oi, ni = old.insns(entry), new.insns(ne)
            op, np_ = movw_pairs(oi), movw_pairs(ni)
            for i, v in op.items():
                if v == d and i in imap and imap[i] in np_:
                    print(f"data {d:#x} -> {np_[imap[i]]:#x}  (movw/movt in fn {entry:#x})"); found = True; break
            if found:
                break
            for i, (_, _, vals) in enumerate(oi):
                if d in vals and i in imap:
                    nv = [v for v in ni[imap[i]][2] if v not in (ni[imap[i]][0],)]
                    print(f"data {d:#x} -> {', '.join(hex(v) for v in nv)}  (literal in fn {entry:#x})"); found = True; break
            if found:
                break
        if not found:
            print(f"data {d:#x}: not referenced by a matched hooked function")


if __name__ == "__main__":
    main()
