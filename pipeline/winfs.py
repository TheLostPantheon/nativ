"""winfs.py: removing files the way POSIX does, on Windows too.

Windows refuses to delete a read-only file, and the read-only attribute belongs to the file, not the name: every hard link
of it shares it. An update layered over its game (packs.py layer) is hard links into the game's own copy, often read-only
(copied off another system with its write bits off) -- clearing the attribute to remove a link would clear it on the
game's copy too. So a read-only name is removed with POSIX semantics and the attribute ignored, left as it was
(FileDispositionInfoEx, Windows 10 1809 and later, NTFS). Elsewhere these are os.remove and shutil.rmtree.
"""
import os
import shutil
import sys


def remove_file(p):
    try:
        os.remove(p)
        return
    except PermissionError:
        if os.name != "nt": raise
    import ctypes
    from ctypes import wintypes
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.CreateFileW.restype = wintypes.HANDLE
    k.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    k.SetFileInformationByHandle.argtypes = [wintypes.HANDLE, ctypes.c_int, wintypes.LPVOID, wintypes.DWORD]
    k.CloseHandle.argtypes = [wintypes.HANDLE]
    DELETE, SHARE_ALL, OPEN_EXISTING, OPEN_REPARSE_POINT = 0x10000, 7, 3, 0x00200000   # (a link itself, not its target)
    h = k.CreateFileW(os.path.abspath(p), DELETE, SHARE_ALL, None, OPEN_EXISTING, OPEN_REPARSE_POINT, None)
    if h is None or h == ctypes.c_void_p(-1).value: raise ctypes.WinError(ctypes.get_last_error())
    try:
        FileDispositionInfoEx = 21
        flags = ctypes.c_ulong(0x1 | 0x2 | 0x10)   # DELETE | POSIX_SEMANTICS | IGNORE_READONLY_ATTRIBUTE
        if not k.SetFileInformationByHandle(h, FileDispositionInfoEx, ctypes.byref(flags), ctypes.sizeof(flags)):
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        k.CloseHandle(h)


def remove_tree(p):
    """shutil.rmtree, read-only files included; a missing tree is fine"""
    if not os.path.lexists(p): return
    def again(func, path, _exc):
        if func in (os.unlink, os.remove): remove_file(path)
        else: raise
    if sys.version_info >= (3, 12): shutil.rmtree(p, onexc=again)
    else: shutil.rmtree(p, onerror=lambda f, path, info: again(f, path, info[1]))
