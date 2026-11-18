"""The player's library: games, firmware, licences, builds and saves (the folder the code calls the dump).

ATIV_DUMP=<dir> when set (a test library, a second disk); else <root>/library in a portable install (portable.txt beside
the app); else <root>/dump when it exists (a checkout, or an install from before the library had a folder of its own:
nothing moves); else the folder the player moved it to (hub.ini's library_dir: the launcher's Settings, vitarecomp
library move); else the platform's per-user data folder, beside the launcher's settings, so an update can replace the
install without touching anything the player made:
  macOS    ~/Library/Application Support/ProjectNativ/Nativ/library
  Linux    $XDG_DATA_HOME/ProjectNativ/Nativ/library   (~/.local/share when unset)
  Windows  %LOCALAPPDATA%/ProjectNativ/Nativ/library    (local rather than roaming: a game is gigabytes)
The launcher makes the same choice (launcher/hub.cpp library_dir) and passes it to the stages as ATIV_DUMP.
"""
import os
import sys


def portable(root: str) -> bool:
    """a portable install (Windows' zip: portable.txt beside the app): its library and its settings in its own folder"""
    return os.path.isfile(os.path.join(root, "portable.txt"))


def chosen_library() -> str:
    """the folder the player moved their library to (the launcher's Settings, vitarecomp library move): hub.ini's
    library_dir, when it is there; else """""
    try:
        with open(os.path.join(prefs_dir(), "hub.ini"), encoding="utf-8") as f:
            for line in f:
                k, _, v = line.rstrip("\r\n").partition("=")
                if k == "library_dir" and v and os.path.isdir(v): return v
    except OSError:
        pass
    return ""


def default_library() -> str:
    home = os.path.expanduser("~")
    if sys.platform == "win32": base = os.environ.get("LOCALAPPDATA") or os.path.join(home, "AppData", "Local")
    elif sys.platform == "darwin": base = os.path.join(home, "Library", "Application Support")
    else: base = os.environ.get("XDG_DATA_HOME") or os.path.join(home, ".local", "share")
    return os.path.join(base, "ProjectNativ", "Nativ", "library")


def library_dir(root: str) -> str:
    d = os.environ.get("ATIV_DUMP")
    if d: return d
    if portable(root): return os.path.join(root, "library")
    if os.path.isdir(os.path.join(root, "dump")): return os.path.join(root, "dump")
    return chosen_library() or default_library()


def prefs_dir(root: str = "") -> str:
    """the launcher's own folder (SDL_GetPrefPath("ProjectNativ", "Nativ"), or ATIV_PREFS; a portable install's settings/):
    its settings, the settings every game reads (settings.ini) and its log (launcher.log). Windows keeps it in the roaming
    folder, as SDL does"""
    d = os.environ.get("ATIV_PREFS")
    if d: return d
    if root and portable(root): return os.path.join(root, "settings")
    home = os.path.expanduser("~")
    if sys.platform == "win32": base = os.environ.get("APPDATA") or os.path.join(home, "AppData", "Roaming")
    elif sys.platform == "darwin": base = os.path.join(home, "Library", "Application Support")
    else: base = os.environ.get("XDG_DATA_HOME") or os.path.join(home, ".local", "share")
    return os.path.join(base, "ProjectNativ", "Nativ")
