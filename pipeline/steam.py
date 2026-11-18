#!/usr/bin/env python3
"""A game as a Steam shortcut (a "non-Steam game"): Steam's Game Mode, on a Steam Deck above all, then starts it like any other
of its games. The shortcut runs the launcher with --play <title> (launcher/hub.cpp g_autoplay: the game built when it needs
it, the launcher gone when the game ends), and carries the game's own art from the player's copy (sce_sys: pic0.png as the
grid and hero picture, icon0.png as the icon).

    steam.py add <title> <name> <launcher> [<art folder>]     every Steam user on this machine gets it (one already there
                                                              for the title is replaced)

Steam keeps its shortcuts in each user's config/shortcuts.vdf, a binary VDF it rewrites when it exits: with Steam running,
an edit would be lost, so this refuses then. Nothing here talks to Steam or the network."""
import os
import struct
import subprocess
import sys
import zlib


def userdata_dirs():
    """the userdata folders of the Steam installs on this machine"""
    home = os.path.expanduser("~")
    cands = []
    if sys.platform == "win32":
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k: cands.append(os.path.join(winreg.QueryValueEx(k, "SteamPath")[0], "userdata"))
        except OSError:
            pass
        cands.append(os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), "Steam", "userdata"))
    elif sys.platform == "darwin":
        cands.append(os.path.join(home, "Library", "Application Support", "Steam", "userdata"))
    else:
        cands += [os.path.join(home, ".steam", "steam", "userdata"), os.path.join(home, ".local", "share", "Steam", "userdata"),
                  os.path.join(home, ".var", "app", "com.valvesoftware.Steam", ".local", "share", "Steam", "userdata")]
    seen, out = set(), []
    for c in cands:
        if os.path.isdir(c) and os.path.realpath(c) not in seen: seen.add(os.path.realpath(c)); out.append(c)
    return out


def steam_running():
    try:
        if sys.platform == "win32":
            r = subprocess.run(["tasklist", "/FI", "IMAGENAME eq steam.exe", "/NH"], capture_output=True, text=True)
            return "steam.exe" in r.stdout.lower()
        return subprocess.run(["pgrep", "-x", "steam_osx" if sys.platform == "darwin" else "steam"], capture_output=True).returncode == 0
    except OSError:
        return False


# binary VDF: a map is {type byte, name, value}... 0x08; 0x00 a nested map, 0x01 a string, 0x02 a 32-bit int
def vdf_load(b):
    def cstr(i):
        j = b.index(b"\0", i); return b[i:j].decode("utf-8", "replace"), j + 1
    def mapping(i):
        d = {}
        while True:
            t = b[i]; i += 1
            if t == 0x08: return d, i
            k, i = cstr(i)
            if t == 0x00: d[k], i = mapping(i)
            elif t == 0x01: d[k], i = cstr(i)
            elif t == 0x02: d[k] = struct.unpack_from("<i", b, i)[0]; i += 4
            elif t == 0x07: d[k] = struct.unpack_from("<Q", b, i)[0]; i += 8
            else: raise ValueError(f"shortcuts.vdf: value type {t} at {i - 1}")
    return mapping(0)[0] if b else {}


def vdf_dump(d):
    out = bytearray()
    def mapping(m):
        for k, v in m.items():
            kb = k.encode("utf-8") + b"\0"
            if isinstance(v, dict): out.extend(b"\x00" + kb); mapping(v)
            elif isinstance(v, int): out.extend(b"\x02" + kb + struct.pack("<i", v if v < 1 << 31 else v - (1 << 32)))
            else: out.extend(b"\x01" + kb + str(v).encode("utf-8") + b"\0")
        out.append(0x08)
    mapping(d)
    return bytes(out)


def shortcut_id(exe, name):
    """the id Steam gives a shortcut (its art files are named by it): CRC-32 of the quoted exe and the name, top bit set"""
    return (zlib.crc32((exe + name).encode("utf-8")) & 0xffffffff) | 0x80000000


def add(title, name, launcher, art=""):
    if steam_running(): sys.exit("Close Steam first: it rewrites its list of games when it quits, so a game added while it runs would be lost.")
    users = [os.path.join(u, d) for u in userdata_dirs() for d in sorted(os.listdir(u)) if d.isdigit() and d != "0"]
    if not users: sys.exit("No Steam account found on this computer: open Steam and sign in once, then try again.")
    exe = f'"{launcher}"'; start = f'"{os.path.dirname(launcher)}"'; opts = f"--play {title}"
    sid = shortcut_id(exe, name)
    icon = os.path.join(art, "icon0.png") if art and os.path.isfile(os.path.join(art, "icon0.png")) else ""
    for u in users:
        cfg = os.path.join(u, "config"); os.makedirs(os.path.join(cfg, "grid"), exist_ok=True)
        path = os.path.join(cfg, "shortcuts.vdf")
        data = vdf_load(open(path, "rb").read()) if os.path.isfile(path) else {}
        sc = data.get("shortcuts", {})
        keep = [v for v in sc.values() if not (isinstance(v, dict) and v.get("LaunchOptions") == opts)]   # (this title's earlier one goes)
        entry = {"appid": sid, "AppName": name, "Exe": exe, "StartDir": start, "icon": "", "ShortcutPath": "", "LaunchOptions": opts,
                 "IsHidden": 0, "AllowDesktopConfig": 1, "AllowOverlay": 1, "OpenVR": 0, "Devkit": 0, "DevkitGameID": "",
                 "DevkitOverrideAppID": 0, "LastPlayTime": 0, "FlatpakAppID": "", "tags": {"0": "Project Nativ"}}
        if icon:   # the art, named by the shortcut's id (Steam's grid folder): grid, hero and icon from the game's own sce_sys
            pic = os.path.join(art, "pic0.png")
            for suffix, src in (("", pic), ("_hero", pic), ("_icon", icon)):
                if os.path.isfile(src):
                    with open(src, "rb") as f, open(os.path.join(cfg, "grid", f"{sid}{suffix}.png"), "wb") as o: o.write(f.read())
            entry["icon"] = os.path.join(cfg, "grid", f"{sid}_icon.png")
        data["shortcuts"] = {str(i): v for i, v in enumerate(keep + [entry])}
        tmp = path + ".nativ"
        with open(tmp, "wb") as f: f.write(vdf_dump(data))
        os.replace(tmp, path)
        print(f"added {name} to Steam ({os.path.basename(u)}): it starts with {launcher} --play {title}")


def selftest():
    d = {"shortcuts": {"0": {"appid": shortcut_id('"a"', "b"), "AppName": "b", "Exe": '"a"', "tags": {"0": "x"}, "IsHidden": 0}}}
    assert vdf_load(vdf_dump(d)) == {"shortcuts": {"0": dict(d["shortcuts"]["0"], appid=d["shortcuts"]["0"]["appid"] - (1 << 32))}}
    print("steam.py self-test: OK")


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[:1] == ["add"] and len(a) >= 4: add(a[1], a[2], a[3], a[4] if len(a) > 4 else "")
    elif a[:1] == ["selftest"]: selftest()
    else: sys.exit(__doc__)
