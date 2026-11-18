#!/usr/bin/env python3
"""The recompiler against the Vita's whole instruction set, with no game: every instruction form a Cortex-A9 runs
(ARM, Thumb-2, VFPv3 and NEON), each translated as recomp.py translates it and run against Unicorn from random states
(dev/insnsweep.py's engine). insnsweep tests the forms one game happens to use; this tests the ones no game has met yet,
so a game that needs one finds it already working.

    dev/isacover.py [--samples N] [--trials T] [--per-form K] [--only SUBSTRING] [--json OUT] [--baseline IN]

The corpus: every 16-bit Thumb encoding, and N random encodings (default 400000) from each of the 32-bit spaces (ARM
data processing, loads and stores, media, VFP, NEON; Thumb-2's), decoded by recomp.py's own capstone instances. What
capstone decodes isn't all the Vita's CPU has: clang's assembler for -mcpu=cortex-a9 -mfpu=neon-fp16 keeps only what
that CPU runs (no VFPv4 fused multiply-add, no hardware divide, nothing from ARMv8). Control flow, exclusives,
barriers and coprocessor moves are insnsweep's SKIP list: the function-level tests' business. Each form (insnsweep's
form_of: registers by first appearance, immediates by class) keeps up to K encodings.

Reports, by instruction stem: forms the translator doesn't handle (recomp.py raises Unsupported), forms whose result
differs from the CPU's, and forms that crashed the sweep (Unicorn's JIT on some NEON encodings on Apple silicon: run on
an x86 host to tell those apart). --json writes the result; --baseline compares with an earlier one and exits 1 on a form
that passed there and fails now.
"""
import argparse, json, os, random, re, subprocess, sys, tempfile
from collections import defaultdict, Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "recompiler"))
import insnsweep   # its engine (sweep) and form key
import recomp      # md (Thumb), md_arm: the decoders the recompiler itself uses

CC = os.environ.get("CC", "clang")
ARM_BASE, THUMB_BASE = 0x81000000, 0x83000000


