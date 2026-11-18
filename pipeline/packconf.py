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
FEATURES says what the pack gives the game beyond playing it, each feature verified or name:experimental (FEATURE_NAMES
below). --check holds each against the settings that make it (widescreen needs RECOMP_ASPECT_HOOK, and so on), so the
list can't claim what the pack doesn't do or leave out what it does; --table makes the README's games table from it.
`packconf.py --check` reads every pack, version by version, with and without each switch, and checks its shape;
`packconf.py --table [--check|--write] <README.md>` prints, checks or rewrites the README's games table;
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
    "GAME_ADDRS": "", "TITLE_ID": "", "VERSIONS": "", "UPDATE_NOTE": "", "FUNCTION_SEEDS": "", "ONLINE_VERSIONS": "",
    "TIER": "", "GENERATED": "", "FEATURES": "", "LANGUAGES": "",
}
# FEATURES: the names a pack may list, the README table's column for each, and what in the pack's settings makes the
# feature (None: nothing to check it against)
FEATURE_NAMES = {
    "widescreen": ("Widescreen", lambda s: "RECOMP_ASPECT_HOOK=1" in s["RUN_ENV"].split()),
    "fov": ("Field of view", lambda s: "fov" in _options(s)),
    "mouse_aim": ("1:1 mouse aim", None),
    "fsr": ("FSR 3.1", lambda s: "RECOMP_FSR_SUPPORTED=1" in s["RUN_ENV"].split()),
    "online": ("Online play", lambda s: "online" in _options(s)),
}
FEATURE_STATUSES = ("verified", "experimental")


def _options(s):   # RECOMP_GAME_OPTIONS's list (the game's own menu options)
    for w in s.get("RUN_ENV", "").split():
        if w.startswith("RECOMP_GAME_OPTIONS="): return w.split("=", 1)[1].split(",")
    return []


def features(s):
    """FEATURES as {name: status}"""
    out = {}
    for w in s.get("FEATURES", "").split():
        name, _, status = w.partition(":")
        out[name] = status or "verified"
    return out
# what an "unknown" copy (a version the pack doesn't play, tried anyway: vitarecomp import --experimental) goes without:
# everything made for particular builds' addresses, and the switches in RUN_ENV that only those hooks honour
BUILD_SPECIFIC = ("HOOKS", "IHOOKS", "GAME_ADDRS", "FUNCTION_SEEDS")
HOOKED_ENV = ("RECOMP_GAME_OPTIONS", "RECOMP_ASPECT_HOOK")
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
    if versions and version != "unknown":
        pick = versions.get(version) or next(iter(versions.values()))
        v.update({k: str(val) for k, val in pick.items() if k not in ("sha256", "status")})
    for name, w in when.items():
        if not env.get(name, ""): continue
        for k, val in w.items():
            if k == "append": v.update({k2: " ".join(x for x in (v.get(k2, ""), str(add)) if x) for k2, add in val.items()})
            else: v[k] = str(val)
    if version == "unknown":
        v.update({k: "" for k in BUILD_SPECIFIC})
        v["RUN_ENV"] = " ".join(e for e in v.get("RUN_ENV", "").split() if e.split("=")[0] not in HOOKED_ENV)
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
            v = load(path, "")
            for name, status in features(v).items():
                if name not in FEATURE_NAMES: raise PackError(f"FEATURES: unknown feature {name!r} (known: {', '.join(FEATURE_NAMES)})")
                if status not in FEATURE_STATUSES: raise PackError(f"FEATURES: {name}'s status {status!r} (verified or experimental)")
            for name, (_, made) in FEATURE_NAMES.items():
                if made and made(v) != (name in features(v)):
                    said, does = ("lists", "don't make") if name in features(v) else ("leaves out", "make")
                    raise PackError(f"FEATURES {said} {name}, but the pack's settings {does} it")
            print(f"ok     {title} features: {v['FEATURES'] or '-'}")
            known = re.findall(r'\{"(\w+)", "[^"]*"\}', open(os.path.join(here, "..", "runtime", "ui", "languages.h")).read())
            for lang in v["LANGUAGES"].split():
                if lang not in known: raise PackError(f"LANGUAGES: unknown language {lang!r} (runtime/ui/languages.h: {', '.join(known)})")
        except PackError as e:
            bad += 1; print(f"BAD    {title}: {e}")
    return bad


