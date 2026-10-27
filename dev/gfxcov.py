#!/usr/bin/env python3
"""The graphics gap table: one or more RECOMP_GFX_COVERAGE maps (runtime/debug/gfx_coverage.c; `vitarecomp
test` writes one per game as test/gfxcov.json), side by side, weighted by draws.

  gfxcov.py <map.json> [<map.json> ...] [--all]

Sections: draw state/formats a backend doesn't honour; GXM calls the host ignores (no-op stubs,
state tracked but never passed on, missing imports) with the arguments they were called with;
shader programs the interpreter or the SPIR-V translator can't take, grouped by reason. Each
count is draws (frames) @ first frame, so a gap can be found again with RECOMP_SHOT. --all also
lists what is fully supported.
"""
import json, re, struct, sys
from collections import defaultdict


def load(paths):
    games = []
    for p in paths:
        try:
            d = json.load(open(p))
        except (OSError, ValueError) as e:
            print(f"{p}: {e}", file=sys.stderr)
            continue
        d["label"] = d.get("title") or p
        games.append(d)
    return games


def cell(x):
    return f"{x['draws']} ({x['frames']}f) @{x['first_frame']}" if x else "-"


def table(head, rows):
    w = [max(len(str(r[i])) for r in [head] + rows) for i in range(len(head))]
    line = lambda r: "  ".join(str(c).ljust(w[i]) for i, c in enumerate(r)).rstrip()
    print(line(head)); print(line(["-" * x for x in w]))
    for r in rows: print(line(r))


def as_float(u):
    f = struct.unpack("<f", struct.pack("<I", u))[0]
    return f"{f:g}" if 1e-6 < abs(f) < 1e7 or u == 0 else None


def fmt_args(a):
    """r0-r3 | s0-s5 (float arguments, hard-float ABI) | three stack words"""
    hexes = lambda ws: ", ".join(f"{u:#x}" for u in ws)
    floats = ", ".join(as_float(u) or f"{u:#x}" for u in a[4:10])
    return f"(r {hexes(a[:4])} | s {floats} | stack {hexes(a[10:])})"


# usse_internal.h's OP_ enum, for usse_spirv's "(op N)" reasons
OPS = ["NOP", "VMAD2", "VNMAD", "VMAD", "VDP", "VDUAL", "VCOMP", "VMOV", "VPCK", "VTST", "VTSTMSK", "VBW", "SOP2", "SOP2M",
       "SOP3", "I8MAD", "I16MAD", "I32MAD", "I32MAD2", "SMP", "BR", "SMLSI", "SMBO", "KILL", "LIMM", "DEPTHF", "LDST", "UNSUPPORTED"]


def reason_of(err):
    m = re.search(r"\(op (\d+)\)", err or "")
    if m and int(m.group(1)) < len(OPS): return OPS[int(m.group(1))] + " (some form of it)"
    return re.sub(r"0x[0-9a-f]{5,}|\b\d{3,}\b", "#", err or "?")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    show_all = "--all" in sys.argv
    if not args:
        print(__doc__); sys.exit(1)
    games = load(args)
    if not games: sys.exit(1)
    labels = [g["label"] for g in games]
    print("runs: " + ", ".join(f"{g['label']}: {g['frames']} frames, {g['scenes']} scenes, {g['draws']} draws" for g in games))

    # ---- draw state and formats
    feats = defaultdict(dict)
    meta = {}
    for g in games:
        for f in g["features"]:
            k = (f["key"], f["value"]); feats[k][g["label"]] = f; meta[k] = f
    rows = []
    for k, per in feats.items():
        m = meta[k]
        if not show_all and m["soft"] == "yes" and m["gpu"] == "yes": continue
        total = sum(x["draws"] for x in per.values())
        rows.append((total, [k[0], k[1], m["soft"], m["gpu"]] + [cell(per.get(l)) for l in labels] + [m["note"]]))
    rows.sort(key=lambda r: -r[0])
    print("\n== draw state and formats" + ("" if show_all else " a backend doesn't fully honour"))
    table(["feature", "value", "soft", "gpu"] + labels + ["note"], [r[1] for r in rows]) if rows else print("(none)")

    # ---- api
    apis = defaultdict(dict)
    for g in games:
        for a in g["api"]:
            apis[a["name"]][g["label"]] = a
    rows = []
    for name, per in apis.items():
        kind = next(iter(per.values()))["kind"]
        if kind == "implemented" and not show_all: continue
        calls = sum(a["calls"] for a in per.values())
        rows.append((kind != "missing", -calls, [name, kind] + [f"{per[l]['calls']} @{per[l]['first_frame']}" if l in per else "-" for l in labels]))
    rows.sort(key=lambda r: (r[0], r[1]))
    print("\n== GXM/display calls" + ("" if show_all else " the host ignores"))
    table(["call", "kind"] + labels, [r[2] for r in rows]) if rows else print("(none)")
    for name, per in apis.items():
        for l, a in per.items():
            if a["kind"] == "implemented" or not a["args"] or a["kind"] == "no-op" and not show_all and not re.search(r"Viewport|RegionClip|PolygonMode|Clip|Gamma|Palette|Lod|MipFilter|TwoSided|BackDepth|PointLine", name):
                continue
            print(f"  {name} [{l}]:")
            for x in a["args"]: print("    " + fmt_args(x))

    # ---- shaders
    print("\n== shader programs")
    for g in games:
        sh = g["shaders"]
        tot = sum(s["draws"] for s in sh) or 1
        bad_i = [s for s in sh if not s["interp"]]
        bad_g = [s for s in sh if not s["gpu"]]
        pct = lambda xs: 100.0 * sum(s["draws"] for s in xs) / tot
        print(f"{g['label']}: {len(sh)} programs; interpreter can't run {len(bad_i)} ({pct(bad_i):.1f}% of program-draws); "
              f"GPU can't translate {len(bad_g)} ({pct(bad_g):.1f}%)")
        for what, xs, key in (("interpreter", bad_i, "interp_bad"), ("gpu", bad_g, "gpu_err")):
            groups = defaultdict(list)
            for s in xs:
                reason = reason_of(s[key])   # group by reason, not by address or instruction index
                if what == "interpreter": reason = " ".join(s[key].split()[:2]) or "?"
                groups[reason].append(s)
            for reason, ss in sorted(groups.items(), key=lambda kv: -sum(s["draws"] for s in kv[1])):
                d = sum(s["draws"] for s in ss)
                first = min(s["first_frame"] for s in ss)
                print(f"  {what:11s} {d:7d} draws  {len(ss):3d} programs @{first:<5d} {reason}   e.g. {ss[0]['addr']}")


if __name__ == "__main__":
    main()
