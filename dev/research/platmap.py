#!/usr/bin/env python3
"""The platform map: for every function the given games import, which firmware module implements
it, whether that implementation is PURE (it reaches no system call: recompile it and run Sony's
code, as libc already is) or crosses into the kernel (BOUNDARY: the system calls it reaches are
what the host must implement), and what our host does with it today -- and from that, the
complete set of system calls the platform needs for those games.

  platmap.py <game elf dir> [<game elf dir> ...] [--out map.md]

User-mode firmware modules are every .suprx under dump/decrypted (us/, sys/external, ...) plus the
games' own sce_module; their exports are what games call. Kernel modules (every .skprx, the boot
image's included) export the system calls. A function is followed through its call tree across
user modules (a user library importing another user library is still Sony code we can run); any
kernel export it reaches is a system call it needs.

The map is derived from Sony code and stays local (write it under dump/).
"""
import glob, os, re, sys
from collections import defaultdict
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, "..", "..", "recompiler"))   # (platmap beside this; analyze in the recompiler)
from analyze import Module, Analyser

DEC = os.path.join(ROOT, "dump", "decrypted")


def nid_names():
    names = {}
    for l in open(os.path.join(HERE, "..", "..", "decrypt", "nidmap.tsv")):
        p = l.rstrip("\n").split("\t")
        if len(p) >= 3:
            names[(p[0], int(p[1], 16))] = p[2]
    return names


def host_names():
    """names the runtime implements today: every {"name", fn} table entry under runtime/"""
    got = set()
    for p in glob.glob(os.path.join(ROOT, "runtime", "**", "*.c"), recursive=True):
        got |= set(re.findall(r'\{\s*"(\w+)"\s*,\s*h_\w+\s*\}', open(p, errors="replace").read()))
    return got


class Mod:
    """a loaded firmware module: its exports, imports, and a call-graph walker"""
    def __init__(self, path):
        self.path, self.m = path, Module(path)
        self.name = self.m.name
        self.exports = {(lib, nid): a for lib, nid, a, var in self.m.export_table() if not var and lib}
        self.stub = {}
        for lib, fns, _ in self.m.imports():
            for nid, a in fns:
                self.stub[a & ~1] = (lib, nid)
        text = self.m.loads[0]
        self.an = Analyser(self.m, text[2], text[2] + text[4])
        self.entries = {a & ~1 for a in self.exports.values()} | set(self.stub)
        self.calls = {}

    def direct(self, f, arm):
        if f not in self.calls:
            r = self.an.function(f, arm, self.entries)
            self.calls[f] = [] if r is None else [(t & ~1, tarm) for t, tarm in r[1]]
        return self.calls[f]

    def reach(self, a):
        """the (library, NID) imports reachable from export address a"""
        out, seen, todo = set(), set(), [(a & ~1, not a & 1)]
        while todo:
            f, arm = todo.pop()
            if f in seen:
                continue
            seen.add(f)
            if f in self.stub:
                out.add(self.stub[f]); continue
            todo += self.direct(f, arm)
        return out


def build_index():
    """(kernel, users): (library, NID) -> the kernel module exporting it (the system calls), and ->
    the path of the user-mode module exporting it"""
    kernel = {}                     # (lib, nid) -> kernel module name: the system calls
    for p in glob.glob(os.path.join(DEC, "**", "*.skprx.elf"), recursive=True):
        try:
            m = Module(p)
            for lib, nid, a, var in m.export_table():
                if lib: kernel.setdefault((lib, nid), m.name)
        except Exception:
            pass
    user_paths = [p for p in glob.glob(os.path.join(DEC, "**", "*.suprx.elf"), recursive=True)
                  if "/recomp/" not in p and not re.search(r"/PC[A-Z]{2}\d{5}/", p)]
    users = {}                      # (lib, nid) -> path of the user module exporting it
    for p in user_paths:
        try:
            for lib, nid, a, var in Module(p).export_table():
                if lib and not var: users.setdefault((lib, nid), p)
        except Exception:
            pass
    return kernel, users