def candidates(samples, rng):
    """(bytes, arm) for every 16-bit Thumb encoding and `samples` random words from each 32-bit space"""
    out = []
    for hw in range(0x10000):
        if (hw >> 11) in (0x1d, 0x1e, 0x1f): continue   # 0xe800 and up: the first half of a 32-bit Thumb instruction
        out.append((hw.to_bytes(2, "little"), False))
    for _ in range(samples):   # Thumb-2: a first halfword from the 32-bit space, a random second
        hw1 = rng.randrange(0xe800, 0x10000); hw2 = rng.getrandbits(16)
        out.append((hw1.to_bytes(2, "little") + hw2.to_bytes(2, "little"), False))
    per = max(1, samples // 8)
    for op in range(8):   # ARM, condition AL, by bits 27:25 (data processing, loads and stores, media, block transfers, VFP)
        for _ in range(per):
            out.append(((0xe0000000 | op << 25 | rng.getrandbits(25)).to_bytes(4, "little"), True))
    for _ in range(samples):   # VFP: coprocessors 10 and 11 in the condition space (bits 11:9 = 101)
        w = 0xec000a00 | rng.getrandbits(1) << 25 | rng.getrandbits(24) & 0x01fff1ff
        out.append((w.to_bytes(4, "little"), True))
    for _ in range(samples):   # NEON: the unconditional data-processing (0xf2, 0xf3) and load/store (0xf4) spaces
        w = rng.choice((0xf2000000, 0xf3000000, 0xf4000000)) | rng.getrandbits(24)
        out.append((w.to_bytes(4, "little"), True))
    return out


def decode(cands):
    """what capstone decodes, the whole encoding consumed, not control flow: {(text, arm): [bytes...]}"""
    seen = defaultdict(list)
    for b, arm in cands:
        dis = recomp.md_arm if arm else recomp.md
        i = next(dis.disasm(b, ARM_BASE if arm else THUMB_BASE, count=1), None)
        if i is None or i.size != len(b): continue
        if insnsweep.is_control(i, i.mnemonic): continue
        seen[(f"{i.mnemonic} {i.op_str}".strip(), arm)].append(b)
    return seen


def cortex_a9(texts, arm, tmp):
    """the instruction texts clang's assembler accepts for the Vita's CPU (one assembly per mode, errors by line)"""
    ok = set(range(len(texts)))
    for _ in range(4):   # (an error can stop the assembler early: retry with what's left)
        live = sorted(ok)
        if not live: break
        src = os.path.join(tmp, "a9.s")
        with open(src, "w") as f:
            f.write(".syntax unified\n" + (".arm\n" if arm else ".thumb\n"))
            for k in live: f.write(texts[k] + "\n")
        r = subprocess.run([CC, "--target=armv7a-none-eabi", "-mcpu=cortex-a9", "-mfpu=neon-fp16", "-mfloat-abi=softfp",
                            "-c", src, "-o", os.path.join(tmp, "a9.o")], capture_output=True, text=True)
        bad = {int(m.group(1)) - 3 for m in re.finditer(r"a9\.s:(\d+):\d+: error", r.stderr)}
        if not bad: break
        ok -= {live[n] for n in bad if 0 <= n < len(live)}
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--samples", type=int, default=400000); ap.add_argument("--trials", type=int, default=32)
    ap.add_argument("--per-form", type=int, default=2); ap.add_argument("--only", default="")
    ap.add_argument("--json", default=""); ap.add_argument("--baseline", default=""); ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    rng = random.Random(a.seed)
    seen = decode(candidates(a.samples, rng))
    print(f"decoded: {len(seen)} distinct instructions", flush=True)
    kept, rejected = {}, Counter()
    with tempfile.TemporaryDirectory(prefix="isacover_") as tmp:
        for arm in (False, True):
            texts = [t for (t, m) in seen if m == arm]
            ok = cortex_a9(texts, arm, tmp)
            for k, t in enumerate(texts):
                if k in ok: kept[(t, arm)] = seen[(t, arm)]
                else: rejected[insnsweep.stem(t.split()[0])] += 1
    print(f"the Cortex-A9 runs {len(kept)} of them; set aside {sum(rejected.values())} (another CPU's, or capstone's "
          f"spelling the assembler doesn't take): {', '.join(f'{s} {n}' for s, n in rejected.most_common(12))}", flush=True)
    # forms, each with up to per-form encodings laid out at addresses of their own
    forms = defaultdict(list)
    for (t, arm), bs in kept.items():
        mn, _, ops = t.partition(" ")
        f = insnsweep.form_of(mn, ops) + (" (arm)" if arm else "")
        if a.only and a.only not in f: continue
        forms[f].append((bs[0], arm))
    arm_blk, thumb_blk, inst = bytearray(), bytearray(), {}
    for f in sorted(forms):
        r = random.Random(f); pick = forms[f] if len(forms[f]) <= a.per_form else r.sample(forms[f], a.per_form)
        for b, arm in pick:
            blk, base = (arm_blk, ARM_BASE) if arm else (thumb_blk, THUMB_BASE)
            at = base + len(blk); blk.extend(b + b"\0" * (8 - len(b)))   # (8 bytes each: a 32-bit Thumb one stays aligned)
            inst.setdefault(f, []).append((at, arm))
    print(f"{len(inst)} forms, {sum(len(v) for v in inst.values())} encodings to test", flush=True)
    res = insnsweep.sweep(inst, [(ARM_BASE, bytes(arm_blk)), (THUMB_BASE, bytes(thumb_blk))], a.trials, a.per_form, quiet=True)
    by_stem = defaultdict(lambda: Counter())
    for f in inst: by_stem[insnsweep.stem(f.split()[0])]["forms"] += 1
    for k in ("bad", "unsupported", "crashed"):
        for f in res[k]: by_stem[insnsweep.stem(f.split()[0])][k] += 1
    print(f"\n== {res['ok']} forms match the CPU, {len(res['bad'])} differ, {len(res['unsupported'])} the translator "
          f"doesn't handle, {len(res['crashed'])} crashed the sweep (Unicorn's JIT on Apple silicon: rerun on x86), "
          f"{res['untested']} untested (every state faulted in Unicorn)")
    print("\nby stem (forms: unsupported / differ / crashed):")
    for s, c in sorted(by_stem.items(), key=lambda kv: -(kv[1]["unsupported"] + kv[1]["bad"])):
        if c["unsupported"] or c["bad"] or c["crashed"]:
            print(f"  {s:<12} {c['forms']:>5}: {c['unsupported']:>4} / {c['bad']:>4} / {c['crashed']:>4}")
    for k, label in (("unsupported", "doesn't handle"), ("bad", "differs")):
        for f, why in sorted(res[k].items())[:60]:
            print(f"  {label}: {f}\n      {why}")
    if a.json:
        json.dump({"forms": sorted(inst), "bad": res["bad"], "unsupported": res["unsupported"], "crashed": res["crashed"]},
                  open(a.json, "w"), indent=1)
    if a.baseline:
        base = json.load(open(a.baseline)); failing = set(res["bad"]) | set(res["unsupported"])
        was_ok = set(base["forms"]) - set(base["bad"]) - set(base["unsupported"]) - set(base["crashed"])
        worse = sorted(failing & was_ok)
        for f in worse: print(f"REGRESSION {f}: {res['bad'].get(f) or res['unsupported'].get(f)}")
        sys.exit(1 if worse else 0)


if __name__ == "__main__":
    main()
