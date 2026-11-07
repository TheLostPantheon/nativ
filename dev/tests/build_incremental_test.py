#!/usr/bin/env python3
"""What runtime/build.py recompiles on a rebuild, and whether recompiler/recomp.py keeps unchanged files' times.

No game, toolchain or SDL: a tiny fake recompiler output (x.c, x_part0.c, x.h including rt.h) is built with a fake
compiler (this file, called as CC/CXX) that logs every source it is given and writes empty objects and a -MMD header
list (the source's quoted #includes, followed through the -I folders). The build's steps and what each must compile:

  1. clean build                       everything (not checked)
  2. no-op rebuild                     nothing
  3. touch one runtime header          only the runtime sources that include it
  4. rewrite rt.h with the same text   no game part
  5. change x.h                        x.c and x_part0.c (they compile against it)
  6. change rt.h                       every game part and LibAtrac9 file (then rt.h is restored)

recomp.py: write_if_changed writes the same bytes as open(path, "w").write(text), leaves a file with the same text
untouched (its time kept), rewrites one whose text changed; main() writes every generated file through it.

usage: dev/tests/build_incremental_test.py [runtime/<build script>.py]   (a POSIX host: Linux, macOS, WSL)
The build script must sit in runtime/ (it finds the runtime beside itself); default runtime/build.py.
"""
import ast, os, re, shutil, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


# ---------------------------------------------------------------- the fake compiler
def includes(src, idirs, seen):
    """the quoted #includes of src, followed through its folder and the -I folders (what -MMD would list)"""
    try: text = open(src, encoding="utf-8", errors="replace").read()
    except OSError: return
    for name in re.findall(r'^\s*#\s*include\s*"([^"]+)"', text, re.M):
        for d in [os.path.dirname(src)] + idirs:
            f = os.path.normpath(os.path.join(d, name))
            if os.path.isfile(f):
                if f not in seen: seen.add(f); includes(f, idirs, seen)
                break


def fake_cc(argv):
    if "--version" in argv: print("fake cc"); return 0
    out = mf = None; idirs, forced, srcs = [], [], []; k = 0
    while k < len(argv):
        a = argv[k]
        if a in ("-o", "-MF", "-I", "-include", "-isystem", "-D", "-L", "-framework"):
            v = argv[k + 1] if k + 1 < len(argv) else ""
            if a == "-o": out = v
            elif a == "-MF": mf = v
            elif a == "-I": idirs.append(v)
            elif a == "-include": forced.append(v)
            k += 2; continue
        if a.startswith("-I"): idirs.append(a[2:])
        elif a.endswith((".c", ".cpp", ".m")) and os.path.isfile(a): srcs.append(a)
        k += 1
    with open(os.environ["FAKECC_LOG"], "a") as f:
        for s in srcs: f.write(os.path.realpath(s) + "\n")
    if out: open(out, "w").close()
    if mf and srcs:
        seen = set(forced)
        for h in forced: includes(h, idirs, seen)
        includes(srcs[0], idirs, seen)
        open(mf, "w").write(f"{out}: " + " \\\n ".join([srcs[0]] + sorted(seen)) + "\n")
    return 0


# ---------------------------------------------------------------- the build test
def run_build(script, base, env, log):
    open(log, "w").close()
    r = subprocess.run([sys.executable, script, base, base + "_bin"], env=env, capture_output=True, text=True)
    if r.returncode: sys.stdout.write(r.stdout + r.stderr); raise SystemExit(f"build failed ({r.returncode})")
    return {l.strip() for l in open(log) if l.strip()}


def classify(files, game):
    groups = {"game parts": [], "libatrac9": [], "runtime C": [], "imgui/overlay C++": []}
    for f in sorted(files):
        if f.startswith(game): groups["game parts"].append(f)
        elif "libatrac9" in f: groups["libatrac9"].append(f)
        elif f.endswith(".cpp"): groups["imgui/overlay C++"].append(f)
        else: groups["runtime C"].append(f)
    return ", ".join(f"{k} {len(v)}" for k, v in groups.items())


