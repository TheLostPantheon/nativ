#!/usr/bin/env python3
"""Cross-host venv interpreter selection: focused checks for vitarecomp._venv_python.

The bug this pins: _python() used to try BOTH venv layouts in a fixed order
(.venv/bin/python first, then .venv/Scripts/python.exe), whatever the host platform.
A Windows .venv is visible under WSL (and a POSIX .venv on Windows) but its
interpreter cannot run the other host's paths -- the pipeline then launched Windows
Python with WSL paths (or a POSIX shim on Windows).

Checks: for each (host platform, venv layout) cell the selection must return that
host's own venv interpreter, else the running interpreter. Platform-independent:
the host is a parameter (_venv_python(root, is_nt)); layouts are real temp files.

  python pipeline/tests/test_python_venv.py                    # the fixed selection: 8 checks
  python pipeline/tests/test_python_venv.py --old <vitarecomp.py>   # control: the file's own
                                                              # def _python() against the same
                                                              # contract (must fail on the old code)

Pass line: "N checks, 0 failed" (exit 0); any failure exits 1. Stdlib only.
"""
import importlib.util
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PIPE = os.path.dirname(HERE)
MODULE = os.path.join(PIPE, "vitarecomp.py")

# (host is_nt, layout) -> the venv interpreter that must be selected
# layouts: "win" = only .venv/Scripts/python.exe, "posix" = only .venv/bin/python,
#          "both" = both, "none" = no .venv
def expect(is_nt, layout, root):
    if layout == "none":
        return sys.executable
    c = os.path.join(root, ".venv", "Scripts", "python.exe") if is_nt else os.path.join(root, ".venv", "bin", "python")
    if layout == "win" and not is_nt: return sys.executable
    if layout == "posix" and is_nt: return sys.executable
    return c

def make_layout(root, layout):
    for c in ("bin/python", "Scripts/python.exe"):
        p = os.path.join(root, ".venv", *c.split("/"))
        want = (layout in ("both", "posix") and c == "bin/python") or (layout in ("both", "win") and c == "Scripts/python.exe")
        if want:
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, "wb").close()

def load_module():
    sys.path.insert(0, PIPE)
    os.environ["ATIV_DUMP"] = tempfile.mkdtemp(prefix="tv-dump-")
    spec = importlib.util.spec_from_file_location("vitarecomp_under_test", MODULE)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m

def load_old(path):
    # extract that file's top-level def _python(): block; it reads ROOT and os.name at call
    # time, so the per-cell root is injected through the namespace
    src = open(path, encoding="utf-8").read().splitlines()
    start = next(i for i, l in enumerate(src) if l.startswith("def _python("))
    end = start + 1
    while end < len(src) and (not src[end].strip() or src[end].startswith((" ", chr(9)))):
        end += 1
    ns = {"os": os, "sys": sys, "ROOT": None}
    exec(compile(chr(10).join(src[start:end]), path, "exec"), ns)
    return ns

def main(argv):
    old_path = None
    if argv and argv[0] == "--old":
        old_path = argv[1]
    checks = failed = 0
    base = tempfile.mkdtemp(prefix="tv-venv-")
    try:
        mod = None if old_path else load_module()
        old_ns = load_old(old_path) if old_path else None
        for layout in ("win", "posix", "both", "none"):
            root = os.path.join(base, layout)
            make_layout(root, layout)
            for is_nt in (True, False):
                want = expect(is_nt, layout, root)
                if old_path:
                    old_ns["ROOT"] = root
                    got = old_ns["_python"]()
                else:
                    got = mod._venv_python(root, is_nt) or sys.executable
                checks += 1
                ok = got == want
                if not ok:
                    failed += 1
                print("  %s host=%s layout=%s -> %s (want %s)" % ("ok  " if ok else "FAIL", "nt" if is_nt else "posix", layout, got, want))
    finally:
        shutil.rmtree(base, ignore_errors=True)
    print("%d checks, %d failed" % (checks, failed))
    return 1 if failed else 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
