#!/usr/bin/env python3
"""vitarecomp: a Vita game from a player's own decrypted dump to a native program, stage by stage.

  vitarecomp firmware  <PSP2UPDAT.PUP>      Sony's firmware update file (any recent one; 3.74 is what
                                            Sony serves) decrypted here -> dump/firmware/<version>/
  vitarecomp scan      <folder> [--import-licences] [--out <file>]   every game, update, add-on and firmware file
                                            under it, at any depth (a copied ux0:, a downloads folder): pipeline/
                                            packs.py's rows, each with what the game's pack says of that version (and
                                            the licences found there kept in the player's licences); --out: the rows
                                            into that file rather than the output
  vitarecomp licenses  [import <file|folder>... | vita <address> | vita3k [folder]]   the player's
                                            licences, kept once (dump/licenses, pipeline/licenses.py): add looks
                                            there by a copy's content id; from files, the Vita itself over
                                            VitaShell's FTP, or a Vita3K install
  vitarecomp add       <copy> [--license work.bin] [--no-build]   a game or its update (ux0:app/<title>
                                            or ux0:patch/<title> copied off the Vita, or a PKG with its licence file
                                            (.rif or work.bin); an already-decrypted folder works too), decrypted here
                                            (decrypt/vitadecrypt) and kept by kind -- dump/decrypted/<title>/app0 (the
                                            game, with license.rif: its update decrypts with the game's licence),
                                            patch0 (the update) -- then import, analyze, recompile and build when a
                                            packs/<title>/pack.toml exists (--no-build: decrypt and keep only, more is
                                            coming -- the game's update: the last add imports and builds them together)
  vitarecomp import    <title> [<dump dir>] executables (eboot.bin, sce_module/*.suprx; FAGDec
                                            output, or add's decrypted tree) -> verified ELFs; game
                                            data -> fs/app0. No dump dir: the title's own copy, its
                                            update layered over it (app0_patched, hard links) when
                                            the pack plays that version; records which version it is
  vitarecomp versions  <title>              the versions the game's pack plays, and which this copy is
  vitarecomp analyze   <title>              functions, code ranges and the memory image of the main
                                            module (analyze.py, seconds; ANALYZER=ghidra: the old
                                            Ghidra export, minutes to hours)
  vitarecomp recompile <title>              ARM/Thumb -> C (recompiler/recomp.py) + memory image
                                            (a pack's HOOKS = "addr:name ..." becomes recomp.py --hooks:
                                            a host recomp_hook_<name>(c), see packs/<title>/hooks.c, runs at
                                            that guest function's entry, e.g. Unit 13's fov setting;
                                            IHOOKS= the same after one instruction: --insn-hooks)
  vitarecomp build     <title> [verify|profile]   the native host with the game linked in
  vitarecomp run       <title> [host args]  play it (window, keyboard/gamepad)
  vitarecomp ready     <title>              its build brought up to date with these tools before it plays (the
                                            launcher's Play): code regenerated when recompiler/ or its pack.toml
                                            changed, the program rebuilt when the tools did; nothing to do: a moment
  vitarecomp test      <title>              headless scripted run (packs/<title>/pack.toml TEST_INPUT),
                                            saves the TEST_FRAMES frame as test.png, and the
                                            graphics coverage map as gfxcov.json
  vitarecomp coverage  <title> ...          the gap table from those maps (gfxcov.py)
  (EXTRA_ENV="A=1 B=0": settings appended to the game's RUN_ENV (run) or TEST_ENV (test/capture) for one command)
  (BUILD_EXTRA_ENV="A=1": settings built into the binary's defaults by build, after RUN_ENV: a diagnostic build for a player)
  vitarecomp accept    <title>              the last test frame becomes the game's reference frame
  vitarecomp check     <title> ...          the regression gate: build and test each game, PASS/FAIL
                                            against its reference (framediff.py: % of pixels differing)
  vitarecomp capture   <title> <frame>      the scripted run up to that frame, its graphics saved
                                            as captures/frame_<n>.vfc (gfx_capture.c)
  vitarecomp replay    <title> <frame> [out.ppm]   that capture through the graphics layer alone,
                                            in seconds (gfxreplay; RECOMP_GFX=gpu etc. as usual)
  vitarecomp verify    <title> [seconds]    the in-place checker over a scripted run
  vitarecomp recipes   <title>              the shader recipes play recorded (shader_cache/recipes.txt) merged
                                            into packs/<title>/shaders.txt, the set that ships: no game bytes in it
  vitarecomp dist      <folder>             the install: only what ships copied there (the launcher, this pipeline,
                                            the recompiler, the decrypter, the runtime's sources, the packs; no tests
                                            or developer tools). Over an install a dist wrote it replaces the program
                                            and nothing else: the library, a python/ or .venv there stay
  vitarecomp report    <title>              a bug report: the game's logs, the launcher's, the settings and the
                                            newest F5 frame history in one zip, <library>/reports/<title>-<time>.zip
  vitarecomp update-check                   is a newer release out? "update <tag> <page>" or "current" (an install
                                            whose dist named a release channel, NATIV_RELEASES; GitHub asked once a day)
                                            (no game files, saves or keys; the launcher's card: Report a problem)

Everything generated lives in the player's library, under dump/decrypted/<title>/ (pipeline/datadir.py: <root>/dump
when it exists, as in a checkout, else the per-user data folder; ATIV_DUMP=<dir> puts it elsewhere; gitignored: it is
the game's code and data). Per-game facts are in packs/<title>/pack.toml.
A title with a suffix (PCSA00107_112) is another copy of that game in a folder of its own under the
same pack: a second version side by side (the game is still told its own title id).

(The same program on every platform: macOS, Linux and Windows -- no shell. ./vitarecomp runs it with the
repository's Python; the packs, packs/<title>/pack.toml, are read by packconf.py.)
"""
import glob
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
import zlib

R = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(R)   # (pipeline/: this; the repository above it)
RECOMPILER, RUNTIME, PACKS, DECRYPT, DEV = (os.path.join(ROOT, d) for d in ("recompiler", "runtime", "packs", "decrypt", "dev"))
sys.path.insert(0, R)
import packconf  # noqa: E402
from datadir import library_dir  # noqa: E402
from winfs import remove_file, remove_tree  # noqa: E402

WINDOWS = os.name == "nt"
ENV = os.environ


def _python():   # the repository's virtualenv (.venv), else whichever Python runs this
    for p in (os.path.join(ROOT, ".venv", "bin", "python"), os.path.join(ROOT, ".venv", "Scripts", "python.exe")):
        if os.path.exists(p): return p
    return sys.executable


PY = _python()
DUMP = library_dir(ROOT)   # (the player's library: datadir.py)
TOOLS_ENV = {"PYTHONPATH": DECRYPT}   # (vitadecrypt, a package there)
SELF = [sys.executable, os.path.abspath(__file__)]   # ($0: a command that runs another stage)
TITLE_RE = re.compile(r"[A-Z]{4}[0-9]{5}\Z")


def say(*a):
    print(*a, flush=True)


def die(msg="", code=1):
    if msg: say(msg)
    sys.exit(code)


def env_with(*maps, words=()):
    """the environment, then each map, then NAME=value words in order (`env A=1 B=2 cmd`: later wins)"""
    e = dict(ENV)
    for m in maps: e.update(m)
    for w in words:
        k, eq, v = w.partition("=")
        if eq: e[k] = v
    return e


def run(argv, env=None, cwd=None, check=True, **kw):
    """one command, its output passing through; check: a failure stops here with its status (set -e)"""
    sys.stdout.flush()
    r = subprocess.run([str(a) for a in argv], env=env, cwd=cwd, **kw)
    if check and r.returncode: sys.exit(r.returncode)
    return r


def run_filtered(argv, pattern, env=None, first=False, check=True):
    """`cmd | grep pattern` (first: `| head -1`): the lines it prints that match, as they come"""
    sys.stdout.flush()
    p = subprocess.Popen([str(a) for a in argv], env=env, stdout=subprocess.PIPE, text=True, errors="replace")
    shown = False
    for line in p.stdout:
        if (pattern is None or re.search(pattern, line)) and not (first and shown):
            sys.stdout.write(line); sys.stdout.flush(); shown = True
    rc = p.wait()
    if check and rc: sys.exit(rc)
    return rc


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""): h.update(b)
    return h.hexdigest()


def rmtree(p):   # rm -rf (a link: itself; read-only files too, winfs.py)
    if _is_junction(p): os.rmdir(p)
    elif os.path.islink(p) or (os.path.lexists(p) and not os.path.isdir(p)): remove_file(p)
    elif os.path.isdir(p): remove_tree(p)


