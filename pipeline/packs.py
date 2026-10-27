#!/usr/bin/env python3
"""A player's copies of their games, as vitarecomp and the launcher (launcher/nativ) see them.

  packs.py scan <folder> [--packs packs/] [--dumps dump/decrypted] [--firmware dump/firmware] [--import-licences]
      every game, update, add-on and firmware file under <folder> (any depth: a copied ux0:, a downloads folder, a
      folder of links to copies kept elsewhere -- linked folders are followed), one tab-separated line each:
          kind  form  title_id  version  status  licence  name  path  licence_file  content_id  note
      kind: game | update | addon | firmware;  form: folder | pkg | pup
      status: recommended | supported (a version its game pack plays) | check (the pack has builds with no version
          number recorded: its hash decides when added) | unsupported (a version the pack doesn't know) | nopack
          (no game pack for this title yet) | otherregion (a game Nativ has a pack for, in another region's release:
          the pack plays its region's, by hash) | added (already in the library, this version) | licensed (an add-on:
          the host licenses every add-on, nothing to add) | installed / firmware (a firmware file: that version is
          installed / is not)
      licence: yes (its own sce_sys/package/work.bin) | file (one the player's licences hold -- licenses.py's store
          -- or one elsewhere in the folder, ux0:nonpdrm's .rif or a work.bin kept apart, whose content id is this
          copy's: licence_file) | game (an update: it decrypts with its game's licence, which the library or this
          folder has) | rif (a PKG: needs its licence) | no
      note: for a copy Nativ can't play as it is, which one it plays ("EU release: Nativ plays the US release, PCSA00029,
          1.00"; "Nativ plays 1.00")
      --import-licences: every licence found in the folder is kept in the player's licences first (licenses.py),
          the launcher's search: one import, and each later PKG, update or add-on finds its own
  packs.py info <path>
      one copy's "kind title_id version content_id" (vitarecomp add: the kept licence for it, an update its game's)
  packs.py selftest
  packs.py layer <app0> <patch0> <out>
      the tree a patched game runs from: app0 with its update's files over it, as hard links (copies where the
      filesystem has none). <out> is generated and replaced whole.

Nothing here decrypts: a game folder's sce_sys/param.sfo is plain, a PKG's identity reads from its header with the
retail outer key (vitadecrypt.pkg.peek_pkg), a PUP's version from its header.
"""
from __future__ import annotations

import os
import shutil
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "decrypt"))
from vitadecrypt import pkg as PKG  # noqa: E402
from vitadecrypt import sfo as SFO  # noqa: E402
from vitadecrypt import rif as RIF  # noqa: E402
import licenses as LIC  # noqa: E402
from winfs import remove_file, remove_tree  # noqa: E402
from datadir import library_dir  # noqa: E402
import packconf  # noqa: E402

KINDS = {"gd": "game", "gp": "update", "ac": "addon"}   # param.sfo CATEGORY
PKG_KINDS = {"app": "game", "patch": "update", "dlc": "addon"}
MAX_DEPTH = 8


def pack_versions(pack: str) -> list[tuple[str, str, str]] | None:
    """A game pack's versions (its [versions."..."] tables, packconf.py) as (version, sha256, status); None without a pack."""
    if not os.path.isfile(pack):
        return None
    out = []
    for e in packconf.load(pack, "", {})["VERSIONS"].split():
        v, _, rest = e.partition("=")
        h, _, st = rest.partition(":")
        out.append((v, h, st or "supported"))
    return out


# a release's region, from its content id's first letter (UP9000-..., EP9000-...), else its title id's prefix
REGION_BY_CID = {"U": "US", "E": "EU", "J": "JP", "H": "Asia", "K": "Korea"}
REGION_BY_TITLE = {"PCSA": "US", "PCSE": "US", "PCSF": "EU", "PCSB": "EU", "PCSG": "JP", "PCSC": "JP", "PCSH": "Asia", "PCSD": "Asia"}


def region_of(content_id: str, title_id: str) -> str:
    return REGION_BY_CID.get(content_id[:1], "") or REGION_BY_TITLE.get(title_id[:4], "")


def _plain(name: str) -> str:   # "Killzone™ Mercenary" and "Killzone: Mercenary" alike
    return " ".join("".join(c.lower() if c.isalnum() else " " for c in name).split())


