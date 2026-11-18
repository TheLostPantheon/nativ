#!/usr/bin/env python3
"""Find where a game builds the constants the runtime's generic hooks change (runtime/core/hooks.c), so 60 fps and
widescreen for a new game start from a list instead of a search:

  frame_dt   1/30 s as a float (0x3d088889): code that takes "one frame" as a fixed 1/30 s, which runs at double speed at
             60 fps unless replaced by the frame's real time (recomp_ihook_frame_dt)
  aspect     960/544 as a float (0x3fe1e1e2): the view's aspect, widened for wider screens (recomp_ihook_aspect)

    dev/hookscan.py <analysis/eboot.data.json> [--pack packs/<title>/pack.toml [--version V]]
    dev/hookscan.py <analysis/eboot.data.json> --verify <title> --from <frame> [--frames N]

--verify sorts the frame_dt candidates by running the game: one headless run of its verify build on the fixed clock
counts every function's entries over N frames at 30 fps from <frame>, switches the frame rate to 60, and counts again over
2N frames, the same game time. A function that builds "one frame = 1/30 s" runs once a frame, so its count doubles at 60:
hook it. One in a fixed-step loop (step 1/30 until caught up with real time) runs as often per second at either rate:
leave it. Pick <frame> where the game plays its world, with the scripted input done by then (frames run twice as fast
at 60, so inputs after the switch land elsewhere). The run uses the game's TEST_INPUT, its GPU renderer and ATIV_DUMP.

Each site is where a core register receives the constant: the movt of a movw/movt pair, or a pc-relative ldr from a
literal pool. An IHOOKS entry goes on that instruction ("0xADDR:frame_dt"), which is how the packs list them. Loads
straight into a VFP register (vldr sN, [pc]) are listed apart: the generic hooks act on core registers.

A site is a candidate, not a verdict: the constant may serve something that isn't per-frame time (a UI tween's step, an
animation table). --pack compares with what the pack lists by hand, the known answer for the enhanced games: sites
found and listed, found and not listed (to look at), and listed but not found (what this scan can't see). Checking a
candidate means running the game at 30 and 60 fps and comparing (docs/debugging.md, frame-rate A/B)."""
import argparse, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "pipeline"))
from hookmap import Build   # the analysis' functions, decoded

KINDS = {0x3d088889: "frame_dt", 0x3fe1e1e2: "aspect"}
REG = re.compile(r"^(r\d+|sb|sl|fp|ip|lr)\b")


def scan(b):
    """[(kind, site, register, function, how)] for every place a register receives one of the constants"""
    out = []
    for entry in sorted(b.fn):
        pend = {}   # register -> (movw value, address, instructions since)
        for addr, tok, vals in b.insns(entry):
            mn = tok.split()[0]
            m = REG.match(tok.split(" ", 1)[1]) if " " in tok else None
            reg = m.group(1) if m else None
            if mn == "movw" and reg and vals:
                pend[reg] = (vals[0] & 0xffff, addr, 0)
                continue
            if mn == "movt" and reg and reg in pend and vals:
                v = ((vals[0] & 0xffff) << 16) | pend[reg][0]
                if v in KINDS:
                    out.append((KINDS[v], addr, reg, entry, f"movw/movt from {pend[reg][1]:#x}"))
                pend.pop(reg, None)
                continue
            if mn.startswith("ldr") and "[pc" in tok and vals and vals[-1] in KINDS and reg:
                out.append((KINDS[vals[-1]], addr, reg, entry, "ldr from a literal pool"))
            elif mn == "vldr" and "[pc" in tok and vals and vals[-1] in KINDS:
                out.append((KINDS[vals[-1]] + " (vfp)", addr, tok.split()[1].rstrip(","), entry, "vldr from a literal pool: no generic hook yet"))
            for r in list(pend):   # (a pair is a few instructions apart at most; a register reused in between ends it)
                w, a, n = pend[r]
                if n > 8 or (reg == r and mn not in ("movt",)): pend.pop(r)
                else: pend[r] = (w, a, n + 1)
    return out


def pack_sites(path, version):
    sys.path.insert(0, os.path.join(HERE, "..", "pipeline"))
    import packconf
    s = packconf.load(path, version)
    sites = {}
    for w in (s.get("IHOOKS") or "").split():
        a, _, name = w.partition(":")
        sites[int(a, 16)] = name
    return sites


