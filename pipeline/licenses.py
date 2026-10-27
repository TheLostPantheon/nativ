#!/usr/bin/env python3
"""The player's licences, imported once and kept: <dump>/licenses/<content id>.rif (dump/ in the checkout, or
ATIV_DUMP). Adding a game, its update or a PKG looks here by the copy's content id before asking for one, as Vita3K
looks in its ux0/license; every licence that decrypts a copy, and every one a folder search finds, comes here.

  licenses.py                              the kept licences: content id, title id
  licenses.py import <file|folder>...      .rif and work.bin files (a folder: at any depth -- a copied ux0:nonpdrm, ux0:app)
  licenses.py vita <address>[:port]        from the Vita itself over VitaShell's FTP (VitaShell, SELECT; port 1337):
                                           NoNpDrm's ux0:nonpdrm/license, and each ux0:app/<title> and
                                           ux0:addcont/<title>/<add-on> folder's sce_sys/package/work.bin
  licenses.py vita3k [folder]              from a Vita3K install's ux0/license (its default folders, or the one named)
  licenses.py find <content id>            the kept licence's path (exit 1: none)
  licenses.py selftest

Only a licence that decrypts away from a console is kept: NoNpDrm's kind (account id 0x0123456789ABCDEF -- a work.bin
or a NoNpDrm .rif). A PSN licence as the Vita keeps it (ux0:license) is bound to that console's account; started once
with the NoNpDrm plugin on, the game gets one of the kind kept here.
"""
from __future__ import annotations

import ftplib
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "decrypt"))
from vitadecrypt import rif as RIF  # noqa: E402
from datadir import library_dir  # noqa: E402

FAKE_AID = 0x0123456789ABCDEF


def store_dir() -> str:
    here = os.path.dirname(os.path.abspath(__file__))
    dump = library_dir(os.path.dirname(here))
    return os.path.join(dump, "licenses")


def _title_like(s: str) -> bool:
    return len(s) == 9 and s[:4].isalpha() and s[:4].isupper() and s[4:].isdigit()


def add_bytes(data: bytes) -> tuple[str, str]:
    """One licence into the store: ("added" | "known" | "console" | "bad", content id)."""
    if len(data) not in (RIF.RIF_SIZE, 2 * RIF.RIF_SIZE):   # (NoNpDrm's newer work.bin is 1024 bytes: the licence first)
        return "bad", ""
    rif = data[:RIF.RIF_SIZE]
    try:
        info = RIF.parse_rif(rif)
    except ValueError:
        return "bad", ""
    if not _title_like(info.title_id):
        return "bad", ""
    if info.aid != FAKE_AID:
        return "console", info.content_id
    d = store_dir()
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, info.content_id + ".rif")
    if os.path.isfile(path) and open(path, "rb").read() == rif:
        return "known", info.content_id
    with open(path + ".tmp", "wb") as f:
        f.write(rif)
    os.replace(path + ".tmp", path)
    return "added", info.content_id


def find(content_id: str) -> str:
    p = os.path.join(store_dir(), content_id + ".rif")
    return p if content_id and os.path.isfile(p) else ""


def kept() -> list[str]:
    d = store_dir()
    return sorted(f[:-4] for f in os.listdir(d) if f.endswith(".rif")) if os.path.isdir(d) else []


def is_licence_name(name: str) -> bool:
    low = name.lower()
    return low == "work.bin" or low.endswith(".rif")


def import_paths(paths: list[str]) -> list[tuple[str, str, str]]:
    """(status, content id, file) for each licence file at the paths (folders searched, linked ones followed once)."""
    out, seen = [], set()
    for p in paths:
        files = []
        if os.path.isdir(p):
            for dp, dn, fn in os.walk(p, followlinks=True):
                real = os.path.realpath(dp)
                if real in seen:
                    dn[:] = []
                    continue
                seen.add(real)
                dn[:] = [d for d in dn if not d.startswith(".")]
                files += [os.path.join(dp, f) for f in fn if is_licence_name(f)]
        elif os.path.isfile(p):
            files.append(p)
        for f in files:
            try:
                status, cid = add_bytes(open(f, "rb").read(2 * RIF.RIF_SIZE + 1))
            except OSError:
                status, cid = "bad", ""
            out.append((status, cid, f))
    return out


