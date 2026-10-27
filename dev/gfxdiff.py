#!/usr/bin/env python3
"""Compare two captures of the same frame draw by draw, naming the fields that differ.

  gfxdiff.py <a.vfc> <b.vfc> [--max N]

Typically the host's GXM against Sony's libgxm (RECOMP_SONY_GXM=1) on the same frames: both feed the
renderer, so any field where they disagree is a decode difference at the boundary. Uses gfxreplay's
RECOMP_REPLAY_DUMP text (one line per scene/draw). Scenes are aligned by content (frame boundaries
differ between runs), and guest addresses differ too (the heap is laid out differently), so values
>= 0x10000000 are compared only as present/absent; everything else -- formats, sizes, counts, strides,
offsets, render state -- must match exactly.
"""
import os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPLAY = os.path.join(HERE, "..", "build", "gfxreplay")


def dump(cap):
    out = subprocess.run([REPLAY, cap], env=dict(os.environ, RECOMP_REPLAY_DUMP="1"), capture_output=True, text=True).stdout
    return [l for l in out.splitlines() if l.startswith(("scene", "draw", "present"))]


def fields(line):
    """(kind, {name: value}) from a dump line: 'name value' and 'k=value' tokens, in order"""
    kind, _, rest = line.partition(":")
    toks = rest.split()
    out, section, n = {}, "", 0
    for t in toks:
        if re.fullmatch(r"[a-z]+", t):   # a section word (attr, streams, tex, blend, ...)
            section, n = t, 0
            continue
        out[f"{section}[{n}]"] = t
        n += 1
    return kind.split()[0], out


def norm(v):
    """an address-like value -> 'addr' (presence only)"""
    def one(x):
        try:
            n = int(x, 16) if x.startswith("0x") else None
        except ValueError:
            return x
        return "addr" if n is not None and n >= 0x10000000 else x
    return re.sub(r"0x[0-9a-f]+", lambda m: one(m.group(0)), v)


def scenes(lines):
    """[(scene line, [draw lines])]: the dump split at scene boundaries (presents dropped)"""
    out = []
    for l in lines:
        if l.startswith("scene"): out.append((l, []))
        elif l.startswith("draw") and out: out[-1][1].append(l)
    return out


def signature(draws):
    """what identifies a scene across two runs, addresses aside: each draw's primitive, count, attribute
    count, texture sizes/formats and blend -- in order"""
    sig = []
    for d in draws:
        _, f = fields(d)
        sig.append(tuple(norm(v) for k, v in f.items() if k.startswith(("[", "attr", "tex", "blend", "cull", "depth"))))
    return tuple(sig)


def canon(f):
    """equivalent encodings the two decoders use: a texture's mip-count nibble 0 means 1 (the host's
    layout) and reads 1 from Sony's words; stencil masks don't matter while the test is ALWAYS/KEEP"""
    for k, v in list(f.items()):
        if k.startswith("tex[") and re.fullmatch(r"v0x0?[0-9a-f]", v): f[k] = "v0x1" + v[-1]
    if f.get("st[0]") == "7" and f.get("st[1]") == "000": f["st[2]"] = "0xff/0xff"
    return f


def compare(la, lb):
    ka, fa = fields(la); kb, fb = fields(lb)
    fa, fb = canon(fa), canon(fb)
    return [(k, fa.get(k), fb.get(k)) for k in dict.fromkeys(list(fa) + list(fb)) if norm(fa.get(k, "-")) != norm(fb.get(k, "-"))]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    mx = int(sys.argv[sys.argv.index("--max") + 1]) if "--max" in sys.argv else 10
    args = [x for x in args if x != str(mx)] if "--max" in sys.argv else args
    if len(args) < 2:
        print(__doc__); sys.exit(1)
    a, b = dump(args[0]), dump(args[1])
    sa, sb = scenes(a), scenes(b)
    print(f"{os.path.basename(args[0])}: {len(sa)} scenes, {sum(len(d) for _, d in sa)} draws; "
          f"{os.path.basename(args[1])}: {len(sb)} scenes, {sum(len(d) for _, d in sb)} draws")
    # align: each scene of a to the first unmatched scene of b with the same signature (frame
    # boundaries differ between runs; a scene's content doesn't), else to the next one by order
    used, pairs = set(), []
    for i, (ha, da) in enumerate(sa):
        j = next((j for j, (hb, db) in enumerate(sb) if j not in used and signature(db) == signature(da)), None)
        if j is None: j = next((j for j in range(len(sb)) if j not in used), None)
        if j is None: break
        used.add(j); pairs.append((i, j))
    shown = 0
    for i, j in pairs:
        (ha, da), (hb, db) = sa[i], sb[j]
        tag = f"scene {i + 1}" + (f" (vs scene {j + 1})" if j != i else "")
        d = compare(ha, hb)
        if d: print(f"{tag}: " + "; ".join(f"{k} {x} vs {y}" for k, x, y in d[:6])); shown += 1
        if len(da) != len(db):
            print(f"{tag}: {len(da)} draws vs {len(db)}"); shown += 1
        for n, (la, lb) in enumerate(zip(da, db)):
            d = compare(la, lb)
            if d:
                print(f"{tag} draw {n + 1}: " + "; ".join(f"{k} {x} vs {y}" for k, x, y in d[:8]) + (f" (+{len(d) - 8})" if len(d) > 8 else ""))
                shown += 1
            if shown >= mx: break
        if shown >= mx: break
    if not shown: print("no differences (addresses aside)")


if __name__ == "__main__":
    main()