def verify(b, found, title, start, n):
    """(site, kind, function, count at 30, count at 60, verdict) for each frame_dt candidate, from one headless run"""
    import subprocess
    root = os.path.join(HERE, "..")
    vit = [sys.executable, os.path.join(root, "pipeline", "vitarecomp.py")]
    a0, a1 = start, start + n
    sw = a1                      # the windows back to back around the switch: the scene barely moves between them
    b0, b1 = sw + 2, sw + 2 + 2 * n
    env = dict(os.environ, RECOMP_BIN="verify", TEST_FRAMES=str(b1 + 1), TEST_COVERAGE="0", TIMEOUT=os.environ.get("TIMEOUT", "1800"),
               EXTRA_ENV=" ".join(x for x in (os.environ.get("EXTRA_ENV", ""), "RECOMP_GFX=gpu RECOMP_CLOCK=fixed",
                                               f"RECOMP_WATCH_WINDOWS={a0}-{a1},{b0}-{b1}", f"RECOMP_SETTINGS_SWITCH={sw}:frame_rate=60") if x))
    print(f"verify build of {title}, then one run: frames {a0}-{a1} at 30 fps, the switch at {sw}, {b0}-{b1} at 60", flush=True)
    subprocess.run(vit + ["build", title, "verify"], env=env, check=True, stdout=subprocess.DEVNULL)
    subprocess.run(vit + ["test", title], env=env, stdout=subprocess.DEVNULL)
    import glob
    logs = sorted(glob.glob(os.path.join(os.environ.get("ATIV_DUMP", os.path.join(root, "dump")), "decrypted", title, "recomp", "test_verify", "test.log")))
    if not logs: sys.exit("no test_verify/test.log: the run didn't start")
    windows, cur = [], None
    for line in open(logs[0], errors="replace"):
        if line.startswith("== watch: entries per function"): cur = {}; windows.append(cur); continue
        if cur is not None:
            m = re.match(r"\s+\S+\s+(0x[0-9a-f]+)\s+(\d+)", line)
            if m: cur[int(m.group(1), 16)] = cur.get(int(m.group(1), 16), 0) + int(m.group(2))
            elif not line.startswith("  "): cur = None
    if len(windows) < 2: sys.exit(f"the run counted {len(windows)} of 2 windows (see {logs[0]}): did it reach frame {b1}?")
    # per presented frame, against the game's own frame: the function that runs exactly once a frame in both windows
    # (n at 30, 2n at 60). A per-object function's count moves with the scene; divided by a once-a-frame function's it
    # doesn't. A function in a fixed-step loop runs at the same rate per second, so per frame it halves at 60
    ref = [e for e, c in windows[0].items() if c == n and windows[1].get(e) == 2 * n]
    out = []
    for kind, site, reg, entry, how in found:
        if kind != "frame_dt": continue
        c30, c60 = windows[0].get(entry, 0), windows[1].get(entry, 0)
        p30, p60 = c30 / n, c60 / (2 * n)   # per frame
        if not c30 and not c60: v = "not run in the windows"
        elif c30 and c60 and 0.75 <= p60 / p30 <= 1.33: v = "per frame: hook it"
        elif c30 and c60 and 0.4 <= p60 / p30 <= 0.6: v = "per second (a fixed step): leave it"
        else: v = f"unclear ({p30:.1f} a frame at 30, {p60:.1f} at 60)"
        out.append((site, kind, entry, c30, c60, v))
    if not ref: print("  (no function ran exactly once a frame in both windows: the frame rate switch may not have taken)")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("analysis"); ap.add_argument("--pack", default=""); ap.add_argument("--version", default="")
    ap.add_argument("--verify", default="", metavar="TITLE"); ap.add_argument("--from", dest="start", type=int, default=0)
    ap.add_argument("--frames", type=int, default=150)
    a = ap.parse_args()
    b = Build(a.analysis)
    found = scan(b)
    if a.verify:
        if not a.start: sys.exit("--verify needs --from <frame>: where the game plays its world")
        name = lambda e: b.names.get(e) or f"fn_{e:08x}"
        rows = verify(b, found, a.verify, a.start, a.frames)
        for site, kind, entry, c30, c60, v in rows:
            print(f"  {site:#010x} in {name(entry):<14} {c30:>6} at 30, {c60:>6} at 60: {v}")
        keep = [r for r in rows if r[5].startswith("per frame")]
        if keep: print("\nIHOOKS: " + " ".join(f"{s:#x}:frame_dt" for s, *_ in keep))
        return
    name = lambda e: b.names.get(e) or f"fn_{e:08x}"
    for kind in sorted({k for k, *_ in found}):
        rows = [f for f in found if f[0] == kind]
        print(f"== {kind}: {len(rows)} site{'s' if len(rows) != 1 else ''}")
        for _, site, reg, entry, how in rows:
            print(f"  {site:#010x}  {reg:<3} in {name(entry)} ({how})")
    hook = [f for f in found if "(vfp)" not in f[0]]
    if hook:
        print("\nIHOOKS candidates: " + " ".join(f"{site:#x}:{kind}" for kind, site, *_ in hook))
    if a.pack:
        listed = pack_sites(a.pack, a.version)
        mine = {site: kind for kind, site, *_ in hook}
        def generic(n):   # a pack's own wrapper names for the generic hooks
            return "frame_dt" if n.endswith("frame_dt") else "aspect" if "aspect" in n else n
        relevant = {s: n for s, n in listed.items() if generic(n) in ("frame_dt", "aspect")}
        both = sorted(s for s in mine if s in relevant)
        extra = sorted(s for s in mine if s not in relevant)
        missed = sorted(s for s in relevant if s not in mine)
        print(f"\n== against {a.pack}: {len(both)} of {len(relevant)} listed sites found; {len(extra)} found but not listed; {len(missed)} listed but not found")
        for s in extra: print(f"  not listed: {s:#x} {mine[s]} in {name(b.containing(s) or 0)}")
        for s in missed: print(f"  not found:  {s:#x} {relevant[s]} in {name(b.containing(s) or 0)}")


if __name__ == "__main__":
    main()