def vita3k_dirs() -> list[str]:
    """Vita3K's ux0/license folders where it keeps them by default (SDL's pref path Vita3K/Vita3K; its fs/ subfolder
    when that has the ux0), and under a pref-path its config names."""
    home = os.path.expanduser("~")
    bases = [os.path.join(home, "Library", "Application Support", "Vita3K", "Vita3K"),
             os.path.join(os.environ.get("XDG_DATA_HOME", os.path.join(home, ".local", "share")), "Vita3K", "Vita3K")]
    if os.environ.get("APPDATA"):
        bases.append(os.path.join(os.environ["APPDATA"], "Vita3K", "Vita3K"))
    for cfg in (os.path.join(bases[0], "config.yml"), os.path.join(home, ".config", "Vita3K", "config.yml"),
                os.path.join(os.environ.get("APPDATA", ""), "Vita3K", "Vita3K", "config.yml")):
        try:
            for line in open(cfg, encoding="utf-8", errors="replace"):
                if line.startswith("pref-path:"):
                    v = line.split(":", 1)[1].strip().strip("'\"")
                    if v:
                        bases.append(v)
        except OSError:
            pass
    out = []
    for b in bases:
        for d in (os.path.join(b, "ux0", "license"), os.path.join(b, "fs", "ux0", "license")):
            if os.path.isdir(d) and d not in out:
                out.append(d)
    return out


def fetch_vita(address: str, timeout: float = 10.0, log=print) -> list[tuple[str, str, str]]:
    """The licences on a Vita, over VitaShell's FTP server (anonymous, port 1337, paths like /ux0:/app)."""
    host, _, port = address.partition(":")
    ftp = ftplib.FTP()
    ftp.connect(host, int(port or 1337), timeout=timeout)
    ftp.login()
    out = []

    def listing(path: str) -> list[tuple[str, bool]]:   # (name, is a folder)
        lines: list[str] = []
        try:
            ftp.cwd(path)
            ftp.retrlines("LIST", lines.append)
        except ftplib.all_errors:
            return []
        items = []
        for ln in lines:
            parts = ln.split(None, 8)
            if len(parts) == 9 and parts[8] not in (".", ".."):
                items.append((parts[8], ln[:1] == "d"))
        return items

    def fetch(path: str) -> None:
        buf = bytearray()
        try:
            ftp.retrbinary("RETR " + path, buf.extend)
        except ftplib.all_errors:
            return   # (no such file: a game installed some other way)
        status, cid = add_bytes(bytes(buf))
        out.append((status, cid, "vita:" + path))

    def walk_rifs(path: str, depth: int) -> None:   # NoNpDrm: license/app/<title>/*.rif, license/addcont/<title>/<add-on>/*.rif
        for name, is_dir in listing(path):
            p = path.rstrip("/") + "/" + name
            if is_dir and depth > 0:
                walk_rifs(p, depth - 1)
            elif not is_dir and name.lower().endswith(".rif"):
                fetch(p)

    walk_rifs("/ux0:/nonpdrm/license", 4)
    titles = [n for n, d in listing("/ux0:/app") if d]
    log(f"{len(titles)} games on the Vita")
    for t in titles:
        fetch(f"/ux0:/app/{t}/sce_sys/package/work.bin")
    for t in [n for n, d in listing("/ux0:/addcont") if d]:
        for a in [n for n, d in listing(f"/ux0:/addcont/{t}") if d]:
            fetch(f"/ux0:/addcont/{t}/{a}/sce_sys/package/work.bin")
    # a game with no licence Nativ can use: installed the usual way, its licence bound to the console -- named, with the cure
    have = {cid[7:16] for s, cid, _f in out if s in ("added", "known")} | {c[7:16] for c in kept()}
    for t in titles:   # (store games only, PCS..., as Vita3K tells them: homebrew carries no licence)
        if _title_like(t) and t.startswith("PCS") and t not in have:
            out.append(("missing", "", t))
    try:
        ftp.quit()
    except ftplib.all_errors:
        pass
    return out


