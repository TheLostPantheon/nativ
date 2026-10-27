"""One entry point: a PKG+licence file, a NoNpDrm-style folder, or an already-
decrypted folder -> a plain app0 tree plus a game.json.

This just wires the other modules together in the right order:

    PKG (+ licence file)       -- pkg.py -->  NoNpDrm-style folder (still PFS-encrypted)
    NoNpDrm-style folder       -- pfs.py -->  plain files (SELFs still SCE-encrypted)
    plain files                -- sce_self.decrypt_self() -->  every SELF -> a sibling .elf

A SELF is recognised by its own magic ("SCE\\0"), not by filename, so
eboot.bin, sce_module/*.suprx and anything else gets the same treatment; the
klicensee (needed for the PFS layer and for any NPDRM/APP-type SELF, but
*not* for a plain system .suprx) comes from --license (a .rif/work.bin file),
or is read automatically from sce_sys/package/work.bin if the input already
has one.
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import os
import shutil
import sys
import tempfile

from . import pfs as PFS
from . import pkg as PKG
from . import sce_self as SELF
from . import sfo as SFO
from . import rif as RIF

PKG_MAGIC_BYTES = b"\x7fPKG"
SCE_MAGIC_BYTES = b"SCE\x00"


class GameError(ValueError):
    pass


@dataclasses.dataclass
class GameInfo:
    title_id: str
    title: str
    version: str
    content_id: str
    category: str
    source: str              # "pkg" | "nonpdrm" | "decrypted"
    klicensee_source: str    # "license" | "work.bin (auto)" | "none"
    selfs_decrypted: int
    selfs_failed: int


def _detect_kind(path: str) -> str:
    if os.path.isdir(path):
        if os.path.exists(os.path.join(path, "sce_pfs", "files.db")):
            return "nonpdrm"
        return "decrypted"
    if os.path.isfile(path):
        with open(path, "rb") as f:
            magic = f.read(4)
        if magic == PKG_MAGIC_BYTES:
            return "pkg"
        raise GameError(f"{path} is a file but its magic ({magic!r}) is not a PKG's")
    raise GameError(f"{path} does not exist")


def _read_rif_file(path: str) -> bytes:
    with open(path, "rb") as f:
        data = f.read()
    if len(data) == RIF.RIF_SIZE:
        return data
    # NoNpDrm's 1024-byte work.bin (our test copy of Killzone): the 512-byte licence first, then a trailer
    if len(data) == 2 * RIF.RIF_SIZE:
        return data[:RIF.RIF_SIZE]
    raise GameError(f"{path} is not a {RIF.RIF_SIZE}-byte .rif/work.bin")


def _find_work_bin(root: str) -> str | None:
    p = os.path.join(root, "sce_sys", "package", "work.bin")
    return p if os.path.isfile(p) else None


def _copy_tree(src: str, dst: str) -> None:
    """An already-decrypted folder into out_dir: hard links where the filesystem allows (a copy the player keeps on the
    machine is not duplicated, gigabytes of it), copies otherwise. What is written into out_dir afterwards (the .elf
    files, game.json) is written as new files (_write_new), never through a link into the player's copy."""
    for dirpath, _dirnames, filenames in os.walk(src, followlinks=True):
        rel = os.path.relpath(dirpath, src)
        dest_dir = os.path.join(dst, rel) if rel != "." else dst
        os.makedirs(dest_dir, exist_ok=True)
        for name in filenames:
            s, d = os.path.join(dirpath, name), os.path.join(dest_dir, name)
            try:
                os.link(s, d)
            except OSError:
                shutil.copy2(s, d)


def _write_new(path: str, data: bytes) -> None:
    """Write `path` as a new file: if it is a hard link into the player's own copy (_copy_tree), replace the link
    rather than write through it."""
    if os.path.lexists(path):
        os.unlink(path)
    with open(path, "wb") as f:
        f.write(data)