def pack_index(packs: str) -> list[dict]:
    """Every game pack: its title id, NAME, REGION, VERSIONS, OTHER_TITLES (other releases' title ids known to be it)."""
    out = []
    for fn in sorted(os.listdir(packs)) if os.path.isdir(packs) else []:
        pack = os.path.join(packs, fn, "pack.toml")
        if len(fn) != 9 or not os.path.isfile(pack):
            continue
        c = packconf.load(pack, "", {})
        info = {"id": fn, "name": c.get("NAME", ""), "region": c.get("REGION", ""), "others": c.get("OTHER_TITLES", "").split()}
        info["versions"] = [v for v, _h, _s in (pack_versions(pack) or []) if v != "-"]
        out.append(info)
    return out


def other_release(title: str, name: str, index: list[dict]) -> dict | None:
    """The pack whose game this copy is, in another release: listed among its OTHER_TITLES, or the same name (a
    localised one isn't caught -- list its title id)."""
    for p in index:
        if title in p["others"]:
            return p
    n = _plain(name)
    for p in index:
        pn = _plain(p["name"])
        if pn and n and (n == pn or n.startswith(pn + " ") or pn.startswith(n + " ")):
            return p
    return None


def release_note(title: str, content_id: str, name: str, version: str, status: str, packs: str, index=None) -> str:
    """What to tell a player of a copy Nativ can't play as it is: which release and version it does play."""
    index = index if index is not None else pack_index(packs)
    if status == "unsupported":
        p = next((p for p in index if p["id"] == title), None)
        return f"Nativ plays {', '.join(p['versions'])}" if p and p["versions"] else ""
    if status in ("nopack", "otherregion"):
        p = other_release(title, name, index)
        if p:
            mine = region_of(content_id, title) or title
            theirs = p["region"] or region_of("", p["id"]) or p["id"]
            vs = f", {', '.join(p['versions'])}" if p["versions"] else ""
            return f"{mine} release: Nativ plays the {theirs} release ({p['id']}{vs})"
    return ""


def folder_identity(path: str) -> tuple[str, str, str, str, str] | None:
    """A copied title folder's (kind, title_id, version, name, content_id) from its plain sce_sys/param.sfo."""
    sfo_path = os.path.join(path, "sce_sys", "param.sfo")
    if not os.path.isfile(sfo_path):
        return None
    info = SFO.get_app_info(SFO.read_sfo_file(sfo_path))
    return KINDS.get(info.category, info.category or "game"), info.title_id, info.app_version, info.title, info.content_id


def pkg_identity(path: str) -> tuple[str, str, str, str, str]:
    info = PKG.peek_pkg(path)
    return PKG_KINDS.get(info.kind, info.kind), info.title_id, info.app_version, info.title, info.content_id


def licence_content_id(path: str) -> str:
    """A licence file's content id (a 512-byte .rif / work.bin, or NoNpDrm's 1024-byte one); "" if it isn't one."""
    try:
        if os.path.getsize(path) not in (RIF.RIF_SIZE, 2 * RIF.RIF_SIZE):
            return ""
        with open(path, "rb") as f:
            return RIF.parse_rif(f.read(RIF.RIF_SIZE)).content_id
    except (OSError, ValueError):
        return ""


def pup_version(path: str) -> str:
    """A firmware update file's version from its SCEUF header (0x03740011 -> "3.74")."""
    with open(path, "rb") as f:
        head = f.read(0x20)
    if head[:5] != b"SCEUF":
        raise ValueError("not a PUP")
    v, = struct.unpack_from("<I", head, 0x10)
    return f"{(v >> 24) & 0xff:x}.{(v >> 16) & 0xff:02x}"


def identity(path: str) -> tuple[str, str, str, str]:
    if os.path.isdir(path):
        r = folder_identity(path)
        if r is None:
            raise ValueError(f"{path}: no sce_sys/param.sfo -- not a game, update or add-on folder")
        return r
    with open(path, "rb") as f:
        magic = f.read(5)
    if magic[:4] == b"\x7fPKG":
        return pkg_identity(path)
    if magic == b"SCEUF":
        return "firmware", "", pup_version(path), "PS Vita system software", ""
    raise ValueError(f"{path}: neither a game folder, a PKG nor a firmware file")