def classifier(kernel, users):
    """(classify, mod): classify((lib, NID)) -> (system calls reached, unresolved imports), following
    calls across user modules; mod(path) -> its loaded Mod"""
    loaded, cache = {}, {}
    def mod(p):
        if p not in loaded:
            try: loaded[p] = Mod(p)
            except Exception as e: loaded[p] = None; print(f"  {os.path.basename(p)}: {e}", file=sys.stderr)
        return loaded[p]
    def classify(key, stack=()):
        """(system calls reached, unresolved imports) for a user-module export, across modules"""
        if key in cache: return cache[key]
        if key in stack or key not in users: return set(), {key}
        cache[key] = (set(), set())   # cycle guard
        m = mod(users[key])
        if m is None or key not in m.exports:
            cache[key] = (set(), {key}); return cache[key]
        sys_, unres = set(), set()
        for imp in m.reach(m.exports[key]):
            if imp in kernel: sys_.add(imp)
            elif imp in users:
                s, u = classify(imp, stack + (key,)); sys_ |= s; unres |= u
            else: unres.add(imp)
        cache[key] = (sys_, unres)
        return cache[key]
    return classify, mod


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    outp = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else None
    if outp in args: args.remove(outp)
    if not args:
        print(__doc__); sys.exit(1)
    names, host = nid_names(), host_names()
    nm = lambda k: names.get(k, f"{k[0]}_{k[1]:08X}")
    kernel, users = build_index()
    classify, mod = classifier(kernel, users)

    w = (open(outp, "w") if outp else sys.stdout).write
    w("# Platform map\n\nGenerated by `dev/research/platmap.py`. For every function the games import: the firmware module "
      "implementing it, PURE (no system call reachable: run Sony's code) or BOUNDARY (the system calls it reaches), "
      "and whether the host implements it today.\n\n")
    all_sys = defaultdict(set)      # system call -> games needing it
    per_lib = defaultdict(lambda: defaultdict(lambda: [0, 0, 0, 0]))   # lib -> game -> [imports, pure, boundary, host]
    rows_by_game = {}
    for gdir in args:
        game = os.path.basename(os.path.dirname(os.path.abspath(gdir))) if os.path.basename(os.path.abspath(gdir)) == "elf" else os.path.basename(gdir)
        elves = glob.glob(os.path.join(gdir, "*.elf"))
        own = set()
        for p in elves:
            try:
                own |= {(lib, nid) for lib, nid, a, var in Module(p).export_table() if lib}
            except Exception: pass
        imps = set()
        for p in elves:
            try:
                for lib, fns, _ in Module(p).imports():
                    imps |= {(lib, nid) for nid, _ in fns}
            except Exception: pass
        rows = []
        for key in sorted(imps - own):
            lib = key[0]; name = nm(key); h = name in host
            if key in kernel:
                cls, detail = "SYSCALL", kernel[key]; all_sys[key].add(game)
            elif key in users:
                s, u = classify(key)
                for k in s: all_sys[k].add(game)
                cls = "BOUNDARY" if s else "PURE"
                detail = os.path.basename(users[key]).replace(".suprx.elf", "") + (f": {len(s)} syscalls" if s else "") + (f", {len(u)} unresolved" if u else "")
            else:
                cls, detail = "MISSING", "no firmware module in the dump exports it"
            c = per_lib[lib][game]; c[0] += 1; c[1] += cls == "PURE"; c[2] += cls in ("BOUNDARY", "SYSCALL"); c[3] += h
            rows.append((lib, name, cls, detail, h))
        rows_by_game[game] = rows

    games = list(rows_by_game)
    w("## By library\n\nimports / PURE / BOUNDARY or SYSCALL / implemented by the host today, per game\n\n")
    w("| library | " + " | ".join(games) + " |\n|---|" + "---|" * len(games) + "\n")
    for lib in sorted(per_lib):
        w(f"| `{lib}` | " + " | ".join("{}/{}/{}/{}".format(*per_lib[lib][g]) if g in per_lib[lib] else "-" for g in games) + " |\n")
    w(f"\n## The system calls these games need ({len(all_sys)})\n\nThe complete host surface if every user library runs as Sony's code.\n\n| system call | kernel module | games | host today |\n|---|---|---|---|\n")
    for key in sorted(all_sys, key=lambda k: (kernel.get(k, ""), nm(k))):
        w(f"| `{nm(key)}` | {kernel.get(key, '?')} | {len(all_sys[key])} | {'yes' if nm(key) in host else '-'} |\n")
    for game, rows in rows_by_game.items():
        w(f"\n## {game}: {len(rows)} imports\n\n| library | function | class | where | host today |\n|---|---|---|---|---|\n")
        for lib, name, cls, detail, h in rows:
            w(f"| {lib} | `{name}` | {cls} | {detail} | {'yes' if h else '-'} |\n")


if __name__ == "__main__":
    main()