def _is_junction(p):
    return WINDOWS and hasattr(os.path, "isjunction") and os.path.isjunction(p)


def link_dir(target, link, relative=None):
    """a folder link (ln -sfn): a symbolic link where the system makes them freely, a junction on Windows (no
    privilege needed); relative: the link's text where a symbolic link can be relative"""
    rmtree(link) if os.path.lexists(link) else None
    if WINDOWS:
        import _winapi
        _winapi.CreateJunction(os.path.abspath(target), os.path.abspath(link))
    else:
        os.symlink(relative or target, link)


def natkey(s):   # zsh's (n) glob order: numbers inside names compared as numbers
    return [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", s)]


def head1(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f: return f.readline().rstrip("\n")
    except OSError: return ""


def ppm_to_png(src, dst):   # (sips / ImageMagick were the converters: neither is on every platform)
    data = open(src, "rb").read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    if not m: raise ValueError(f"{src}: not a P6 PPM")
    w, h = int(m.group(1)), int(m.group(2)); px = data[m.end():m.end() + w * h * 3]
    raw = b"".join(b"\0" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    with open(dst, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def usage():
    sys.stdout.write(__doc__)


# VERSIONS (a game pack's): "<version>=<sha256 of its main program>:<status> ..." -- the builds of the game it plays
def version_of(versions, digest):
    for e in versions.split():
        if e.partition("=")[2].split(":")[0] == digest: return e.split("=")[0]
    return ""


# ---- the commands that take a file or folder rather than a title: firmware, scan, licenses and add

def cmd_firmware(T):
    FW = os.path.join(DUMP, "firmware"); tmp = os.path.join(FW, f".new.{os.getpid()}")
    rmtree(tmp); os.makedirs(tmp)
    if run([PY, "-m", "vitadecrypt.pup", os.path.realpath(T), tmp], env=env_with(TOOLS_ENV), check=False).returncode:
        rmtree(tmp); die()
    if not os.path.isfile(os.path.join(tmp, "os0", "us", "libgxm_es4.suprx.elf")):
        rmtree(tmp); die(f"no libgxm_es4 came out of {T}: not a Vita firmware update file?")
    v = head1(os.path.join(tmp, "version.txt")) or "unknown"
    rmtree(os.path.join(FW, v)); os.rename(tmp, os.path.join(FW, v)); say(f"firmware {v} -> {os.path.join(FW, v)}")


def cmd_scan(T, rest):
    out = None
    if "--out" in rest:   # (the hub's: the rows into its file, no shell to redirect them)
        i = rest.index("--out"); out = rest[i + 1]; rest = rest[:i] + rest[i + 2:]
    argv = [PY, os.path.join(R, "packs.py"), "scan", os.path.realpath(T), "--dumps", os.path.join(DUMP, "decrypted"),
            "--firmware", os.path.join(DUMP, "firmware")] + rest
    e = env_with(TOOLS_ENV, {"ATIV_DUMP": DUMP})
    if out:
        with open(out + ".tmp", "w", encoding="utf-8", newline="\n") as f: rc = run(argv, env=e, check=False, stdout=f).returncode
        if rc: os.remove(out + ".tmp"); sys.exit(rc)
        os.replace(out + ".tmp", out)
    else:
        run(argv, env=e)


def cmd_add(T, rest):
    nobuild = "--no-build" in rest; args = [a for a in rest if a != "--no-build"]
    # what the copy is, from its plain param.sfo or PKG header (packs.py info: kind title version content-id)
    r = subprocess.run([PY, os.path.join(R, "packs.py"), "info", os.path.realpath(T)], env=env_with(TOOLS_ENV), capture_output=True, text=True)
    ident = r.stdout.split() if r.returncode == 0 and len(r.stdout.split()) >= 4 else ["game", "-", "-", "-"]
    kind, tid, cid = ident[0], ident[1], ident[3]
    if kind == "addon": die(f"{tid}: {os.path.basename(T)} is an add-on: nothing to add -- the game gets every add-on it asks for (runtime/core/apputil.c)", 0)
    if kind == "firmware": cmd_firmware(T); return
    if kind not in ("game", "update"): die(f"{T}: a {kind}, not a game or its update")
    # no licence given and none in the copy: the player's kept licence for its content id (licenses.py; an update's is its
    # game's), else, for an update, the one its game was added with (license.rif)
    W = os.path.join(DUMP, "decrypted", tid)
    if "--license" not in args and not os.path.isfile(os.path.join(T, "sce_sys", "package", "work.bin")):
        k = subprocess.run([PY, os.path.join(R, "licenses.py"), "find", cid], env=env_with({"ATIV_DUMP": DUMP}), capture_output=True, text=True)
        kept = k.stdout.strip() if k.returncode == 0 else ""
        if kept: args += ["--license", kept]; say(f"{tid}: its licence, kept ({cid})")
        elif kind == "update" and os.path.isfile(os.path.join(W, "license.rif")): args += ["--license", os.path.join(W, "license.rif")]
        elif kind == "update": die(f"{tid}: add the game itself first -- its update decrypts with the game's licence")
        else: say(f"{tid}: no licence kept for it ({cid}): a copy that needs one stops below -- give its licence file (.rif or work.bin, --license), or keep yours first (vitarecomp licenses)")
    tmp = os.path.join(DUMP, "decrypted", f".add.{os.getpid()}"); rif = tmp + ".rif"
    rmtree(tmp); rmtree(rif); os.makedirs(os.path.join(DUMP, "decrypted"), exist_ok=True)
    if run([PY, "-m", "vitadecrypt.game", os.path.realpath(T), tmp] + args + ["--license-out", rif], env=env_with(TOOLS_ENV), check=False).returncode:
        rmtree(tmp); rmtree(rif); die()
    if tid == "-":
        try: tid = json.load(open(os.path.join(tmp, "game.json")))["title_id"]
        except (OSError, ValueError, KeyError): tid = "-"
        W = os.path.join(DUMP, "decrypted", tid)
    if not (TITLE_RE.match(tid) and (kind == "update" or os.path.isfile(os.path.join(tmp, "eboot.bin.elf")))):
        rmtree(tmp); rmtree(rif); die(f"{T}: no title id or no decryptable eboot.bin (see above)")
    os.makedirs(W, exist_ok=True)
    if os.path.isfile(rif): _last_line([PY, os.path.join(R, "licenses.py"), "import", rif], env_with({"ATIV_DUMP": DUMP}))   # (the licence that decrypted it, kept)
    if kind == "game":
        rmtree(os.path.join(W, "app0")); os.rename(tmp, os.path.join(W, "app0")); say(f"{tid}: the game decrypted -> {os.path.join(W, 'app0')}")
        if os.path.isfile(rif): os.replace(rif, os.path.join(W, "license.rif"))
    else:
        rmtree(os.path.join(W, "patch0")); os.rename(tmp, os.path.join(W, "patch0")); say(f"{tid}: its update decrypted -> {os.path.join(W, 'patch0')}")
        rmtree(rif)
        if not os.path.isdir(os.path.join(W, "app0")): die(f"{tid}: kept the update; add the game itself to play it", 0)
    # (--no-build: kept, not imported -- the game alone may be a version the pack refuses that its update makes one it plays)
    if nobuild: die(f"{tid}: kept; the next add (its update) or vitarecomp import {tid} imports it", 0)
    run(SELF + ["import", tid])
    if not os.path.isfile(os.path.join(PACKS, tid, "pack.toml")): die(f"{tid}: imported; there's no game pack for it yet (packs/{tid}/pack.toml)", 0)
    for c in ("analyze", "recompile", "build"): run(SELF + [c, tid])


def _last_line(argv, env):   # `cmd | tail -1`
    r = subprocess.run([str(a) for a in argv], env=env, stdout=subprocess.PIPE, text=True, errors="replace")
    lines = r.stdout.rstrip("\n").split("\n")
    if lines and lines[-1]: say(lines[-1])


# ---- a title's commands: its pack read for the copy's version

class Game:
    def __init__(self, T, cmd):
        self.T = T
        self.W = os.path.join(DUMP, "decrypted", T)
        self.PACK = T.split("_")[0] if re.match(r"[A-Z]{4}[0-9]{5}_", T) else T
        self.CONF = os.path.join(PACKS, self.PACK, "pack.toml")
        # VERSION: which of the pack's builds this copy is (import records it from the main program's hash): the pack's
        # per-version settings (HOOKS, GAME_ADDRS, FUNCTION_SEEDS...) branch on it
        vf = os.path.join(self.W, "version")
        version = open(vf, encoding="utf-8").read().rstrip("\n") if os.path.isfile(vf) else ""
        c = packconf.load(self.CONF, version, ENV)
        elf = os.path.join(self.W, "elf", c["ELF"])
        if not version and c["VERSIONS"] and os.path.isfile(elf) and cmd != "import":   # a copy imported before versions were recorded
            version = version_of(c["VERSIONS"], sha256(elf))
            if version:
                with open(vf, "w") as f: f.write(version + "\n")
                c = packconf.load(self.CONF, version, ENV)
        # TEST_INPUT / TEST_TOUCH / TEST_FRAMES set in the environment override the pack's for one run
        # (a tap tried on Killzone's first dialog)
        for k in ("TEST_INPUT", "TEST_TOUCH", "TEST_FRAMES"):
            if ENV.get(k): c[k] = ENV[k]
        self.c = c; self.VERSION = version
        # TITLE_ID: the title id the game is told it runs as (sceAppMgr's, RECOMP_TITLE) -- the pack's unless the pack says otherwise
        self.TITLE_ID = c.get("TITLE_ID") or self.PACK
        self.OUT = os.path.join(self.W, "recomp")
        self.SEEDS = os.path.join(DUMP, "decrypted", "seeds")   # firmware modules' missed functions (offsets), shared by every game
        # RECOMP_BIN=<name>: build/test a second host binary (<base>_<name>, test_<name>/) alongside the
        # usual one, so two sessions can build and run the same game without clobbering each other
        BIN = ENV.get("RECOMP_BIN") or "host"
        self.TD = "test" + ("_" + ENV["RECOMP_BIN"] if ENV.get("RECOMP_BIN") else "")
        self.BASE = c.get("BASE") or "game"
        # TARGET=windows: build makes <game>_<bin>.exe (runtime/build.py), run/test/capture launch it -- through Wine
        # (WINE, default wine) anywhere but Windows itself; the test directory is its own (test_win...). On Windows
        # it is the only target.
        # RECOMP_RUNNER: a command the run/test/capture binary is started under (a debugger: "lldb --batch -s cmds --"); a test's
        # timeout stops the runner, not the game it launched -- kill that yourself
        self.EXE = f"{self.BASE}_{BIN}"; self.RUNNER = (ENV.get("RECOMP_RUNNER") or "").split()
        self.windows = WINDOWS or ENV.get("TARGET") == "windows"
        if self.windows:
            self.TD += "_win"; self.EXE += ".exe"
            if not WINDOWS: self.RUNNER = (ENV.get("WINE") or "wine").split()
        # MODULES="libc ...": modules recompiled and loaded with the game, each at its own base from
        # 0x88000000; imports resolve to their exports. A name is the game's own sce_module/<name>.suprx
        # when it ships one, else the firmware's (dump/decrypted/us or sys/external: libgxm_es4, ...)
        self.MODS = (c.get("MODULES") or "").split()
        if self.MODS:
            ENV["RECOMP_MODULE_BASES"] = " ".join(os.path.join(self.OUT, "mod_" + m) for m in self.MODS)
            ENV["RECOMP_MODULES"] = ",".join(os.path.join(self.OUT, m + ".img") for m in self.MODS)
        else:
            ENV.pop("RECOMP_MODULE_BASES", None); ENV.pop("RECOMP_MODULES", None)
        # the analysis recompile reads: analyze.py's, else an earlier Ghidra export
        self.DATA = os.path.join(self.W, "analysis", "eboot.data.json")
        if not os.path.isfile(self.DATA): self.DATA = os.path.join(self.W, "ghidra", "eboot.data.json")

    def __getattr__(self, k):   # the pack's variables: g.HOOKS, g.TEST_FRAMES ...
        c = self.__dict__.get("c")
        if c is not None and k.isupper(): return c.get(k, "")
        raise AttributeError(k)

    @staticmethod
    def mod_base(i):
        return 0x88000000 + 0x1000000 * i

    def modelf(self, m):
        # (a console dump's first, then a firmware update file's from vitarecomp firmware, the newest version
        # first: libgxm_es4/libgpu_es4 3.65 -> 3.74 differ only in a version constant and the module fingerprint)
        own = os.path.join(self.W, "elf", m + ".suprx.elf")
        cands = [own, os.path.join(DUMP, "decrypted", "us", m + ".suprx.elf"), os.path.join(DUMP, "decrypted", "sys", "external", m + ".suprx.elf")]
        for sub in (os.path.join("os0", "us"), os.path.join("vs0", "sys", "external")):
            cands += sorted(glob.glob(os.path.join(glob.escape(os.path.join(DUMP, "firmware")), "*", sub, m + ".suprx.elf")), key=natkey, reverse=True)
        for f in cands:
            if os.path.isfile(f): return f
        return own

    def links(self, skip):   # --link for every module but the one named (main: "")
        l = []
        if skip: l += ["--link", f"{os.path.join(self.W, 'elf', self.ELF)}@0x81000000"]
        for i, m in enumerate(self.MODS):
            if m != skip: l += ["--link", f"{self.modelf(m)}@{self.mod_base(i):#x}"]
        return l

    def entry(self):
        with open(self.DATA) as f: return "%x" % (json.load(f)["functions"]["module_start"] & ~1)

    def exe_argv(self, exe):
        return self.RUNNER + [exe]


def cmd_import(g, src):
    W, OUT = g.W, g.OUT
    if not src:   # the title's own copy (add's): the game, with its update layered over it when the pack plays that version
        src = os.path.join(W, "app0")
        if not os.path.isdir(src): die(f"{g.T}: no copy of the game in {W} (vitarecomp add it)")
        if os.path.isdir(os.path.join(W, "patch0")):
            e = os.path.join(W, "patch0", g.ELF)
            if not os.path.isfile(e): e = os.path.join(W, "app0", g.ELF)
            info = subprocess.run([PY, os.path.join(R, "packs.py"), "info", os.path.join(W, "patch0")], env=env_with(TOOLS_ENV), capture_output=True, text=True)
            if info.returncode: sys.stderr.write(info.stderr); sys.exit(info.returncode)
            pv = (info.stdout.split() + ["", "", ""])[2]
            if not g.VERSIONS or version_of(g.VERSIONS, sha256(e)):
                run([PY, os.path.join(R, "packs.py"), "layer", os.path.join(W, "app0"), os.path.join(W, "patch0"), os.path.join(W, "app0_patched")], env=env_with(TOOLS_ENV))
                src = os.path.join(W, "app0_patched")
            else:
                say(f"{g.T}: its update ({pv}) isn't a version packs/{g.PACK}/pack.toml plays yet -- the game runs without it")
    os.makedirs(os.path.join(W, "elf"), exist_ok=True); os.makedirs(os.path.join(OUT, "fs"), exist_ok=True)
    app0 = os.path.join(OUT, "fs", "app0")
    if os.path.isfile(os.path.join(src, "eboot.bin.elf")):   # decrypted on this machine (add): each ELF beside its SELF, and the whole app0 (sce_sys too) read in place
        els = [os.path.join(src, "eboot.bin.elf"), os.path.join(src, g.ELF)] + sorted(glob.glob(os.path.join(glob.escape(os.path.join(src, "sce_module")), "*.elf")))   # (Uncharted's main module is app0's uncharted.self)
        for f in dict.fromkeys(els):
            if not os.path.isfile(f): die(f"cp: {f}: No such file or directory")
            shutil.copyfile(f, os.path.join(W, "elf", os.path.basename(f))); shutil.copymode(f, os.path.join(W, "elf", os.path.basename(f)))
        if os.path.islink(app0) or _is_junction(app0) or not os.path.lexists(app0):   # (a link: re-pointed -- the game, or with its update)
            link_dir(os.path.realpath(src), app0)
    else:
        os.makedirs(app0, exist_ok=True)
        run([PY, os.path.join(DECRYPT, "fself2elf.py"), src, os.path.join(W, "elf")])
        if os.path.isdir(os.path.join(src, "sce_module")): run([PY, os.path.join(DECRYPT, "fself2elf.py"), os.path.join(src, "sce_module"), os.path.join(W, "elf")])
        if os.path.isdir(os.path.join(src, "data")): shutil.copytree(os.path.join(src, "data"), os.path.join(app0, "data"), symlinks=True, dirs_exist_ok=True)
    # VERSIONS (packs/<title>/pack.toml): the builds the pack's hooks and addresses were made for -- another version or region
    # would mis-hook, so say so here rather than crash later; which one this is goes to $W/version, for the pack's per-version settings
    if g.VERSIONS:
        v = version_of(g.VERSIONS, sha256(os.path.join(W, "elf", g.ELF)))
        if not v:
            vs = [e.split("=")[0] for e in g.VERSIONS.split()]
            say(f"{g.T}: this copy's {g.ELF} is no version packs/{g.PACK}/pack.toml plays (it plays {', '.join(vs)}): another version or region, or a modified dump (MaiDump's eboot loads its _m modules)? its hooks won't fit")
            if not ENV.get("FORCE"): die()
            v = "unknown"
        with open(os.path.join(W, "version"), "w") as f: f.write(v + "\n")
        say(f"{g.T}: version {v}")
    run_filtered([PY, os.path.join(RECOMPILER, "imports.py"), os.path.join(W, "elf", g.ELF)], None, first=True)


def cmd_analyze(g):
    W = g.W
    if ENV.get("ANALYZER") == "ghidra":   # the Ghidra route lives with the firmware decompilation's scripts (VITA_FW_DECOMP: its checkout)
        FWD = ENV.get("VITA_FW_DECOMP") or os.path.join(ROOT, "..", "vita-fw-decomp")
        sc = os.path.join(FWD, "scripts", "decomp-module.sh")
        if not os.access(sc, os.X_OK): die(f"ANALYZER=ghidra needs the firmware decomp checkout's scripts (VITA_FW_DECOMP, default {FWD})")
        run([sc, os.path.join(W, "elf", g.ELF), os.path.join(W, "ghidra", "eboot"), "recomp-" + g.T]); return
    A = os.path.join(W, "analysis"); os.makedirs(A, exist_ok=True)
    # seeds.txt: functions runs found the analysis missed (the host records them; test collects them)
    # (a firmware module's: $SEEDS/<module>.txt, offsets from its base, shared by every game)
    seeds = ["--seeds", os.path.join(A, "seeds.txt")] if os.path.isfile(os.path.join(A, "seeds.txt")) else []
    # FUNCTION_SEEDS (the pack's, for this version): the same, found on the developers' runs -- a new copy needs them
    # from its first build ("0x818b28c6:thumb ..." -> "<address> <kind>" lines)
    if g.FUNCTION_SEEDS:
        with open(os.path.join(A, "pack_seeds.txt"), "w") as f:
            for s in g.FUNCTION_SEEDS.split(): f.write(f"{s.split(':')[0]} {s.partition(':')[2]}\n")
        seeds += ["--seeds", os.path.join(A, "pack_seeds.txt")]
    for i, m in enumerate(g.MODS):
        # (recompiler/seeds/<module>.txt: the ones that ship -- a new install's firmware modules need them from the first build;
        # $SEEDS: what this machine's runs found since, merged into the shipped set by hand)
        fw = []
        for f in (os.path.join(RECOMPILER, "seeds", m + ".txt"), os.path.join(g.SEEDS, m + ".txt")):
            if os.path.isfile(f): fw += ["--seeds", f"{f}@{g.mod_base(i):#x}"]
        run([PY, os.path.join(RECOMPILER, "analyze.py"), g.modelf(m), os.path.join(A, m + ".data.json"), "--base", f"{g.mod_base(i):#x}"] + g.links(m) +
            ["--var-base", f"{0xfff80000 + 0x8000 * i:#x}"] + seeds + fw)
    run([PY, os.path.join(RECOMPILER, "analyze.py"), os.path.join(W, "elf", g.ELF), os.path.join(A, "eboot.data.json")] + g.links("") + seeds)


def cmd_recompile(g):
    W, OUT = g.W, g.OUT
    os.makedirs(OUT, exist_ok=True)
    for i, m in enumerate(g.MODS):
        # MOD_HOOKS / MOD_IHOOKS="addr:name ...": HOOKS / IHOOKS for the loaded modules' code (Sony's libraries), each going to
        # the module whose 16 MB slot holds its address (e.g. libgxm's shader patcher: the shader prebuild)
        lo = g.mod_base(i)
        inside = lambda h: lo <= int(h.split(":")[0], 0) < lo + 0x1000000
        mh = [h for h in g.MOD_HOOKS.split() if inside(h)]
        # every game's: libgxm's sceGxmShaderPatcherRegisterProgram (firmware 3.65/3.74 libgxm_es4, +0x15c38) -> runtime/core/hooks.c
        # gxm_register: each program the game registers, for the shader recipes (gfx_gpu.c)
        if m == "libgxm_es4" and ":gxm_register" not in f" {g.MOD_HOOKS} ": mh.append(f"{lo + 0x15c38:#x}:gxm_register")
        mih = [h for h in g.MOD_IHOOKS.split() if inside(h)]
        mhk = (["--hooks", " ".join(mh)] if mh else []) + (["--insn-hooks", " ".join(mih)] if mih else [])
        run_filtered([PY, os.path.join(RECOMPILER, "recomp.py"), os.path.join(W, "analysis", m + ".data.json"), os.path.join(OUT, f"mod_{m}.c"), "--split", "2", "--module", m] + mhk, "functions,")
        run_filtered([PY, os.path.join(RECOMPILER, "mkimage.py"), os.path.join(W, "analysis", m + ".data.json"), os.path.join(OUT, m + ".img"), g.modelf(m), "--module"], "EXPT")
    hk = (["--hooks", g.HOOKS] if g.HOOKS else []) + (["--insn-hooks", g.IHOOKS] if g.IHOOKS else [])
    run([PY, os.path.join(RECOMPILER, "recomp.py"), g.DATA, os.path.join(OUT, g.BASE + ".c"), "--split", g.SPLIT] + hk + (ENV.get("RECOMP_PY_FLAGS") or "").split())   # RECOMP_PY_FLAGS: extra options for an A/B (--unordered-loads)
    run_filtered([PY, os.path.join(RECOMPILER, "mkimage.py"), g.DATA, os.path.join(OUT, g.BASE + ".img"), os.path.join(W, "elf", g.ELF)], "TLS|VARS")
    with open(os.path.join(OUT, ".recompile_inputs"), "w", newline="\n") as f: f.write(recompile_inputs(g) + "\n")   # (ready's)


def recompile_inputs(g):
    """what a title's generated code was made from: recompiler/ and its pack.toml (HOOKS, IHOOKS, MODULES, FUNCTION_SEEDS ...),
    as git hashes a blob of their contents in path order -- dev/build-all.sh's formula before this, so its stamps
    (recomp/.recompile_inputs) stay valid -- and computed without git, so an install has it too"""
    files = [f"packs/{g.PACK}/pack.toml"] if os.path.isfile(g.CONF) else []
    for d, sub, names in os.walk(RECOMPILER):
        sub[:] = [x for x in sub if x != "__pycache__" and not x.startswith(".")]
        # (hidden files aren't the recompiler's: a tar from a Mac leaves ._ metadata beside each file, 2026-10-26)
        files += [os.path.relpath(os.path.join(d, n), ROOT).replace(os.sep, "/") for n in names if not n.endswith(".pyc") and not n.startswith(".")]
    data = b"".join(open(os.path.join(ROOT, f), "rb").read() for f in sorted(files, key=lambda f: f.encode()))
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def tools_version():
    """which tools these are: an install's version.txt (vitarecomp dist); a checkout's commit, and a hash of its uncommitted
    changes when it has some ("" without git: always out of date)"""
    v = os.path.join(ROOT, "version.txt")
    if os.path.isfile(v): return open(v, encoding="utf-8").read().strip()
    head = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    diff = subprocess.run(["git", "-C", ROOT, "diff", "HEAD"], capture_output=True).stdout
    return head + ("+" + hashlib.sha1(diff).hexdigest()[:10] if head and diff else "")


def cmd_ready(g):
    # the stamps recompile and build write: what the code was made from (.recompile_inputs), which tools built the program
    # (.built_version). A pack's hooks only take effect in regenerated code (Killzone's new hooks built fine and never ran
    # until a recompile, 2026-10-25); a runtime change only in a rebuilt program
    def stamp(n):
        f = os.path.join(g.OUT, n)
        return open(f, encoding="utf-8").read().strip() if os.path.isfile(f) else ""
    if not os.path.isfile(os.path.join(g.W, "elf", g.ELF)): die(f"{g.T}: not added yet (vitarecomp add, or + Add game in the launcher)")
    remade = False
    if stamp(".recompile_inputs") != recompile_inputs(g):
        say(f"{g.T}: the recompiler or the game's pack changed since its code was made: making it again")
        run(SELF + ["analyze", g.T]); run(SELF + ["recompile", g.T]); remade = True
    ver = tools_version()
    if remade or not ver or stamp(".built_version") != ver or not os.path.isfile(os.path.join(g.OUT, g.EXE)):
        say(f"{g.T}: building it with these tools ({ver or 'a checkout without git'})")
        run(SELF + ["build", g.T])
    else: say(f"{g.T}: up to date")


def cmd_build(g, mode):
    # RECOMP_RUN_ENV: the pack's RUN_ENV, built into the binary as its defaults (runtime/core/main.c builtin_env): started any other way
    # than `vitarecomp run` -- an old checkout's, a launcher's -- it still plays as the pack says
    # RECOMP_GAME_ADDRS: the pack's GAME_ADDRS ="name=0x... ...", the guest addresses its hooks' host code uses (a lock, a function it
    # calls) for this build of the game -- built in always (tests too), unlike RUN_ENV; hooks.c game_addr()
    # BUILD_EXTRA_ENV="A=1 B=2": more settings built into this binary's defaults, after RUN_ENV's (a diagnostic build a player
    # runs as usual, from the launcher: RECOMP_HUD_LOG=2 to log their session's HUD layout)
    run_env = " ".join(x for x in (g.RUN_ENV, ENV.get("BUILD_EXTRA_ENV") or "") if x)
    # the game's own host code (runtime/build.py RECOMP_PACK_SOURCES): every .c in its pack -- hooks.c, a stand-in for its
    # long-gone game server (Killzone's server.c)
    srcs = sorted(glob.glob(os.path.join(glob.escape(os.path.join(PACKS, g.PACK)), "*.c")))
    e = env_with({"RECOMP_RUN_ENV": run_env, "RECOMP_GAME_ADDRS": g.GAME_ADDRS, "RECOMP_PACK_SOURCES": os.pathsep.join(srcs)})
    base = os.path.join(g.OUT, g.BASE)
    out, extra = {"verify": (base + "_verify", {"VERIFY": "1"}), "profile": (base + "_prof", {"PROFILE": "1"})}.get(mode, (os.path.join(g.OUT, g.EXE.removesuffix(".exe")), {}))
    if g.windows: extra["TARGET"] = "windows"
    mods = [os.path.join(g.OUT, "mod_" + m) for m in g.MODS]
    run([PY, os.path.join(RUNTIME, "build.py"), base, out] + mods, env=env_with(e, extra))
    if mode not in ("verify", "profile") and not ENV.get("RECOMP_BIN"):   # (ready's: which tools built the play program)
        with open(os.path.join(g.OUT, ".built_version"), "w", newline="\n") as f: f.write(tools_version() + "\n")


def game_env(g, words, extra):
    """the game's process environment: words (RUN_ENV / TEST_ENV, then EXTRA_ENV), then the fixed settings in order"""
    e = env_with(words=words)
    for k, v in extra: e[k] = v
    return e


# Windows' Smart App Control, when on, has Microsoft's cloud judge each unsigned program Windows doesn't know, once per file,
# and a game built here is always new to it: now and then a build is refused (ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION). Said
# plainly; the launcher shows its Smart App Control dialog on this line ("Smart App Control is on")
SAC_BLOCKED = ("Windows blocked the game: Smart App Control is on, and it refused this build, which is new to Windows (built on this PC "
               "from your copy). Its verdict is made once for each file: a rebuild (vitarecomp build, or Rebuild in the launcher) is a new "
               "file that usually starts. If that's refused too, turn Smart App Control off in Windows Security > App & browser control > "
               "Smart App Control settings.")


def start_game(argv, **kw):   # subprocess.Popen for a game, with Windows' refusals said plainly
    try: return subprocess.Popen(argv, **kw)
    except OSError as e:
        if getattr(e, "winerror", 0) == 4551: die(SAC_BLOCKED)
        if getattr(e, "winerror", 0) == 1260: die(f"Windows blocked the game: a policy on this PC (AppLocker or similar) refuses {argv[0]}")
        raise


def tee_run(argv, env, cwd, log_path):
    """the game, its stderr to the terminal and to log_path (2> >(tee log >&2)): its exit status"""
    sys.stdout.flush()
    with open(log_path, "wb") as log:
        p = start_game(argv, env=env, cwd=cwd, stderr=subprocess.PIPE)
        def pump():
            err = getattr(sys.stderr, "buffer", None)
            for b in iter(lambda: p.stderr.read1(65536) if hasattr(p.stderr, "read1") else p.stderr.read(4096), b""):
                log.write(b); log.flush()
                if err: err.write(b); err.flush()
        t = threading.Thread(target=pump, daemon=True); t.start()
        try: rc = p.wait()
        except KeyboardInterrupt: p.terminate(); rc = p.wait()
        t.join()
    return rc


def cmd_run(g, frames):
    OUT = g.OUT
    os.makedirs(os.path.join(OUT, "captures"), exist_ok=True)   # F12 in the window saves captures here
    # RUN_ENV: the game's play settings (correctness fixes it needs); TEST_ENV is the gate's and pins the software renderer -- applying it here ran every manual play on the interpreter
    # session.log: this session's stderr (the terminal still shows it; the one before is session.prev.log), with the hitch
    # log on for frames over 50 ms -- what a first-use stutter did, from the player's own session (RECOMP_HITCH_LOG=0: off)
    # RECOMP_SHADER_RECIPES_SHIPPED: packs/<title>/shaders.txt, the shader recipes that ship (gfx_gpu.c: built as the game registers each program)
    shipped = os.path.join(PACKS, g.PACK, "shaders.txt")
    icon = os.path.join(ROOT, "assets", "brand", "png", "icon-" + ("macos-512" if sys.platform == "darwin" else "256") + ".png")   # (the window's: window.c)
    fixed = ([("RECOMP_SHADER_RECIPES_SHIPPED", shipped)] if os.path.isfile(shipped) else []) + ([("RECOMP_ICON", icon)] if os.path.isfile(icon) and sys.platform != "win32" else []) + [
        ("RECOMP_TITLE", g.TITLE_ID), ("RECOMP_GAME_NAME", g.NAME), ("RECOMP_LOG", ENV.get("RECOMP_LOG") or "0"),
        ("RECOMP_HITCH_LOG", ENV.get("RECOMP_HITCH_LOG") or "50"), ("RECOMP_STALL_PROF", ENV.get("RECOMP_STALL_PROF") or "150")]
    # ATIV_PRE_LAUNCH / ATIV_POST_EXIT (the environment): commands run before the game starts and after it exits. On a machine
    # whose GPU is shared (a local AI model kept in VRAM, say) the GPU must be free BEFORE the game starts: a game that starts
    # into full VRAM keeps its allocations in system memory for the whole session, even once the model is gone (an Uncharted
    # menu at 12 fps on a launch 34 s after a model reloaded, 2026-10-22)
    if ENV.get("ATIV_PRE_LAUNCH"):
        rc = subprocess.run(ENV["ATIV_PRE_LAUNCH"], shell=True).returncode
        if rc: sys.exit(rc)
    try:
        if os.path.isfile(os.path.join(OUT, "session.log")): os.replace(os.path.join(OUT, "session.log"), os.path.join(OUT, "session.prev.log"))
        e = game_env(g, g.RUN_ENV.split() + (ENV.get("EXTRA_ENV") or "").split(), fixed)
        rc = tee_run(g.exe_argv(os.path.join(OUT, g.EXE)) + [g.BASE + ".img", g.entry(), frames or "100000000000"], e, OUT, os.path.join(OUT, "session.log"))
    finally:
        if ENV.get("ATIV_POST_EXIT"): subprocess.run(ENV["ATIV_POST_EXIT"], shell=True)
    sys.exit(rc)


def fresh_fs(g, td):
    """a test directory with a fresh file system (the game's app0, no saves) and no settings.ini (a test runs on defaults,
    not on what a menu or F2 saved last time)"""
    os.makedirs(td, exist_ok=True)
    for f in glob.glob(os.path.join(glob.escape(td), "shot_*.ppm")): os.remove(f)
    rmtree(os.path.join(td, "fs")); rmtree(os.path.join(td, "settings.ini"))
    os.makedirs(os.path.join(td, "fs"))
    link_dir(os.path.join(g.OUT, "fs", "app0"), os.path.join(td, "fs", "app0"), relative=os.path.join("..", "..", "fs", "app0"))


def test_timeout(g):   # a scripted run's limit in seconds: TIMEOUT for one run, else the pack's TEST_TIMEOUT, else 300
    return float(ENV.get("TIMEOUT") or g.c.get("TEST_TIMEOUT") or ENV.get("TEST_TIMEOUT") or 300)


def timed(argv, env, cwd, stdout, stderr, seconds):
    """`timeout <seconds> cmd || true`: stopped (SIGTERM) when the time is up"""
    p = start_game(argv, env=env, cwd=cwd, stdout=stdout, stderr=stderr)
    try: return p.wait(timeout=seconds)
    except subprocess.TimeoutExpired: p.terminate(); return p.wait()


def cmd_test(g):
    OUT, W = g.OUT, g.W
    td = os.path.join(OUT, g.TD)
    fresh_fs(g, td)
    if ENV.get("TEST_SAVES"): shutil.copytree(ENV["TEST_SAVES"], os.path.join(td, "fs", "savedata0"), symlinks=True)   # TEST_SAVES=<a savedata0 folder>: start from those saves instead (the save dialog's list with saves in it)
    if ENV.get("TEST_SETTINGS"): shutil.copyfile(ENV["TEST_SETTINGS"], os.path.join(td, "settings.ini"))   # TEST_SETTINGS=<a settings.ini>: start from a player's settings instead of the defaults (a problem only their setup shows)
    # TEST_TOUCH: scripted front-panel taps; TEST_ENV: the game's own settings (e.g. its real shaders)
    # the run ends once the test frame's shot is written (RECOMP_SHOT_LAST); SHOT_EVERY=n: a shot every n frames up to it
    frames = g.TEST_FRAMES; every = ENV.get("SHOT_EVERY") or ""
    e = game_env(g, g.TEST_ENV.split() + (ENV.get("EXTRA_ENV") or "").split(), [
        ("RECOMP_GFX_COVERAGE", "gfxcov.json"), ("RECOMP_TITLE", g.TITLE_ID), ("RECOMP_GAME_NAME", g.NAME), ("RECOMP_INPUT", g.TEST_INPUT),
        ("RECOMP_TOUCH", g.TEST_TOUCH), ("RECOMP_HEADLESS", "1"), ("RECOMP_FS", "fs"), ("RECOMP_LOG", "0"), ("RECOMP_SHOT", every or frames),
        ("RECOMP_SHOT_LAST", frames)])   # (a series too ends at the test frame: it ran on to the timeout, 15 minutes for shots it had long since taken)
    with open(os.path.join(td, "test.log"), "wb") as log:
        timed(g.exe_argv(os.path.join(OUT, g.EXE)) + [os.path.join("..", g.BASE + ".img"), g.entry(), "100000000000"], e, td, subprocess.DEVNULL, log,
              test_timeout(g))
    missed = os.path.join(td, "missed_functions.txt")
    if os.path.isfile(missed) and os.path.getsize(missed):
        # each address to its module: a firmware module's as an offset into $SEEDS/<module>.txt, the rest the game's
        for ln in open(missed).read().splitlines():
            if not ln.split(): continue
            a, kind = ln.split()[0], " ".join(ln.split()[1:])
            f, line = os.path.join(W, "analysis", "seeds.txt"), f"{a} {kind}"
            for i, m in enumerate(g.MODS):
                b = g.mod_base(i)
                if b <= int(a, 0) < b + 0x1000000 and not g.modelf(m).startswith(os.path.join(W, "elf") + os.sep):
                    os.makedirs(g.SEEDS, exist_ok=True); f = os.path.join(g.SEEDS, m + ".txt"); line = f"{int(a, 0) - b:#x} {kind}"
            have = open(f).read().splitlines() if os.path.isfile(f) else []
            with open(f, "w") as o: o.write("".join(x + "\n" for x in sorted(set(have + [line]))))
            say(f"the run found a function the analysis missed: {line} in {os.path.basename(f)} (re-run analyze, recompile, build)")
    rmtree(missed)
    shot = os.path.join(td, "shot_%05d.ppm" % int(frames))
    if not os.path.isfile(shot):
        say(f"no frame {frames} (see {os.path.join(td, 'test.log')})")
        sys.stdout.write("".join(open(os.path.join(td, "test.log"), errors="replace").readlines()[-5:])); die()
    try: ppm_to_png(shot, os.path.join(td, "test.png"))
    except (OSError, ValueError): pass
    say(f"frame {frames}: {os.path.join(td, 'test.png')}")


def cmd_capture(g, n):
    if not n: die("vitarecomp capture: a frame number")
    OUT = g.OUT; td = os.path.join(OUT, g.TD); vfc = os.path.join(OUT, "captures", f"frame_{n}.vfc")
    os.makedirs(os.path.join(OUT, "captures"), exist_ok=True)
    fresh_fs(g, td)
    e = game_env(g, g.TEST_ENV.split() + (ENV.get("EXTRA_ENV") or "").split(), [
        ("RECOMP_CAPTURE", n), ("RECOMP_CAPTURE_FILE", vfc), ("RECOMP_CAPTURE_EXIT", "1"), ("RECOMP_TITLE", g.TITLE_ID), ("RECOMP_INPUT", g.TEST_INPUT),
        ("RECOMP_TOUCH", g.TEST_TOUCH), ("RECOMP_HEADLESS", "1"), ("RECOMP_FS", "fs"), ("RECOMP_LOG", "0")])
    with open(os.path.join(td, "capture.log"), "wb") as log:
        timed(g.exe_argv(os.path.join(OUT, g.EXE)) + [os.path.join("..", g.BASE + ".img"), g.entry(), "100000000000"], e, td, subprocess.DEVNULL, log,
              test_timeout(g))
    if not os.path.isfile(vfc): die(f"no capture (see {os.path.join(td, 'capture.log')})")
    say(f"{vfc} ({os.path.getsize(vfc) / (1 << 20):.0f}M)")


def cmd_check(titles):
    # every named game: build (in turn: the host objects are shared), then the scripted tests one
    # at a time -- run at once they slow each other down and the games' wall-clock-driven menus land
    # on a different screen at the test frame (Unit 13's frame 700 went from its menu to the training
    # prompt; CHECK_PARALLEL=1 runs them at once anyway) -- then each frame against the reference
    # `accept` stored
    fail = 0; built = []
    for t in titles:
        r = subprocess.run(SELF + ["build", t], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
        errs = [l for l in r.stdout.splitlines() if re.search(" error", l)][:3]
        if errs or r.returncode:
            say(f"FAIL {t}: build: " + ("\n".join(errs) if errs else "exit %d: %s" % (r.returncode, " / ".join(r.stdout.strip().splitlines()[-3:]))))
            fail = 1; continue
        built.append(t)
    quiet = dict(stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if ENV.get("CHECK_PARALLEL"):
        for p in [subprocess.Popen(SELF + ["test", t], **quiet) for t in built]: p.wait()
    else:
        for t in built: subprocess.run(SELF + ["test", t], **quiet)
    for t in built:
        tg = Game(t, "check")
        rec = tg.OUT; fr = tg.TEST_FRAMES; tol = tg.TEST_TOLERANCE or "1"
        shot = os.path.join(rec, tg.TD, "shot_%05d.ppm" % int(fr)); ref = os.path.join(rec, f"ref_{fr}.ppm")
        if not os.path.isfile(shot): say(f"FAIL {t}: no frame {fr} (see {os.path.join(rec, tg.TD, 'test.log')})"); fail = 1; continue
        if not os.path.isfile(ref): say(f"SKIP {t}: no reference (vitarecomp accept {t})"); continue
        d = subprocess.run([PY, os.path.join(DEV, "framediff.py"), ref, shot], capture_output=True, text=True).stdout.strip()
        if not re.fullmatch(r"\d+\.\d+", d): say(f"FAIL {t}: {d}"); fail = 1
        elif d == "0.00": say(f"PASS {t}: frame {fr} identical to the reference")
        elif int(d.split(".")[0]) < int(tol): say(f"PASS {t}: frame {fr} differs in {d}% of pixels (tolerance {tol}%)")
        else: say(f"FAIL {t}: frame {fr} differs in {d}% of pixels from the reference (tolerance {tol}%)"); fail = 1
    sys.exit(fail)


def cmd_verify(g, seconds):
    OUT = g.OUT; vd = os.path.join(OUT, "verify"); os.makedirs(vd, exist_ok=True)
    e = env_with({"RECOMP_INPUT": g.TEST_INPUT, "RECOMP_HEADLESS": "1", "RECOMP_FS": os.path.join("..", "fs"), "RECOMP_LOG": "0",
                  "RECOMP_VERIFY": ENV.get("RECOMP_VERIFY") or "1", "RECOMP_VERIFY_LOG": "mismatches.log"})
    with open(os.path.join(vd, "verify.log"), "wb") as log:
        timed([os.path.join(OUT, g.BASE + "_verify"), os.path.join("..", g.BASE + ".img"), g.entry(), "100000000000"], e, vd, subprocess.DEVNULL, log, float(seconds or 180))
    lines = [l for l in open(os.path.join(vd, "verify.log"), errors="replace").read().splitlines() if "== verify" in l]
    if lines: say(lines[-1])
    mm = os.path.join(vd, "mismatches.log")
    if os.path.isfile(mm): sys.stdout.write(open(mm, errors="replace").read())


def cmd_recipes(g):
    # the header once, then every record in the order play first met it (the prebuild builds in that order): the shipped
    # set's, then what this machine recorded that it lacks
    local_r = os.path.join(g.OUT, "shader_cache", "recipes.txt"); ship = os.path.join(PACKS, g.PACK, "shaders.txt")
    if not os.path.isfile(local_r): die(f"{g.T}: no recorded recipes ({local_r}): play first")
    rec = open(local_r, encoding="utf-8").read().splitlines(); hdr = rec[0] if rec else ""
    old = open(ship, encoding="utf-8").read().splitlines() if os.path.isfile(ship) else None
    if old is not None and (old[0] if old else "") != hdr: die(f"{ship} is another recipe version than {local_r}: not merged")
    before = len(old) - 1 if old is not None else 0
    seen, body = set(), []
    for l in (old or [])[1:] + rec[1:]:
        if l.split() and l not in seen: seen.add(l); body.append(l)
    with open(ship + ".tmp", "w", encoding="utf-8", newline="\n") as f: f.write("".join(x + "\n" for x in [hdr] + body))
    os.replace(ship + ".tmp", ship)
    say(f"{g.T}: {len(body)} recipes in {os.path.basename(ship)} ({before} before; {sum(l.startswith('S ') for l in body)} shaders, {sum(l.startswith('P ') for l in body)} pipelines)")


# dist: the install (notes/shipping-shape.md, "Where things go"): what a player's copy needs to turn their games into
# programs, and nothing only a developer uses (tests, dev/, docs/, servers/, the image sources, the shader sources:
# builds read vk_shaders.h). The library is not in it (datadir.py), so a new dist written over an install replaces the
# program and leaves the player's games, builds, saves and Python where they are.
SHIPS = ("vitarecomp", "README.md", "LICENSE", "THIRD_PARTY.md", "requirements.txt", "launcher/", "pipeline/", "recompiler/",
         "decrypt/", "runtime/", "packs/", "third_party/", "assets/brand/png/", "assets/brand/nativ.ico", "assets/brand/nativ.icns",
         "dev/framediff.py")   # (check's comparison: an install can gate itself)
NOT_SHIPPED = ("decrypt/vitadecrypt/tests/", "runtime/tests/", "runtime/gfx/shaders/", "runtime/debug/gfxreplay.c",
               "runtime/debug/build_replay.sh", "third_party/kenney-prompts/make.py")
BUILT = ("launcher/nativ", "launcher/nativ.exe", "launcher/SDL3.dll", "launcher/libSDL3.so.0")   # (the launcher as built here: untracked, shipped when present)
DIST_LIST = ".nativ-dist"   # (the files a dist wrote: the next one over it removes those it no longer ships, and only those)


def cmd_dist(out):
    tracked = subprocess.run(["git", "-C", ROOT, "ls-files", "-z"], capture_output=True, text=True, check=True).stdout.split("\0")
    def ships(f): return any(f == s or (s.endswith("/") and f.startswith(s)) for s in SHIPS) and not f.startswith(NOT_SHIPPED)
    built = set(BUILT) if os.environ.get("NATIV_DIST_BUILT", "1") != "0" else set()   # (0: an install for another OS, whose launcher its script builds)
    files = sorted({f for f in tracked if f and ships(f)} | built)
    files = [f for f in files if os.path.isfile(os.path.join(ROOT, f))]
    out = os.path.abspath(out); listing = os.path.join(out, DIST_LIST)
    if os.path.realpath(out) == os.path.realpath(ROOT): die("vitarecomp dist: a folder other than the repository itself")
    old = set()
    if os.path.isfile(listing): old = set(open(listing, encoding="utf-8").read().split("\n")) - {""}
    elif os.path.isdir(out) and os.listdir(out): die(f"vitarecomp dist: {out} holds files a dist didn't write: a new folder, or one a dist made")
    for f in files:
        d = os.path.join(out, f); os.makedirs(os.path.dirname(d), exist_ok=True); shutil.copy2(os.path.join(ROOT, f), d)
    gone = 0
    for f in sorted(old - set(files) - {"version.txt", UPDATES}):   # (shipped before, not now; a listing entry outside the folder is ignored)
        if os.path.isabs(f) or os.path.normpath(f).startswith(".."): continue
        p = os.path.join(out, f)
        if os.path.isfile(p): remove_file(p); gone += 1
        d = os.path.dirname(p)
        while d != out and os.path.isdir(d) and not os.listdir(d): os.rmdir(d); d = os.path.dirname(d)
    ver = subprocess.run(["git", "-C", ROOT, "describe", "--tags", "--always", "--dirty"], capture_output=True, text=True).stdout.strip()   # (a release's tag: what update-check compares)
    if ver.endswith("-dirty"):   # (which changes: ready rebuilds a game when the version differs, and two dirty dists must)
        ver += "+" + hashlib.sha1(subprocess.run(["git", "-C", ROOT, "diff", "HEAD"], capture_output=True).stdout).hexdigest()[:10]
    with open(os.path.join(out, "version.txt"), "w", newline="\n") as v: v.write(ver + "\n")   # (not VERSION: on a
    # case-insensitive disk that is C++'s <version> to a build with the install on its include path)
    extra = ["version.txt"]
    rel = os.environ.get("NATIV_RELEASES", "").strip()   # (owner/repo whose GitHub releases are this install's updates)
    if rel:
        if not re.fullmatch(r"[\w.-]+/[\w.-]+", rel): die(f"vitarecomp dist: NATIV_RELEASES={rel}: owner/repo")
        with open(os.path.join(out, UPDATES), "w", newline="\n") as v: v.write(f"github {rel}\n")
        extra.append(UPDATES)
    elif os.path.isfile(os.path.join(out, UPDATES)) and UPDATES in old: remove_file(os.path.join(out, UPDATES))
    with open(listing, "w", encoding="utf-8", newline="\n") as v: v.write("\n".join(files + extra) + "\n")
    say(f"dist {ver}: {len(files)} files -> {out}" + (f" ({gone} no longer shipped, removed)" if gone else ""))
    if not any(f.startswith("launcher/nativ") for f in files): say("(no launcher built here: launcher/build.sh first, TARGET=windows for nativ.exe)")


UPDATES = "updates.txt"   # (an install's release channel, "github <owner>/<repo>": cmd_dist writes it, cmd_update_check reads it)
def release_newer(tag, mine):
    """a release tag newer than this install's version, both vX.Y.Z (a build that isn't a release is never told)"""
    def parts(v): m = re.fullmatch(r"v?(\d+)\.(\d+)(?:\.(\d+))?", v.strip()); return tuple(int(x or 0) for x in m.groups()) if m else None
    a, b = parts(tag), parts(mine)
    return bool(a and b and a > b)
def cmd_update_check():
    """a newer release of the channel this install came from: "update <tag> <page>", "current", or nothing (no channel,
    offline). The answer is kept a day, so a launcher starting often asks GitHub at most once a day"""
    chan = os.path.join(ROOT, UPDATES)
    if not os.path.isfile(chan): return
    kind, _, repo = open(chan, encoding="utf-8").read().strip().partition(" ")
    if kind != "github" or not re.fullmatch(r"[\w.-]+/[\w.-]+", repo): return
    from datadir import prefs_dir
    cache = os.path.join(prefs_dir(), "update-check.json"); now = time.time(); latest = None
    try:
        c = json.load(open(cache, encoding="utf-8"))
        if c.get("repo") == repo and now - c.get("at", 0) < 20 * 3600: latest = c
    except (OSError, ValueError): pass
    if latest is None:
        import ssl, urllib.request
        # (the release list, not /releases/latest: that one leaves out pre-releases, and beta builds are published as those)
        req = urllib.request.Request(f"https://api.github.com/repos/{repo}/releases?per_page=30", headers={"User-Agent": "ProjectNativ", "Accept": "application/vnd.github+json"})
        ctx = ssl.create_default_context()
        for ca in ("/etc/ssl/cert.pem", "/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt"):   # (a standalone Python finds no system store on macOS and Linux)
            if os.path.isfile(ca): ctx.load_verify_locations(ca); break
        try:
            with urllib.request.urlopen(req, timeout=6, context=ctx) as r: rels = json.load(r)
        except Exception as e: say(f"update-check: {repo}: {e}"); return
        best = {"tag_name": "", "html_url": ""}   # (the highest vX.Y.Z tag; a draft, or a tag named otherwise, is never offered)
        for d in rels if isinstance(rels, list) else []:
            if not d.get("draft") and release_newer(d.get("tag_name", ""), best["tag_name"] or "v0.0.0"): best = d
        latest = {"repo": repo, "at": now, "tag": best.get("tag_name", ""), "page": best.get("html_url", "")}
        try:
            os.makedirs(os.path.dirname(cache), exist_ok=True)
            with open(cache, "w", encoding="utf-8") as f: json.dump(latest, f)
        except OSError: pass
    print(f"update {latest['tag']} {latest['page']}" if release_newer(latest["tag"], tools_version()) else "current")


def cmd_versions(g):
    if not g.VERSIONS: die(f"{g.T}: packs/{g.PACK}/pack.toml records no versions", 0)
    for e in g.VERSIONS.split():
        v = e.split("=")[0]
        say(f"{g.NAME} {v}: {e.rsplit(':', 1)[-1]}" + ("  <- this copy" if v == g.VERSION else ""))
    if g.UPDATE_NOTE: say(g.UPDATE_NOTE)


def _machine():   # about.txt's lines on the computer: what the logs don't say
    import platform
    cpu, ram = platform.processor(), 0
    try:
        if sys.platform == "darwin":
            cpu = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True, text=True).stdout.strip() or cpu
            ram = int(subprocess.run(["sysctl", "-n", "hw.memsize"], capture_output=True, text=True).stdout.strip() or 0)
        elif sys.platform.startswith("linux"):
            m = re.search(r"model name\s*:\s*(.*)", open("/proc/cpuinfo", encoding="utf-8", errors="replace").read()); cpu = m.group(1) if m else cpu
            m = re.search(r"MemTotal:\s*(\d+) kB", open("/proc/meminfo", encoding="utf-8").read()); ram = int(m.group(1)) * 1024 if m else 0
    except (OSError, ValueError): pass
    return [("system", platform.platform()), ("processor", f"{cpu} ({platform.machine()}, {os.cpu_count()} threads)"),
            ("memory", f"{ram / 2**30:.0f} GB" if ram else "unknown")]


def cmd_report(T):
    """a bug report: one zip of what a problem needs and nothing private: the game's last two session logs, the launcher's
    log, both settings files, the newest F5 frame history, and about.txt (versions, the computer). No game files, saves,
    F12 captures (they hold the game's memory) or online key. The home folder's path is written as ~ in the text files"""
    import time
    import zipfile
    from datadir import prefs_dir
    if not TITLE_RE.match(T.split("_")[0]): die(f"vitarecomp report: a title id, such as PCSA00107 (not {T})")
    W = os.path.join(DUMP, "decrypted", T); OUT = os.path.join(W, "recomp")
    try: g = Game(T, "report"); name, version = g.NAME, g.VERSION or "unknown"
    except (Exception, SystemExit): name, version = T, "unknown"   # (no pack, or a copy the pack can't read: report what's there)
    ver = os.path.join(ROOT, "version.txt")
    nativ = open(ver, encoding="utf-8").read().strip() if os.path.isfile(ver) else \
        subprocess.run(["git", "-C", ROOT, "describe", "--always", "--dirty"], capture_output=True, text=True).stdout.strip() or "unknown"
    home = os.path.expanduser("~")
    def private(b): return b.replace(home.encode(), b"~").replace(home.replace("\\", "/").encode(), b"~")
    files = [(os.path.join(OUT, "session.log"), "session.log"), (os.path.join(OUT, "session.prev.log"), "session.prev.log"),
             (os.path.join(prefs_dir(), "launcher.log"), "launcher.log"), (os.path.join(prefs_dir(), "launcher.prev.log"), "launcher.prev.log"),
             (os.path.join(OUT, "settings.ini"), "settings-game.ini"), (os.path.join(prefs_dir(), "settings.ini"), "settings.ini")]
    f5 = sorted(glob.glob(os.path.join(glob.escape(OUT), "captures", "f5_*")), key=os.path.getmtime)
    f5 = f5[-1] if f5 else ""
    stamp = lambda p: time.strftime("%Y-%m-%d %H:%M", time.localtime(os.path.getmtime(p)))
    about = [("game", f"{name} ({T}), version {version}"), ("Project Nativ", nativ)] + _machine() + \
        [(n, f"written {stamp(p)}") for p, n in files if os.path.isfile(p)] + \
        ([("F5 frame history", f"{os.path.basename(f5)}, saved {stamp(f5)}")] if f5 else [("F5 frame history", "none saved")])
    os.makedirs(os.path.join(DUMP, "reports"), exist_ok=True)
    out = os.path.join(DUMP, "reports", f"{T}-{time.strftime('%Y%m%d-%H%M%S')}.zip")
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("about.txt", "".join(f"{k}: {v}\n" for k, v in about) +
                   "\nThis report holds logs, settings and any F5 pictures of the game's last frames: no game files, saves or keys.\n")
        for p, n in files:
            if os.path.isfile(p): z.writestr(n, private(open(p, "rb").read()))
        for p in sorted(glob.glob(os.path.join(glob.escape(f5), "*"))) if f5 else []:
            z.write(p, os.path.basename(f5) + "/" + os.path.basename(p), zipfile.ZIP_STORED if p.endswith(".png") else zipfile.ZIP_DEFLATED)
    say(f"report: {out} ({os.path.getsize(out) / 2**20:.1f} MB)")


def main(argv):
    try: sys.stdout.reconfigure(line_buffering=True)
    except AttributeError: pass
    cmd = argv[0] if argv else ""; T = argv[1] if len(argv) > 1 else ""
    if cmd == "update-check": cmd_update_check(); return 0
    if not cmd or (not T and cmd != "licenses"): usage(); return 1
    if cmd == "firmware": cmd_firmware(T); return 0
    if cmd == "scan": cmd_scan(T, argv[2:]); return 0
    if cmd == "licenses": return run([PY, os.path.join(R, "licenses.py")] + argv[1:], env=env_with({"ATIV_DUMP": DUMP}), check=False).returncode
    if cmd == "add": cmd_add(T, argv[2:]); return 0
    if cmd == "dist": cmd_dist(T); return 0
    if cmd == "report": cmd_report(T); return 0
    if cmd not in ("import", "analyze", "recompile", "build", "ready", "run", "test", "capture", "replay", "accept", "check", "coverage", "verify", "recipes", "versions"):
        usage(); return 1
    g = Game(T, cmd)
    arg3 = argv[2] if len(argv) > 2 else ""
    if cmd == "import": cmd_import(g, arg3)
    elif cmd == "analyze": cmd_analyze(g)
    elif cmd == "recompile": cmd_recompile(g)
    elif cmd == "build": cmd_build(g, arg3)
    elif cmd == "ready": cmd_ready(g)
    elif cmd == "run": cmd_run(g, arg3)
    elif cmd == "test": cmd_test(g)
    elif cmd == "capture": cmd_capture(g, arg3)
    elif cmd == "replay":
        if not arg3: die("vitarecomp replay: a frame number")
        run([os.path.join(RUNTIME, "debug", "build_replay.sh")])
        run([os.path.join(ROOT, "build", "gfxreplay"), os.path.join(g.OUT, "captures", f"frame_{arg3}.vfc"),
             argv[3] if len(argv) > 3 else os.path.join(g.OUT, "captures", f"frame_{arg3}.ppm")])
    elif cmd == "accept":
        f = os.path.join(g.OUT, g.TD, "shot_%05d.ppm" % int(g.TEST_FRAMES))
        if not os.path.isfile(f): die(f"no test frame for {T}: run vitarecomp test {T} first")
        ref = os.path.join(g.OUT, f"ref_{g.TEST_FRAMES}.ppm"); shutil.copyfile(f, ref); say(f"{T}: reference frame {g.TEST_FRAMES} -> {ref}")
    elif cmd == "check": cmd_check(argv[1:])
    elif cmd == "coverage":   # every named game's last test run (vitarecomp test), side by side
        run([PY, os.path.join(DEV, "gfxcov.py")] + [os.path.join(DUMP, "decrypted", t, "recomp", g.TD, "gfxcov.json") for t in argv[1:]])
    elif cmd == "verify": cmd_verify(g, arg3)
    elif cmd == "recipes": cmd_recipes(g)
    elif cmd == "versions": cmd_versions(g)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