def _library_version(dumps: str, title: str, kind: str) -> str:
    """The version the library holds of a title's game (app0) or update (patch0): vitadecrypt's game.json there."""
    import json
    p = os.path.join(dumps, title, "app0" if kind == "game" else "patch0", "game.json")
    try:
        return json.load(open(p)).get("version", "")
    except (OSError, ValueError):
        return ""


def _status(kind: str, title: str, version: str, packs: str, dumps: str) -> str:
    if kind == "addon":
        return "licensed"
    if _library_version(dumps, title, kind) == version and version:
        return "added"
    vs = pack_versions(os.path.join(packs, title, "pack.toml"))
    if vs is None:
        return "nopack"
    for v, _h, st in vs:
        if v == version:
            return st
    if any(v == "-" for v, _h, _st in vs) or not vs:
        return "check"
    return "unsupported"


def scan(root: str, packs: str, dumps: str, firmware: str, import_licences: bool = False) -> list[list[str]]:
    rows: list[list[str]] = []
    found: list[tuple[str, str, str, str, str, str, str]] = []   # kind, form, title, version, name, path, content id
    licences: dict[str, str] = {}   # content id -> a licence file found outside its game's folder
    root = os.path.abspath(root)
    base_depth = root.rstrip(os.sep).count(os.sep)
    seen = set()   # folders by their real path: linked folders are followed, once
    for dirpath, dirnames, filenames in os.walk(root, followlinks=True):
        real = os.path.realpath(dirpath)
        if real in seen:
            dirnames[:] = []
            continue
        seen.add(real)
        dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
        if dirpath.count(os.sep) - base_depth >= MAX_DEPTH:
            dirnames[:] = []
        ident = folder_identity(dirpath) if "sce_sys" in dirnames else None
        if ident:
            kind, title, version, name, cid = ident
            found.append((kind, "folder", title, version, name, dirpath, cid))
            dirnames[:] = []   # a title folder's insides are its own files
            continue
        for fn in sorted(filenames):
            p = os.path.join(dirpath, fn)
            low = fn.lower()
            try:
                if low.endswith(".pkg"):
                    kind, title, version, name, cid = pkg_identity(p)
                    found.append((kind, "pkg", title, version, name, p, cid))
                elif low.endswith(".pup"):
                    found.append(("firmware", "pup", "", pup_version(p), "PS Vita system software", p, ""))
                elif low == "work.bin" or low.endswith(".rif"):
                    cid = licence_content_id(p)
                    if cid:
                        licences.setdefault(cid, p)
            except Exception as exc:   # a broken or foreign file: say so, keep scanning
                print(f"{p}: {exc}", file=sys.stderr)
    def own_licence(kind: str, form: str, path: str) -> bool:
        return form == "folder" and kind == "game" and os.path.isfile(os.path.join(path, "sce_sys", "package", "work.bin"))
    if import_licences:   # the player's own licences, kept: this search is their one import (a copied game folder's own too)
        own = [os.path.join(p, "sce_sys", "package", "work.bin") for _k, f, _t, _v, _n, p, _c in found if f == "folder"]
        res = LIC.import_paths(list(licences.values()) + [o for o in own if os.path.isfile(o)])
        print("licences: " + LIC.summary(res), file=sys.stderr)
    # titles whose game, with a licence, is in this scan: their updates can be decrypted once it is added
    games_here = {t for k, f, t, _v, _n, p, cid in found if k == "game" and (own_licence(k, f, p) or cid in licences)}
    index = pack_index(packs)
    for kind, form, title, version, name, path, cid in found:
        lic_file = ""
        if kind == "firmware":
            status = "installed" if os.path.isdir(os.path.join(firmware, version)) else "firmware"
            licence = "yes"
        else:
            status = _status(kind, title, version, packs, dumps)
            if status == "nopack" and kind != "addon" and other_release(title, name, index):
                status = "otherregion"
            has_lib_licence = os.path.isfile(os.path.join(dumps, title, "license.rif"))
            if own_licence(kind, form, path):
                licence = "yes"
            elif cid and LIC.find(cid):   # (kept earlier: an update's content id is its game's)
                licence, lic_file = "file", LIC.find(cid)
            elif cid and cid in licences:
                licence, lic_file = "file", licences[cid]
            elif kind == "update" and (title in games_here or has_lib_licence):
                licence = "game"
            else:
                licence = "rif" if form == "pkg" else "no"
        note = release_note(title, cid, name, version, status, packs, index) if kind != "firmware" else ""
        rows.append([kind, form, title, version, status, licence, name.replace("\t", " ").replace("\n", " "), path, lic_file, cid, note])
    order = {"firmware": 0, "game": 1, "update": 2, "addon": 3}
    rows.sort(key=lambda r: (order.get(r[0], 9), r[6].lower(), r[3]))
    return rows


