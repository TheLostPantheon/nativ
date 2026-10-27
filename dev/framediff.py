#!/usr/bin/env python3
"""framediff.py <a.ppm> <b.ppm>: the percentage of pixels that differ (RGB distance > 36), for the
regression gate (vitarecomp check); exits 1 when the sizes differ.

framediff.py <a.ppm> <b.ppm> --tol N: the exactness view instead (two renders that should agree to
the last bit or two, e.g. the GPU path with and without native BC textures): the largest difference
per channel in 1/255 units, the percentage of pixels with any channel differing at all and by more
than N, and a histogram of each pixel's largest channel difference; exits 2 when a pixel differs by
more than N."""
import math, sys

def ppm(p):
    with open(p, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = map(int, f.readline().split()); f.readline()
        return w, h, f.read()

args = [a for a in sys.argv[1:] if a != "--tol"]
tol = int(sys.argv[sys.argv.index("--tol") + 1]) if "--tol" in sys.argv else None
if tol is not None: args.remove(str(tol))
(w1, h1, a), (w2, h2, b) = ppm(args[0]), ppm(args[1])
if (w1, h1) != (w2, h2):
    print(f"sizes differ: {w1}x{h1} vs {w2}x{h2}"); sys.exit(1)
n = min(len(a), len(b)) // 3 * 3
if tol is None:
    d = sum(1 for i in range(0, n - 2, 3)
            if math.sqrt((a[i] - b[i]) ** 2 + (a[i + 1] - b[i + 1]) ** 2 + (a[i + 2] - b[i + 2]) ** 2) > 36)
    print(f"{100.0 * d / (w1 * h1):.2f}"); sys.exit(0)
chmax, hist, over = [0, 0, 0], {}, 0
for i in range(0, n, 3):
    m = 0
    for c in range(3):
        v = abs(a[i + c] - b[i + c])
        if v > chmax[c]: chmax[c] = v
        if v > m: m = v
    if m:
        hist[m] = hist.get(m, 0) + 1
        if m > tol: over += 1
px = w1 * h1; anyd = sum(hist.values())
print(f"max diff R {chmax[0]} G {chmax[1]} B {chmax[2]} (/255); pixels differing: {100.0 * anyd / px:.3f}% ({anyd}), "
      f"by more than {tol}: {100.0 * over / px:.3f}% ({over})")
print("  per-pixel max-channel diff histogram: " + (", ".join(f"{k}:{hist[k]}" for k in sorted(hist)) or "none"))
sys.exit(2 if over else 0)