def _decrypt_selfs_in_place(root: str, klicensee: bytes | None) -> tuple[int, int]:
    """Walk `root`; every file whose own first 4 bytes are the SCE magic gets
    decrypted to a `<name>.elf` sibling (fself2elf.py's own convention).
    Returns (decrypted, failed) -- a failure (e.g. no klicensee for an NPDRM
    self) is logged and skipped, not fatal to the rest of the tree."""
    ok = failed = 0
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            if name.endswith(".elf"):
                continue
            path = os.path.join(dirpath, name)
            with open(path, "rb") as f:
                head = f.read(4)
            if head != SCE_MAGIC_BYTES:
                continue
            with open(path, "rb") as f:
                data = f.read()
            try:
                elf = SELF.decrypt_self(data, klicensee, verify_digest=True)
            except SELF.SelfFormatError as exc:
                if os.path.relpath(path, root).split(os.sep)[0] == "sce_sys":
                    # not the game's code: sce_sys/about/right.suprx (the system's licence-text viewer) fails its
                    # digest in Killzone's 1.12 update and nothing loads it
                    print(f"note: {path}: {exc} (under sce_sys/, not the game's code: skipped)", file=sys.stderr)
                    continue
                print(f"warning: {path}: {exc}", file=sys.stderr)
                failed += 1
                continue
            _write_new(path + ".elf", elf)
            ok += 1
    return ok, failed


def convert_game(
    input_path: str, out_dir: str, *, license_path: str | None = None,
    license_out: str | None = None,
) -> GameInfo:
    """license_out: where to keep the licence that decrypted it (a 512-byte .rif) -- the game's update (ux0:patch) is
    decrypted with its game's licence and carries none of its own."""
    os.makedirs(out_dir, exist_ok=True)

    klicensee: bytes | None = None
    klicensee_source = "none"
    rif: bytes | None = None
    content_id_from_license = ""
    if license_path:
        rif = _read_rif_file(license_path)
        info = RIF.parse_rif(rif)
        klicensee, klicensee_source, content_id_from_license = info.klicensee, "license", info.content_id

    source = _detect_kind(input_path)
    cleanup_dir = None
    content_id_from_pkg = ""

    try:
        if source == "pkg":
            cleanup_dir = tempfile.mkdtemp(prefix="vitadecrypt_pkg_")
            pkg_info = PKG.extract_pkg(input_path, cleanup_dir, rif_path=license_path)
            content_id_from_pkg = pkg_info.content_id
            if klicensee is None:
                wb = _find_work_bin(cleanup_dir)
                if wb:
                    rif = _read_rif_file(wb)
                    info = RIF.parse_rif(rif)
                    klicensee, klicensee_source = info.klicensee, "work.bin (auto)"
            if klicensee is None:
                raise GameError("this PKG needs a klicensee -- pass --license")
            PFS.decrypt_pfs_tree(cleanup_dir, klicensee, out_dir)

        elif source == "nonpdrm":
            if klicensee is None:
                wb = _find_work_bin(input_path)
                if wb:
                    rif = _read_rif_file(wb)
                    info = RIF.parse_rif(rif)
                    klicensee, klicensee_source = info.klicensee, "work.bin (auto)"
            if klicensee is None:
                raise GameError(f"{input_path} is PFS-encrypted and needs a klicensee -- pass --license")
            PFS.decrypt_pfs_tree(input_path, klicensee, out_dir)

        else:  # "decrypted" -- no PFS layer; copy through, SELFs may still need klicensee
            if klicensee is None:
                wb = _find_work_bin(input_path)
                if wb:
                    rif = _read_rif_file(wb)
                    info = RIF.parse_rif(rif)
                    klicensee, klicensee_source = info.klicensee, "work.bin (auto)"
            _copy_tree(input_path, out_dir)
    finally:
        if cleanup_dir:
            shutil.rmtree(cleanup_dir, ignore_errors=True)

    n_ok, n_failed = _decrypt_selfs_in_place(out_dir, klicensee)
    if license_out and rif is not None:
        with open(license_out, "wb") as f:
            f.write(rif)

    sfo_path = os.path.join(out_dir, "sce_sys", "param.sfo")
    app_info = SFO.get_app_info(SFO.read_sfo_file(sfo_path)) if os.path.isfile(sfo_path) else SFO.AppInfo()
    content_id = app_info.content_id or content_id_from_pkg or content_id_from_license

    game = GameInfo(
        title_id=app_info.title_id or content_id[7:16],
        title=app_info.title,
        version=app_info.app_version,
        content_id=content_id,
        category=app_info.category,
        source=source,
        klicensee_source=klicensee_source,
        selfs_decrypted=n_ok,
        selfs_failed=n_failed,
    )
    _write_new(os.path.join(out_dir, "game.json"), (json.dumps(dataclasses.asdict(game), indent=2)).encode())
    return game


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="python3 -m vitadecrypt.game",
        description="PKG+licence file / NoNpDrm folder / already-decrypted folder -> a plain app0 tree + game.json.",
    )
    p.add_argument("input", nargs="?")
    p.add_argument("outdir", nargs="?")
    p.add_argument("--license", dest="license_path", help="a .rif/work.bin licence file")
    p.add_argument("--license-out", help="keep the licence used here (a .rif): its update decrypts with it")
    p.add_argument("--selftest", action="store_true")
    args = p.parse_args(argv)

    if args.selftest or not (args.input and args.outdir):
        _selftest()
        return 0

    game = convert_game(args.input, args.outdir, license_path=args.license_path, license_out=args.license_out)
    print(f"{game.source} {game.title_id} {game.content_id!r} {game.title!r} v{game.version} "
          f"(klicensee: {game.klicensee_source})")
    print(f"decrypted {game.selfs_decrypted} SELF(s), {game.selfs_failed} failed -> {args.outdir}/game.json")
    return 1 if game.selfs_failed else 0