def layer(app0: str, patch0: str, out: str) -> tuple[int, int]:
    """out = app0 with patch0's files over it. Hard links where the filesystem allows (no second copy of a game's
    gigabytes), copies otherwise. Returns (linked, copied)."""
    tmp = out + ".new"
    remove_tree(tmp)
    linked = copied = 0
    for src_root in (app0, patch0):
        for dirpath, _dirnames, filenames in os.walk(src_root):
            rel = os.path.relpath(dirpath, src_root)
            dst_dir = os.path.join(tmp, rel) if rel != "." else tmp
            os.makedirs(dst_dir, exist_ok=True)
            for fn in filenames:
                s, d = os.path.join(dirpath, fn), os.path.join(dst_dir, fn)
                if os.path.lexists(d):
                    remove_file(d)   # the update's file replaces the game's (winfs: a read-only game file's link too, on Windows)
                try:
                    os.link(s, d)
                    linked += 1
                except OSError:
                    shutil.copy2(s, d)
                    copied += 1
    remove_tree(out)
    os.rename(tmp, out)
    return linked, copied


def selftest() -> None:
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        def put(rel: str, data: bytes) -> str:
            path = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
            return path
        def sfo(cat: str, ver: str, title: str = "Self Test Golf") -> bytes:
            return SFO.build_sfo({"APP_VER": ver, "CATEGORY": cat, "TITLE_ID": "PCSB00013", "TITLE": title,
                                  "CONTENT_ID": "IP0177-PCSB00013_00-GOLFDS00000000"})
        put("ux0/app/PCSB00013/sce_sys/param.sfo", sfo("gd", "01.00"))
        put("ux0/app/PCSB00013/sce_sys/package/work.bin", bytes(512))
        # a second game, kept elsewhere and linked in, its licence in ux0:license
        put("elsewhere/PCSB00099/sce_sys/param.sfo", SFO.build_sfo({"APP_VER": "01.00", "CATEGORY": "gd", "TITLE_ID": "PCSB00099",
                                                                     "TITLE": "Linked", "CONTENT_ID": "IP0177-PCSB00099_00-LINKED0000000000"}))
        os.symlink(os.path.join(tmp, "elsewhere", "PCSB00099"), os.path.join(tmp, "ux0", "app", "PCSB00099"))
        put("ux0/license/app/PCSB00099/a.rif", RIF.build_rif("IP0177-PCSB00099_00-LINKED0000000000", bytes(16)) + bytes(512))
        put("ux0/app/PCSB00013/eboot.bin", b"SCE\0")   # (a title folder's insides aren't searched)
        put("ux0/patch/PCSB00013/sce_sys/param.sfo", sfo("gp", "01.01"))
        put("ux0/addcont/PCSB00013/P0000000000001/sce_sys/param.sfo", sfo("ac", "", "Golf Clubs"))
        put("dl/PSP2UPDAT.PUP", b"SCEUF" + bytes(11) + struct.pack("<I", 0x03740011) + bytes(12))
        put("dl/patch.pkg", PKG._build_synthetic_pkg(sfo("gp", "01.02"), {"eboot.bin": b"x"}, key_type=3))
        put("packs/PCSB00013/pack.toml", f'NAME = "Golf"\n[versions."1.00"]\nsha256 = "{"a" * 64}"\n[versions."1.01"]\nsha256 = "{"b" * 64}"\nstatus = "recommended"\n'.encode())
        put("packs/PCSA00029/pack.toml", f'NAME = "Uncharted: Golden Abyss"\nREGION = "US"\nOTHER_TITLES = "PCSF00001"\n[versions."1.00"]\nsha256 = "{"c" * 64}"\n'.encode())
        put("eu/a/sce_sys/param.sfo", SFO.build_sfo({"APP_VER": "01.00", "CATEGORY": "gd", "TITLE_ID": "PCSF00001", "TITLE": "Uncharted: Golden Abyss\u2122",
                                                     "CONTENT_ID": "EP9000-PCSF00012_00-0000000000000000"}))
        put("eu/b/sce_sys/param.sfo", SFO.build_sfo({"APP_VER": "01.01", "CATEGORY": "gd", "TITLE_ID": "PCSG00099", "TITLE": "Golf",
                                                     "CONTENT_ID": "JP0000-PCSG00099_00-0000000000000000"}))
        eu = scan(os.path.join(tmp, "eu"), os.path.join(tmp, "packs"), os.path.join(tmp, "dumps"), os.path.join(tmp, "firmware"))
        assert [(r[2], r[4], r[10]) for r in eu] == [
            ("PCSG00099", "otherregion", "JP release: Nativ plays the EU release (PCSB00013, 1.00, 1.01)"),
            ("PCSF00001", "otherregion", "EU release: Nativ plays the US release (PCSA00029, 1.00)")], eu
        shutil.rmtree(os.path.join(tmp, "eu"))
        os.makedirs(os.path.join(tmp, "firmware", "3.74"))
        rows = scan(os.path.join(tmp, "ux0"), os.path.join(tmp, "packs"), os.path.join(tmp, "dumps"), os.path.join(tmp, "firmware"))
        assert [(r[2], r[5], os.path.basename(r[8])) for r in rows if r[2] == "PCSB00099"] == [("PCSB00099", "file", "a.rif")], rows
        rows = [r for r in scan(os.path.join(tmp), os.path.join(tmp, "packs"), os.path.join(tmp, "dumps"), os.path.join(tmp, "firmware"))
                if r[2] != "PCSB00099"]
        got = [(r[0], r[1], r[3], r[4], r[5]) for r in rows]
        want = [("firmware", "pup", "3.74", "installed", "yes"), ("game", "folder", "1.00", "supported", "yes"),
                ("update", "folder", "1.01", "recommended", "game"), ("update", "pkg", "1.02", "unsupported", "game"),
                ("addon", "folder", "", "licensed", "no")]
        assert got == want, got
        assert identity(os.path.join(tmp, "ux0", "patch", "PCSB00013"))[:3] == ("update", "PCSB00013", "1.01")
        # layering: the update's files over the game's, the rest hard links to the game's own
        app0, patch0, out = (os.path.join(tmp, d) for d in ("app0", "patch0", "out"))
        put("app0/a", b"1"); put("app0/d/b", b"2"); put("patch0/a", b"X"); put("patch0/c", b"3")
        layer(app0, patch0, out)
        assert {os.path.relpath(os.path.join(dp, f), out): open(os.path.join(dp, f), "rb").read()
                for dp, _dn, fs in os.walk(out) for f in fs} == {"a": b"X", os.path.join("d", "b"): b"2", "c": b"3"}
        assert os.stat(os.path.join(out, "d", "b")).st_ino == os.stat(os.path.join(app0, "d", "b")).st_ino
        assert open(os.path.join(app0, "a"), "rb").read() == b"1"   # (the game's own file untouched)
    print("packs.py self-test: OK")