TABLE_START, TABLE_END = "<!-- games table: made by pipeline/packconf.py --table --write README.md -->", "<!-- end of games table -->"
_REGIONS = {"PCSA": "US", "PCSE": "US", "PCSB": "EU", "PCSF": "EU", "PCSC": "JP", "PCSG": "JP", "PCSD": "Asia", "PCSH": "Asia"}


def table():
    """the README's games table: every pack in packs/, its versions and FEATURES"""
    import glob
    here = os.path.dirname(os.path.abspath(__file__))
    rows = []
    for path in sorted(glob.glob(os.path.join(here, "..", "packs", "*", "pack.toml"))):
        title = os.path.basename(os.path.dirname(path)); s = load(path, ""); _, versions, _ = read(path)
        vs = []
        for name, x in versions.items():
            note = s["UPDATE_NOTE"].replace(f"Update {name} ", "") if x.get("status") == "recommended" and len(versions) > 1 else ""
            vs.append(f"{name} (recommended{': ' + note if note else ''})" if x.get("status") == "recommended" and len(versions) > 1 else name)
        f = features(s)
        cells = ["Yes" if f.get(n) == "verified" else "Experimental" if n in f else "" for n in FEATURE_NAMES]
        if "online" in f and s["ONLINE_VERSIONS"]:   # (online play only in the versions whose rooms it matches)
            i = list(FEATURE_NAMES).index("online"); cells[i] += f" ({', '.join(s['ONLINE_VERSIONS'].split())})"
        rows.append((s["NAME"], [s["NAME"], _REGIONS.get(title[:4], "?"), title, ", ".join(vs)] + cells))
    rows.sort(key=lambda r: (-sum(1 for c in r[1][4:] if c), r[0]))   # (the most enhanced first)
    head = ["Game", "Release", "Title ID", "Versions"] + [label for label, _ in FEATURE_NAMES.values()]
    lines = ["| " + " | ".join(head) + " |", "|" + "---|" * len(head)] + ["| " + " | ".join(r[1]) + " |" for r in rows]
    return "\n".join(lines)


def _table_in(readme):
    text = open(readme, encoding="utf-8").read()
    a, b = text.find(TABLE_START), text.find(TABLE_END)
    if a < 0 or b < a: raise PackError(f"{readme}: no games table markers ({TABLE_START} ... {TABLE_END})")
    return text, a + len(TABLE_START), b


if __name__ == "__main__":
    if sys.argv[1:] == ["--check"]: sys.exit(1 if _check() else 0)
    if sys.argv[1:2] == ["--table"]:
        t = table()
        if len(sys.argv) == 4 and sys.argv[2] in ("--check", "--write"):
            text, a, b = _table_in(sys.argv[3]); new = text[:a] + "\n" + t + "\n" + text[b:]
            if sys.argv[2] == "--write": open(sys.argv[3], "w", encoding="utf-8").write(new); print(f"wrote the games table into {sys.argv[3]}")
            elif new != text: print(f"{sys.argv[3]}: the games table is out of date: pipeline/packconf.py --table --write {sys.argv[3]}"); sys.exit(1)
            else: print(f"ok     {sys.argv[3]}'s games table matches the packs")
        else: print(t)
        sys.exit(0)
    if len(sys.argv) >= 2:
        for k, val in load(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "").items(): print(f"{k}={val}")
        sys.exit(0)
    print(__doc__); sys.exit(1)