def _selftest() -> None:
    from .tests.test_sce_self import _build_self, _pack_elf
    from . import keys as K

    klic = bytes.fromhex("00112233445566778899aabbccddeeff")[:16]
    sysver, key_revision = 0x36300000000, 5
    segments = [b"vitadecrypt game.py self-test eboot " * 8, b"segment two" + bytes(9)]
    self_bytes = _build_self(segments, [True, False], K.SelfType.APP, sysver, key_revision, klic)
    expected_elf, _ = _pack_elf(segments)

    content_id = "IP0177-PCSB00013_00-GOLFDS00000000"
    sfo_bytes = SFO.build_sfo({
        "APP_VER": "01.00", "CATEGORY": "gd", "TITLE_ID": "PCSB00013",
        "CONTENT_ID": content_id, "TITLE": "Self Test Golf",
    })
    license_rif = RIF.build_rif(content_id, klic)

    import tempfile as _tempfile
    with _tempfile.TemporaryDirectory() as tmp:
        license_path = os.path.join(tmp, "fake.rif")
        with open(license_path, "wb") as f:
            f.write(license_rif)
        # --- (a) the PKG entry point: PKG -> PFS -> SELF, end to end ---
        self_sigs_fn, self_box = _pfs_encrypt_member(self_bytes, klic)
        sfo_sigs_fn = _pfs_verify_only_member(sfo_bytes, klic)
        files_db = PFS._build_files_db(klic, 0x1000, version=4, image_spec=1, entries=[
            (0, "sce_sys", 1, PFS.FT_SYS_DIR, 0),
            (1, "param.sfo", 2, PFS.FT_UNENC_SYS_RO, len(sfo_bytes)),
            (0, "eboot.bin", 3, PFS.FT_NORMAL_FILE, len(self_bytes)),
        ])
        unicv_db = PFS._build_unicv_db([
            {"num_sectors": len(PFS._chunks(sfo_bytes, PFS.FILE_SECTOR_SIZE)), "version": 1, "dbseed": None, "make_sigs": sfo_sigs_fn},
            {"num_sectors": len(PFS._chunks(self_bytes, PFS.FILE_SECTOR_SIZE)), "version": 1, "dbseed": None, "make_sigs": self_sigs_fn},
        ])
        self_cipher = self_box["bytes"]
        pkg_path = os.path.join(tmp, "test.pkg")
        with open(pkg_path, "wb") as f:
            f.write(PKG._build_synthetic_pkg(sfo_bytes, {
                "sce_pfs/files.db": files_db,
                "sce_pfs/unicv.db": unicv_db,
                "sce_sys/param.sfo": sfo_bytes,
                "eboot.bin": self_cipher,
            }, key_type=2))

        out_a = os.path.join(tmp, "out_a")
        game_a = convert_game(pkg_path, out_a, license_path=license_path)
        assert game_a.source == "pkg"
        assert game_a.title_id == "PCSB00013", game_a.title_id
        assert game_a.content_id == content_id
        assert game_a.selfs_decrypted == 1 and game_a.selfs_failed == 0, game_a
        assert open(os.path.join(out_a, "eboot.bin.elf"), "rb").read() == expected_elf
        assert open(os.path.join(out_a, "sce_sys", "param.sfo"), "rb").read() == sfo_bytes
        assert json.load(open(os.path.join(out_a, "game.json")))["content_id"] == content_id

        # --- (b) a NoNpDrm-style folder, with klicensee auto-discovered from
        # its own sce_sys/package/work.bin (no --license passed) ---
        nonpdrm_root = os.path.join(tmp, "nonpdrm")
        os.makedirs(os.path.join(nonpdrm_root, "sce_pfs"))
        with open(os.path.join(nonpdrm_root, "sce_pfs", "files.db"), "wb") as f:
            f.write(files_db)
        with open(os.path.join(nonpdrm_root, "sce_pfs", "unicv.db"), "wb") as f:
            f.write(unicv_db)
        os.makedirs(os.path.join(nonpdrm_root, "sce_sys", "package"))
        with open(os.path.join(nonpdrm_root, "sce_sys", "param.sfo"), "wb") as f:
            f.write(sfo_bytes)
        with open(os.path.join(nonpdrm_root, "sce_sys", "package", "work.bin"), "wb") as f:
            f.write(RIF.build_rif(content_id, klic))
        with open(os.path.join(nonpdrm_root, "eboot.bin"), "wb") as f:
            f.write(self_cipher)

        out_b = os.path.join(tmp, "out_b")
        game_b = convert_game(nonpdrm_root, out_b)
        assert game_b.source == "nonpdrm"
        assert game_b.klicensee_source == "work.bin (auto)", game_b.klicensee_source
        assert open(os.path.join(out_b, "eboot.bin.elf"), "rb").read() == expected_elf

        # --- (c) an already-decrypted folder (no PFS layer), klicensee via --license ---
        decrypted_root = os.path.join(tmp, "decrypted")
        os.makedirs(os.path.join(decrypted_root, "sce_sys"))
        with open(os.path.join(decrypted_root, "sce_sys", "param.sfo"), "wb") as f:
            f.write(sfo_bytes)
        with open(os.path.join(decrypted_root, "eboot.bin"), "wb") as f:
            f.write(self_bytes)  # not PFS-wrapped here -- this folder has no sce_pfs/

        out_c = os.path.join(tmp, "out_c")
        game_c = convert_game(decrypted_root, out_c, license_path=license_path)
        assert game_c.source == "decrypted"
        assert game_c.klicensee_source == "license"
        assert open(os.path.join(out_c, "eboot.bin.elf"), "rb").read() == expected_elf

        # --- (d) no klicensee available at all for PFS-encrypted content -> must fail closed ---
        no_license_root = os.path.join(tmp, "no_license")
        shutil.copytree(nonpdrm_root, no_license_root)
        os.remove(os.path.join(no_license_root, "sce_sys", "package", "work.bin"))
        try:
            convert_game(no_license_root, os.path.join(tmp, "out_d"))
            raise AssertionError("expected GameError with no klicensee available")
        except GameError:
            pass

    print("game.py self-test: OK")