def main(argv: list[str]) -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    dump = library_dir(root)
    if len(argv) >= 2 and argv[0] == "scan":
        args = argv[2:]
        opt = {k: v for k, v in zip(args[::2], args[1::2])}
        imp = "--import-licences" in args
        args = [a for a in args if a != "--import-licences"]
        opt = {k: v for k, v in zip(args[::2], args[1::2])}
        for row in scan(argv[1], opt.get("--packs", os.path.join(root, "packs")), opt.get("--dumps", os.path.join(dump, "decrypted")),
                        opt.get("--firmware", os.path.join(dump, "firmware")), imp):
            print("\t".join(row))
        return 0
    if len(argv) == 2 and argv[0] == "note":   # (vitarecomp add, a copy with no pack: which release Nativ plays)
        kind, title, version, name, cid = identity(argv[1])
        print(release_note(title, cid, name, version, "nopack", os.path.join(root, "packs")))
        return 0
    if len(argv) == 2 and argv[0] == "info":
        kind, title, version, _name, cid = identity(argv[1])
        print(kind, title or "-", version or "-", cid or "-")
        return 0
    if argv[:1] == ["selftest"]:
        selftest()
        return 0
    if len(argv) == 4 and argv[0] == "layer":
        linked, copied = layer(*argv[1:])
        print(f"{argv[3]}: {linked} files linked, {copied} copied")
        return 0
    print(__doc__, file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
