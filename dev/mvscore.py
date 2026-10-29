#!/usr/bin/env python3
"""mvscore.py <dir>: score frame-queued captures (mv_N.ppm.raw + mv_N.ppm.color.ppm, one frame number for both): colour
N-1 warped by vectors N vs colour N, against no motion, over pixels whose source stays on screen; also the error on
pixels that changed. With <path>.jitter files (RECOMP_MV_JITTER) the offsets are taken out of the warp; --nojit
ignores them (the jitter shows as error). Needs numpy"""
import glob, os, re, sys, numpy as np
d = sys.argv[1]; nojit = '--nojit' in sys.argv
def ppm(p):
    b = open(p, "rb").read(); m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", b)
    w, h = int(m[1]), int(m[2]); return np.frombuffer(b[m.end():m.end() + w * h * 3], np.uint8).reshape(h, w, 3).astype(np.float32)
def raw(p):
    a = np.fromfile(p, np.uint32, 2); w, h = int(a[0]), int(a[1])
    return np.fromfile(p, np.float16, offset=8).astype(np.float32).reshape(h, w, 2)
def bilinear(img, x, y):
    h, w = img.shape[:2]; x0 = np.clip(np.floor(x).astype(int), 0, w - 2); y0 = np.clip(np.floor(y).astype(int), 0, h - 2)
    fx = (x - x0)[..., None]; fy = (y - y0)[..., None]
    return (img[y0, x0] * (1 - fx) * (1 - fy) + img[y0, x0 + 1] * fx * (1 - fy) + img[y0 + 1, x0] * (1 - fx) * fy + img[y0 + 1, x0 + 1] * fx * fy)
ns = sorted(int(re.search(r"mv_(\d+)\.ppm\.raw$", f)[1]) for f in glob.glob(os.path.join(d, "mv_*.ppm.raw")))
col = lambda n: os.path.join(d, f"mv_{n}.ppm.color.ppm")
def jit(n):
    p = os.path.join(d, f"mv_{n}.ppm.jitter")
    return (0.0, 0.0) if nojit or not os.path.exists(p) else tuple(float(x) for x in open(p).read().split())
rows = []
for n in ns:
    if n - 1 not in ns or not os.path.exists(col(n)) or not os.path.exists(col(n - 1)): continue
    cur, prev, v = ppm(col(n)), ppm(col(n - 1)), raw(os.path.join(d, f"mv_{n}.ppm.raw"))
    H, W = cur.shape[:2]
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    jc, jp = jit(n), jit(n - 1)
    px = xx - v[..., 0] * W - jc[0] + jp[0]; py = yy + v[..., 1] * H - jc[1] + jp[1]
    ok = (px >= 0) & (px <= W - 1) & (py >= 0) & (py <= H - 1)
    e_mv = np.abs(bilinear(prev, px, py) - cur).mean(axis=2); e_0 = np.abs(prev - cur).mean(axis=2)
    changed = ok & (e_0 > 4)   # pixels that differ from the previous frame at all
    rows.append((n, e_0[ok].mean(), e_mv[ok].mean(), e_0[changed].mean() if changed.any() else 0, e_mv[changed].mean() if changed.any() else 0, changed.mean(),
                 float(np.median(np.hypot(v[..., 0] * W, v[..., 1] * H)))))
print(f"{'frame':>6} {'no motion':>10} {'vectors':>8} {'better':>7} | {'changed px':>10} {'no motion':>10} {'vectors':>8} {'better':>7} | median move")
for n, e0, emv, c0, cmv, fr, med in rows:
    print(f"{n:6d} {e0:10.2f} {emv:8.2f} {100 * (1 - emv / e0) if e0 else 0:6.1f}% | {100 * fr:9.1f}% {c0:10.2f} {cmv:8.2f} {100 * (1 - cmv / c0) if c0 else 0:6.1f}% | {med:5.2f} px")
if rows:
    a = np.array([r[1:] for r in rows]); print(f"mean: all {a[:,0].mean():.2f} -> {a[:,1].mean():.2f} ({100*(1-a[:,1].mean()/a[:,0].mean()):.1f}% better); changed {a[:,2].mean():.2f} -> {a[:,3].mean():.2f} ({100*(1-a[:,3].mean()/a[:,2].mean()):.1f}% better)")