def _pfs_encrypt_member(plain: bytes, klicensee: bytes, files_salt: int = 0x1000):
    """Return (make_sigs, box) for a PFS-encrypted member -- the inverse of
    pfs.decrypt_pfs_file's decrypt path, built the same way pfs.py's own
    self-test does (see its module docstring for why reusing these helpers
    is not circular for what this test actually checks). `make_sigs` has to
    run (inside _build_unicv_db, which knows the real icv_salt) before the
    ciphertext exists; it lands in `box["bytes"]` once that happens."""
    box: dict[str, bytes] = {}

    def make_sigs(icv_salt: int):
        dec_key, tweak_enc_key = PFS.derive_file_keys(klicensee, files_salt, icv_salt, None)
        drv_key = PFS.f00d_unwrap(dec_key)
        cipher = bytearray()
        for i, chunk in enumerate(PFS._chunks(plain, PFS.FILE_SECTOR_SIZE)):
            tweak = bytes(a ^ b for a, b in zip((PFS.FILE_SECTOR_SIZE * i).to_bytes(8, "little") + bytes(8), tweak_enc_key))
            cipher += PFS._cbc_cts(drv_key, tweak, chunk, encrypt=True)
        box["bytes"] = bytes(cipher)
        return PFS._sector_sigs(PFS.derive_secret(klicensee, files_salt, icv_salt), PFS._chunks(bytes(cipher), PFS.FILE_SECTOR_SIZE))

    return make_sigs, box


def _pfs_verify_only_member(plain: bytes, klicensee: bytes, files_salt: int = 0x1000):
    def make_sigs(icv_salt: int):
        return PFS._sector_sigs(PFS.derive_secret(klicensee, files_salt, icv_salt), PFS._chunks(plain, PFS.FILE_SECTOR_SIZE))
    return make_sigs


if __name__ == "__main__":
    raise SystemExit(main())
