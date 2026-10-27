#!/usr/bin/env python3
"""Find the first draw of a capture where the GPU path parts from the software path.

  gfxbisect.py <capture.vfc> [--interp] [--threshold PCT]

Replays the capture on both backends stopped after draw k (gfxreplay's RECOMP_REPLAY_DRAWS: each
dumps that draw's render target with its depth/stencil history intact) and binary-searches k for
the first draw whose target differs in more than PCT% of pixels (default 0.5; a pixel differs when
its RGB distance exceeds 36). --interp runs the software side on the shader interpreter.
Prints the draw, its programs and target, and leaves the two dumps as bisect_soft.ppm and
bisect_gpu.ppm in the current directory.
"""
import os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPLAY = os.path.join(HERE, "..", "build", "gfxreplay")


def ppm(path):
    with open(path, "rb") as f:
        f.readline(); f.readline(); f.readline()
        return f.read()


def run(cap, k, gpu, interp, out):
    env = dict(os.environ, RECOMP_REPLAY_DRAWS=str(k))
    env.pop("RECOMP_GFX_CHECK", None)
    if gpu: env["RECOMP_GFX"] = "gpu"
    else: env.pop("RECOMP_GFX", None)
    if interp: env["RECOMP_SHADERS"] = "interp"
    r = subprocess.run([REPLAY, cap, out], env=env, capture_output=True, text=True)
    m = re.search(r"^draw \d+ .*$", r.stderr, re.M)
    return m.group(0) if m else None


def differ(a, b):
    n = min(len(a), len(b)) // 3
    d = sum(1 for i in range(0, 3 * n, 3) if abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2]) > 36)
    return 100.0 * d / max(n, 1)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args: print(__doc__); sys.exit(1)
    cap, interp = args[0], "--interp" in sys.argv
    thr = float(sys.argv[sys.argv.index("--threshold") + 1]) if "--threshold" in sys.argv else 0.5
    subprocess.run([os.path.join(HERE, "..", "runtime", "debug", "build_replay.sh")], check=True)
    r = subprocess.run([REPLAY, cap, os.devnull], capture_output=True, text=True)
    total = int(re.search(r"(\d+) draws", r.stderr).group(1))

    def diverged(k):
        run(cap, k, False, interp, "bisect_soft.ppm"); info = run(cap, k, True, interp, "bisect_gpu.ppm")
        pct = differ(ppm("bisect_soft.ppm"), ppm("bisect_gpu.ppm"))
        print(f"  draw {k:4d}: {pct:6.2f}% differ")
        return pct > thr, info, pct

    if not diverged(total)[0]:
        print(f"no divergence above {thr}% after all {total} draws"); return
    lo, hi = 0, total   # draw `lo` agrees (0: before any), draw `hi` differs
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if diverged(mid)[0]: hi = mid
        else: lo = mid
    _, info, pct = diverged(hi)
    print(f"first divergent draw: {hi} of {total} ({pct:.2f}% of its target)\n  {info}\n  dumps: bisect_soft.ppm bisect_gpu.ppm")


if __name__ == "__main__":
    main()