def build_test(script):
    results = []
    rt = os.path.join(ROOT, "recompiler", "rt.h"); rt_bytes = open(rt, "rb").read()
    with tempfile.TemporaryDirectory() as td:
        td = os.path.realpath(td)
        cc = os.path.join(td, "fakecc"); log = os.path.join(td, "cc.log")
        with open(cc, "w") as f: f.write(f"#!{sys.executable}\nimport sys\nsys.path.insert(0, {os.path.dirname(os.path.abspath(__file__))!r})\n"
                                          f"import build_incremental_test as t\nsys.exit(t.fake_cc(sys.argv[1:]))\n")
        os.chmod(cc, 0o755)
        game = os.path.join(td, "game"); os.makedirs(game); base = os.path.join(game, "x")
        open(base + ".h", "w").write('#include "rt.h"\nvoid f0(void);\n')
        open(base + ".c", "w").write('#include "x.h"\n')
        open(base + "_part0.c", "w").write('#include "x.h"\nvoid f0(void) {}\n')
        nopc = os.path.join(td, "pkgconfig"); os.makedirs(nopc)
        env = {k: v for k, v in os.environ.items() if not k.startswith(("RECOMP_", "ATIV_", "TARGET", "PROFILE", "VERIFY"))}
        env.update(CC=cc, CXX=cc, JOBS="4", RECOMP_CPU="generic", FAKECC_LOG=log, PKG_CONFIG_PATH="", PKG_CONFIG_LIBDIR=nopc)
        game += os.sep

        def step(name, expect, got):
            ok = expect(got)
            results.append(ok)
            print(f"  {'PASS' if ok else 'FAIL'}  {name:34} compiled {len(got):3}: {classify(got, game)}")

        def later(f):   # a newer time than any object made so far, whatever the file system's resolution
            time.sleep(1.1); os.utime(f)

        all_files = run_build(script, base, env, log)
        print(f"  ----  {'1 clean build':34} compiled {len(all_files):3}: {classify(all_files, game)}")
        step("2 no-op rebuild", lambda g: not g, run_build(script, base, env, log))

        # 3: the runtime header with the fewest includers among the build's runtime sources (at least one)
        idirs = [os.path.join(ROOT, "runtime", d) for d in ("core", "gfx", "audio", "media", "input", "net", "ui", "platform", "debug")] + [os.path.join(ROOT, "recompiler"), ROOT]
        host = [f for f in all_files if not f.startswith(game) and "libatrac9" not in f]
        users = {}
        for s in host:
            seen = set(); includes(s, idirs, seen)
            for h in seen:
                if h.startswith(os.path.join(ROOT, "runtime")) and h.endswith(".h"): users.setdefault(h, set()).add(s)
        hdr = min(users, key=lambda h: (len(users[h]), h))
        later(hdr)
        step(f"3 touch {os.path.basename(hdr)} ({len(users[hdr])} includer{'s' * (len(users[hdr]) != 1)})", lambda g: g == users[hdr], run_build(script, base, env, log))

        try:
            open(rt, "wb").write(rt_bytes); later(rt)
            step("4 rt.h rewritten, same text", lambda g: not any(f.startswith(game) or "libatrac9" in f for f in g), run_build(script, base, env, log))
            open(base + ".h", "a").write("void f1(void);\n"); later(base + ".h")
            step("5 x.h changed", lambda g: g == {os.path.realpath(base + ".c"), os.path.realpath(base + "_part0.c")}, run_build(script, base, env, log))
            open(rt, "ab").write(b"\n/* build_incremental_test */\n"); later(rt)
            atrac = {os.path.realpath(f) for f in all_files if "libatrac9" in f}
            step("6 rt.h changed", lambda g: {os.path.realpath(base + ".c"), os.path.realpath(base + "_part0.c")} | atrac <= g, run_build(script, base, env, log))
        finally:
            open(rt, "wb").write(rt_bytes)
    return results


# ---------------------------------------------------------------- recomp.py
def recomp_test():
    results = []
    src = open(os.path.join(ROOT, "recompiler", "recomp.py"), encoding="utf-8").read()
    tree = ast.parse(src)
    fn = next((n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == "write_if_changed"), None)
    if fn is None:
        print("  FAIL  recomp.py has no write_if_changed: every run rewrites every generated .c/.h (new times)")
        return [False]
    ns = {"os": os}; exec(compile(ast.Module([fn], []), "recomp.py", "exec"), ns); wic = ns["write_if_changed"]
    main = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == "main")
    body = ast.get_source_segment(src, main)
    raw = re.findall(r'open\([^)]*"w"\)\.write', body)
    calls = body.count("write_if_changed(")
    ok = not raw and calls >= 3
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  main() writes through write_if_changed: {calls} calls, {len(raw)} raw open(...,\"w\").write")
    with tempfile.TemporaryDirectory() as td:
        texts = ['#include "x.h"\nvoid f0(void) {}\n', "a\nb\n\nc\n", "", "x" * 100000 + "\n", "no newline at end"]
        same = True
        for i, t in enumerate(texts):
            a, b = os.path.join(td, f"old{i}"), os.path.join(td, f"new{i}")
            open(a, "w").write(t); wic(b, t)
            same &= open(a, "rb").read() == open(b, "rb").read()
        results.append(same)
        print(f"  {'PASS' if same else 'FAIL'}  bytes equal to open(path, \"w\").write for {len(texts)} texts")
        p = os.path.join(td, "new0"); os.utime(p, (1_000_000_000, 1_000_000_000)); before = os.stat(p).st_mtime_ns
        wic(p, texts[0]); kept = os.stat(p).st_mtime_ns == before
        results.append(kept)
        print(f"  {'PASS' if kept else 'FAIL'}  same text again: time kept ({before} -> {os.stat(p).st_mtime_ns})")
        wic(p, texts[0] + "void f1(void);\n")
        changed = os.stat(p).st_mtime_ns != before and open(p).read() == texts[0] + "void f1(void);\n"
        results.append(changed)
        print(f"  {'PASS' if changed else 'FAIL'}  changed text: rewritten")
    return results


def main(argv):
    if os.name == "nt": raise SystemExit("a POSIX host (Linux, macOS, WSL): the build script is run with a fake CC")
    script = os.path.abspath(argv[0]) if argv else os.path.join(ROOT, "runtime", "build.py")
    if os.path.dirname(script) != os.path.join(ROOT, "runtime"): raise SystemExit("the build script must sit in runtime/")
    print(f"build: {os.path.relpath(script, ROOT)}")
    r = build_test(script)
    print("recomp: recompiler/recomp.py")
    r += recomp_test()
    print(f"{sum(r)}/{len(r)} passed")
    return 0 if all(r) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
