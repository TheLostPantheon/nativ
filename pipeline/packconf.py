"""packconf.py: a game pack (packs/<title>/pack.toml) read the same way on every platform (Python's tomllib).

A pack is TOML. The game's settings come first, as top-level KEY = "value" (or a number): what vitarecomp reads, over
DEFAULTS below. Then:
  [versions."<version>"]   one per build of the game the pack plays, in the order a player would meet them: sha256 (of
                           its eboot.bin.elf: vitarecomp import tells a copy's version by it), status (recommended: the
                           one to play; supported: plays) and the settings that differ in that build (HOOKS, IHOOKS,
                           GAME_ADDRS, FUNCTION_SEEDS...). A copy with no version recorded reads the first one's.
  [when.<NAME>]            settings for when the environment has NAME set (not empty), after the version's:
                           KEY = "..." replaces, append.KEY = "..." adds words to it.
VERSIONS, the "<version>=<sha256>:<status> ..." list the rest of the pipeline reads, is made from the version tables.
`packconf.py --check` reads every pack, version by version, with and without each switch, and checks its shape;
`packconf.py <pack.toml> [version]` prints the settings, NAME=value a line.
"""
import os
import re
import sys
import tomllib

# vitarecomp's defaults, before a pack is read (load_conf)
DEFAULTS = {
    "BASE": "game", "SPLIT": "16", "TEST_INPUT": "", "TEST_TOUCH": "", "TEST_ENV": "", "TEST_FRAMES": "600",
    "TEST_TOLERANCE": "1", "ELF": "eboot.bin.elf", "HOOKS": "", "IHOOKS": "", "MOD_HOOKS": "", "MOD_IHOOKS": "",
    "GAME_ADDRS": "", "TITLE_ID": "", "VERSIONS": "", "UPDATE_NOTE": "", "FUNCTION_SEEDS": "",
}
_KEY = re.compile(r"[A-Z][A-Z0-9_]*\Z")
STATUSES = ("recommended", "supported")


class PackError(Exception):
    pass


def _value(path, key, val):
    if isinstance(val, bool) or not isinstance(val, (str, int, float)):
        raise PackError(f"{path}: {key} is neither text nor a number: {val!r}")
    if not _KEY.match(key): raise PackError(f"{path}: {key}: a setting's name is capitals, digits and _")
    return str(val)


def read(path):
    """the pack's TOML as written: (settings, versions, switches)"""
    try:
        with open(path, "rb") as f: t = tomllib.load(f)
    except tomllib.TOMLDecodeError as e:
        raise PackError(f"{path}: {e}")
    versions, when = t.pop("versions", {}), t.pop("when", {})
    settings = {k: _value(path, k, val) for k, val in t.items()}
    for name, x in versions.items():
        if not isinstance(x, dict): raise PackError(f"{path}: versions.{name} isn't a table")
        if not re.fullmatch(r"[0-9a-f]{64}", str(x.get("sha256", ""))): raise PackError(f"{path}: version {name}: sha256 isn't 64 hex digits")
        if x.get("status", "supported") not in STATUSES: raise PackError(f"{path}: version {name}: status is one of {', '.join(STATUSES)}")
        for k, val in x.items():
            if k not in ("sha256", "status"): _value(path, k, val)
    for name, w in when.items():
        if not isinstance(w, dict) or not _KEY.match(name): raise PackError(f"{path}: [when.{name}]: an environment variable's table")
        for k, val in w.items():
            if k == "append":
                if not isinstance(val, dict): raise PackError(f"{path}: when.{name}.append: KEY = \"words\" lines")
                for k2, add in val.items(): _value(path, k2, add)
            else: _value(path, k, val)
    return settings, versions, when


def load(path, version="", env=None):
    """The pack's settings (vitarecomp's defaults first) for this copy's VERSION; env: the environment its [when.X]
    tables read"""
    env = os.environ if env is None else env
    v = dict(DEFAULTS); v["VERSION"] = version
    if not os.path.isfile(path): return v
    settings, versions, when = read(path)
    v.update(settings)
    v["VERSIONS"] = " ".join(f"{name}={x['sha256']}:{x.get('status', 'supported')}" for name, x in versions.items())
    if versions:
        pick = versions.get(version) or next(iter(versions.values()))
        v.update({k: str(val) for k, val in pick.items() if k not in ("sha256", "status")})
    for name, w in when.items():
        if not env.get(name, ""): continue
        for k, val in w.items():
            if k == "append": v.update({k2: " ".join(x for x in (v.get(k2, ""), str(add)) if x) for k2, add in val.items()})
            else: v[k] = str(val)
    return v


def _check():
    """every pack: its shape, then every version it lists (and none), with and without each switch it has"""
    import glob
    here = os.path.dirname(os.path.abspath(__file__)); bad = 0
    for path in sorted(glob.glob(os.path.join(here, "..", "packs", "*", "pack.toml"))):
        title = os.path.basename(os.path.dirname(path))
        try:
            settings, versions, when = read(path)
            if not settings.get("NAME"): raise PackError(f"{path}: no NAME")
            if not versions: raise PackError(f"{path}: no [versions.\"...\"] table")
            for ver in [""] + list(versions):
                for on in ([False, True] if when else [False]):
                    v = load(path, ver, {s: "1" for s in when} if on else {})
                    print(f"ok     {title} version {ver or '-'}" + (f" with {','.join(when)}" if on else "") + f" ({len(v)} settings)")
        except PackError as e:
            bad += 1; print(f"BAD    {title}: {e}")
    return bad


if __name__ == "__main__":
    if sys.argv[1:] == ["--check"]: sys.exit(1 if _check() else 0)
    if len(sys.argv) >= 2:
        for k, val in load(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "").items(): print(f"{k}={val}")
        sys.exit(0)
    print(__doc__); sys.exit(1)
