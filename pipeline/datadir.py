"""The player's library: games, firmware, licences, builds and saves (the folder the code calls the dump).

ATIV_DUMP=<dir> when set (a test library, a second disk); else <root>/dump when it exists (a checkout, or an install
from before the library had a folder of its own: nothing moves); else the platform's per-user data folder, beside the
launcher's settings, so an update can replace the install without touching anything the player made:
  macOS    ~/Library/Application Support/ProjectNativ/Nativ/library
  Linux    $XDG_DATA_HOME/ProjectNativ/Nativ/library   (~/.local/share when unset)
  Windows  %LOCALAPPDATA%/ProjectNativ/Nativ/library    (local rather than roaming: a game is gigabytes)
The launcher makes the same choice (launcher/hub.cpp library_dir) and passes it to the stages as ATIV_DUMP.
"""
import os
import sys


def library_dir(root: str) -> str:
    d = os.environ.get("ATIV_DUMP")
    if d: return d
    if os.path.isdir(os.path.join(root, "dump")): return os.path.join(root, "dump")
    home = os.path.expanduser("~")
    if sys.platform == "win32": base = os.environ.get("LOCALAPPDATA") or os.path.join(home, "AppData", "Local")
    elif sys.platform == "darwin": base = os.path.join(home, "Library", "Application Support")
    else: base = os.environ.get("XDG_DATA_HOME") or os.path.join(home, ".local", "share")
    return os.path.join(base, "ProjectNativ", "Nativ", "library")


def prefs_dir() -> str:
    """the launcher's own folder (SDL_GetPrefPath("ProjectNativ", "Nativ"), or ATIV_PREFS): its settings, the settings every
    game reads (settings.ini) and its log (launcher.log). Windows keeps it in the roaming folder, as SDL does"""
    d = os.environ.get("ATIV_PREFS")
    if d: return d
    home = os.path.expanduser("~")
    if sys.platform == "win32": base = os.environ.get("APPDATA") or os.path.join(home, "AppData", "Roaming")
    elif sys.platform == "darwin": base = os.path.join(home, "Library", "Application Support")
    else: base = os.environ.get("XDG_DATA_HOME") or os.path.join(home, ".local", "share")
    return os.path.join(base, "ProjectNativ", "Nativ")