def summary(results: list[tuple[str, str, str]]) -> str:
    n = {k: sum(1 for s, _c, _f in results if s == k) for k in ("added", "known", "console", "bad", "missing")}
    s = f"{n['added']} licences added, {n['known']} already kept -- {len(kept())} kept in all"
    if n["missing"]:
        names = ", ".join(f for s, _c, f in results if s == "missing")
        s += (f"; {n['missing']} games on the Vita have none Nativ can use ({names}): start each once with the NoNpDrm "
              "plugin on, then get licences again")
    if n["console"]:
        s += (f"; {n['console']} bound to the console (a PSN licence as the Vita keeps it): start those games once with "
              "the NoNpDrm plugin on, then get them again")
    return s


def selftest() -> None:
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        os.environ["ATIV_DUMP"] = tmp
        fake = RIF.build_rif("UP9000-PCSA00107_00-KZV0000000000000", bytes(range(16)))
        real = RIF.build_rif("UP9000-PCSA00029_00-UNCHARTED0000000", bytes(16), aid=0x1122334455667788)
        os.makedirs(os.path.join(tmp, "copy", "app", "PCSA00107", "sce_sys", "package"))
        with open(os.path.join(tmp, "copy", "app", "PCSA00107", "sce_sys", "package", "work.bin"), "wb") as f:
            f.write(fake + bytes(512))   # (a 1024-byte one)
        with open(os.path.join(tmp, "copy", "real.rif"), "wb") as f:
            f.write(real)
        with open(os.path.join(tmp, "copy", "notes.rif"), "wb") as f:
            f.write(b"not a licence")
        r = import_paths([os.path.join(tmp, "copy")])
        assert sorted(s for s, _c, _f in r) == ["added", "bad", "console"], r
        assert find("UP9000-PCSA00107_00-KZV0000000000000") and not find("UP9000-PCSA00029_00-UNCHARTED0000000")
        assert open(find("UP9000-PCSA00107_00-KZV0000000000000"), "rb").read() == fake
        dupe_path = os.path.join(tmp, "dupe.rif")
        with open(dupe_path, "wb") as f:
            f.write(fake)
        assert [s for s, _c, _f in import_paths([dupe_path])] == ["known"]
        assert kept() == ["UP9000-PCSA00107_00-KZV0000000000000"]
    print("licenses.py self-test: OK")


def main(argv: list[str]) -> int:
    if not argv:
        for cid in kept():
            print(cid, cid[7:16])
        print(f"{len(kept())} licences kept in {store_dir()}", file=sys.stderr)
        return 0
    cmd, args = argv[0], argv[1:]
    if cmd == "selftest":
        selftest()
        return 0
    if cmd == "find" and len(args) == 1:
        p = find(args[0])
        print(p)
        return 0 if p else 1
    if cmd == "import" and args:
        results = import_paths(args)
    elif cmd == "vita3k":
        dirs = args or vita3k_dirs()
        if not dirs:
            print("no Vita3K licences found (its ux0/license; name the folder: licenses.py vita3k <folder>)")
            return 1
        results = import_paths(dirs)
    elif cmd == "vita" and len(args) == 1:
        try:
            results = fetch_vita(args[0])
        except (OSError, ftplib.all_errors) as exc:
            print(f"can't reach the Vita at {args[0]} ({exc}): open VitaShell and press SELECT, then use the address it shows")
            return 1
    else:
        print(__doc__, file=sys.stderr)
        return 1
    for status, cid, src in results:
        if status in ("added", "console", "missing"):
            print(f"{status:8s} {cid or '-'}  ({src})")
    print(summary(results))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
