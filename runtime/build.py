#!/usr/bin/env python3
"""Build the native host for a recompiled module: build.py <recomp base (out.c from recomp.py, without .c)> <out binary>
[<module base>...]. Part files compile in parallel and are reused while newer than their source and the runtime.
The modules loaded with it (recomp.py --module): their bases as arguments, else RECOMP_MODULE_BASES (space-separated).

TARGET=windows: cross-compiled with llvm-mingw (runtime/platform/win/setup-toolchain.sh; ATIV_WIN_TOOLCHAIN, default <root>/toolchain
when there is one, else ~/toolchains/ativ-win) into <out>.exe, SDL3.dll copied beside it; objects in obj_win/ (and ImGui's in obj_win/). On
Windows itself it is the only target, built by the same toolchain's Windows-hosted llvm-mingw.

Linux with an install's toolchain (runtime/platform/linux/setup-toolchain.sh: zig cc aimed at an old glibc, SDL3, zlib, FFmpeg;
ATIV_LINUX_TOOLCHAIN, default <root>/toolchain when there is one): nothing of the machine's own but its C library -- SteamOS has
no compiler or development files; libSDL3.so.0 copied beside the binary, objects in obj_lt*/. Without one: the machine's
compiler and pkg-config's libraries, as before.

macOS with an install's toolchain (runtime/platform/macos/setup-toolchain.sh: SDL3, SPIRV-Cross, FFmpeg built for macOS 13;
ATIV_MAC_TOOLCHAIN, default <root>/toolchain when there is one): the libraries from there, Apple's clang and SDK from the Command
Line Tools, libSDL3.0.dylib copied beside the binary; objects in obj_mt*/. Without one: Homebrew's, through pkg-config, as before.

RECOMP_CPU=native (the default; a cross build is always generic) | generic | <a processor name>: what the code is compiled
for; objects per processor in obj_<name>/ (obj_apple-m4, obj_win_znver5). RECOMP_CFLAGS: more code generation flags.
"""
import concurrent.futures
import glob
import hashlib
import os
import platform
import re
import shutil
import subprocess
import sys
import threading

RT = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(RT); RECOMPILER = os.path.join(ROOT, "recompiler")
SUBDIRS = ("core", "gfx", "audio", "media", "input", "net", "ui", "platform", "debug")   # the runtime's parts (docs/architecture.md)
# every part's folder on the include path, so a source names another's header plainly ("hle.h"); the repository root for
# third_party/<library>/...; the recompiler's folder for rt.h
INCLUDES = [f for d in SUBDIRS for f in ("-I", os.path.join(RT, d))] + ["-I", RECOMPILER, "-I", ROOT]


def part(name):   # a runtime source by its name ("kernel", "online/stun"): the file in whichever part holds it
    for d in SUBDIRS:
        f = os.path.join(RT, d, name + ".c")
        if os.path.isfile(f): return f
    raise SystemExit(f"build: no runtime source {name}.c")
ENV = os.environ
HOSTED_WINDOWS = os.name == "nt"
DARWIN = platform.system() == "Darwin"
_print_lock = threading.Lock()


def say(s):
    with _print_lock: print(s, flush=True)


def die(s):
    say(s); sys.exit(1)


def newer(a, b):   # zsh's a -nt b
    try: return os.stat(a).st_mtime_ns > os.stat(b).st_mtime_ns
    except FileNotFoundError: return os.path.exists(a)


def out_of(cmd):
    try: return subprocess.run(cmd, capture_output=True, text=True).stdout.split()
    except OSError: return []


def pkg_exists(*mods):
    try: return subprocess.run(["pkg-config", "--exists", *mods], capture_output=True).returncode == 0
    except OSError: return False


def resolved_cpu(cc, flag):
    """the processor a native flag means to this compiler (clang's -target-cpu), or "native" when it doesn't say"""
    try: err = subprocess.run([cc, flag, "-###", "-x", "c", "-c", os.devnull, "-o", os.devnull], capture_output=True, text=True).stderr
    except OSError: return "native"
    m = re.search(r'"-target-cpu" "([\w.-]+)"', err)
    return m[1] if m else "native"


def memory_gb():
    """the memory a build may take: what other residents leave on Linux (MemAvailable), RAM on macOS (it compresses), the
    available physical memory on Windows"""
    if DARWIN: return int(out_of(["sysctl", "-n", "hw.memsize"])[0]) >> 30
    if HOSTED_WINDOWS:
        import ctypes
        class MS(ctypes.Structure):
            _fields_ = [("len", ctypes.c_ulong), ("load", ctypes.c_ulong)] + [(n, ctypes.c_ulonglong) for n in ("tot", "avail", "ptot", "pavail", "vtot", "vavail", "xavail")]
        m = MS(); m.len = ctypes.sizeof(MS); ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
        return m.avail >> 30
    for l in open("/proc/meminfo"):
        if l.startswith("MemAvailable"): return int(l.split()[1]) >> 20
    return 2


def main(argv):
    if len(argv) < 2: sys.stdout.write(__doc__); return 1
    BASE = os.path.abspath(argv[0]); OUT = os.path.abspath(argv[1])
    mods = argv[2:] or (ENV.get("RECOMP_MODULE_BASES") or "").split()
    WIN = HOSTED_WINDOWS or ENV.get("TARGET") == "windows"
    PROFILE, VERIFY = ENV.get("PROFILE"), ENV.get("VERIFY")
    # RECOMP_CPU: the processor the code is compiled for. native (the default when a machine builds for itself): its own,
    # every instruction it has. Each player compiles their own copy, so nothing has to run anywhere else; on x86-64 the
    # toolchain's default is otherwise the 2003 baseline (SSE2). generic: the toolchain's default, for a binary that runs on
    # other machines (a cross build always is). Floating point stays exact either way: no contraction into fused
    # multiply-adds (-ffp-contract=off) and no reassociation, so a native build draws the gate's frames byte for byte.
    # RECOMP_CFLAGS: more code generation flags (experiments: -O3, -flto=thin). Each set keeps its own objects.
    # The objects are kept per processor, by the name the compiler resolves native to (apple-m4, znver5): an install moved
    # to another machine rebuilds rather than running instructions its processor may not have.
    cross = WIN and not HOSTED_WINDOWS
    cpu = "generic" if cross else (ENV.get("RECOMP_CPU") or "native")
    machine = platform.machine().lower()
    CPUFLAGS = [] if cpu == "generic" else [("-mcpu=" if machine in ("arm64", "aarch64") else "-march=") + cpu]
    extra = (ENV.get("RECOMP_CFLAGS") or "").split()
    FLAGS = ["-O2", "-ffp-contract=off"] + CPUFLAGS + extra + ["-w"] + INCLUDES
    PLATFORM = [part("platform_posix")]
    exe = ".exe" if HOSTED_WINDOWS else ""
    CC, CXX = ENV.get("CC"), ENV.get("CXX")
    LT = None if WIN or DARWIN else (ENV.get("ATIV_LINUX_TOOLCHAIN") or next((d for d in (os.path.join(ROOT, "toolchain"),) if os.path.isfile(os.path.join(d, "bin", "cc"))), None))
    MT = (ENV.get("ATIV_MAC_TOOLCHAIN") or next((d for d in (os.path.join(ROOT, "toolchain"),) if os.path.isfile(os.path.join(d, "SDL3", "lib", "libSDL3.0.dylib"))), None)) if DARWIN and not WIN else None
    if WIN:
        # (an install's own, beside the tools -- <root>/toolchain -- else the developer's)
        WT = ENV.get("ATIV_WIN_TOOLCHAIN") or next((d for d in (os.path.join(ROOT, "toolchain"),) if os.path.isdir(d)), os.path.join(os.path.expanduser("~"), "toolchains", "ativ-win"))
        cc = os.path.join(WT, "llvm-mingw", "bin", "x86_64-w64-mingw32-clang" + exe)
        if not os.path.isfile(cc): die(f"no Windows toolchain at {WT}: run runtime/platform/win/setup-toolchain.sh")
        if VERIFY or PROFILE: die("VERIFY/PROFILE builds: macOS and Linux only")
        CC = cc; CXX = os.path.join(WT, "llvm-mingw", "bin", "x86_64-w64-mingw32-clang++" + exe)
        SDLW = os.path.join(WT, "SDL3", "x86_64-w64-mingw32")
        # win32_compat.h: the POSIX names Windows spells differently (mkdir's mode, pread, realpath); 64-bit off_t;
        # mingw-w64's localtime_r/gmtime_r
        FLAGS += ["-D_FILE_OFFSET_BITS=64", "-D_POSIX_THREAD_SAFE_FUNCTIONS", "-include", os.path.join(RT, "platform", "win32_compat.h"), "-I", os.path.join(WT, "zlib", "include")]
        PLATFORM = [part("platform_win32"), part("win32_compat")]
        OUT = OUT.removesuffix(".exe") + ".exe"
        sdl_cflags = ["-I" + os.path.join(SDLW, "include")]
        sdl_libs = [os.path.join(SDLW, "lib", "libSDL3.dll.a")]   # (the import library by path: links under -static too)
    elif LT:
        if not os.path.isfile(os.path.join(LT, "bin", "cc")): die(f"no Linux toolchain at {LT}: run runtime/platform/linux/setup-toolchain.sh")
        if VERIFY or PROFILE: die("VERIFY/PROFILE builds: the machine's own compiler (no ATIV_LINUX_TOOLCHAIN)")
        CC = CC or os.path.join(LT, "bin", "cc"); CXX = CXX or os.path.join(LT, "bin", "c++")
        FLAGS += ["-I", os.path.join(LT, "zlib", "include")]
        sdl_cflags = ["-I" + os.path.join(LT, "SDL3", "include")]
        sdl_libs = ["-L" + os.path.join(LT, "SDL3", "lib"), "-lSDL3", "-Wl,-rpath,$ORIGIN", "-Wl,--strip-debug"]   # (found beside the binary: copied there below; zig's own debug info, ~150 MB of it, left out -- the symbols crash reports name stay)
    elif MT:
        if not os.path.isfile(os.path.join(MT, "SDL3", "lib", "libSDL3.0.dylib")): die(f"no Mac toolchain at {MT}: run runtime/platform/macos/setup-toolchain.sh")
        sdl_cflags = ["-I" + os.path.join(MT, "SDL3", "include")]
        sdl_libs = ["-L" + os.path.join(MT, "SDL3", "lib"), "-lSDL3", "-Wl,-rpath,@executable_path"]   # (found beside the binary: copied there below)
    else:
        sdl_cflags = out_of(["pkg-config", "--cflags", "sdl3"]); sdl_libs = out_of(["pkg-config", "--libs", "sdl3"])
    # CC/CXX: clang where it's installed -- macOS's compiler and the Android NDK's, so every platform builds
    # the generated C the same way; on a part file it is also ~5x faster than GCC 16 at half the memory
    # (Uncharted's game_part6.c: clang 15 s / 1.1 GB, gcc 70+ s / 2.3 GB)
    CC = CC or ("clang" if shutil.which("clang") else "cc")
    CXX = CXX or ("clang++" if shutil.which("clang++") else "c++")
    tag = "" if not CPUFLAGS else "_" + (resolved_cpu(CC, CPUFLAGS[0]) if cpu == "native" else cpu)
    if extra: tag += "_" + hashlib.sha1(" ".join(extra).encode()).hexdigest()[:8]
    OBJ = os.path.join(os.path.dirname(BASE), "obj" + ("_prof" if PROFILE else "") + ("_verify" if VERIFY else "") + ("_win" if WIN else "") + ("_lt" if LT else "") + ("_mt" if MT else "") + tag)
    os.makedirs(OBJ, exist_ok=True)
    # PROFILE=1: compile the guest code with its per-instruction hook (the host's profiler); objects in obj_prof/
    if PROFILE: FLAGS.append("-DRECOMP_TRACE")
    # VERIFY=1: the in-place checker (runtime/debug/verify.c): guest code with its ENTER hook, linked with
    # Unicorn (the copy bundled with the Python package in .venv); objects in obj_verify/
    EXTRA = []
    if VERIFY:
        UC = ENV.get("UNICORN_DIR") or next(iter(sorted(glob.glob(os.path.join(ROOT, ".venv", "lib", "python3*", "site-packages", "unicorn")))), "")
        UCLIB = "-l:libunicorn.so.2" if os.path.isfile(os.path.join(UC, "lib", "libunicorn.so.2")) and not os.path.isfile(os.path.join(UC, "lib", "libunicorn.so")) else "-lunicorn"   # pip's Linux wheel
        FLAGS += ["-DRECOMP_VERIFY", "-I", os.path.join(UC, "include")]
        EXTRA = [part("verify"), "-L" + os.path.join(UC, "lib"), UCLIB, "-Wl,-rpath," + os.path.join(UC, "lib")]
    # (each with its base's header, game.h: recomp.py rewrites only the files whose text changed, so a part can be older
    # than a header whose declarations it compiles against)
    header = {}
    for b in [BASE] + mods:   # the game, then the modules loaded with it (recomp.py --module)
        for s in [b + ".c"] + sorted(glob.glob(glob.escape(b) + "_part*.c")): header[s] = b + ".h"
    srcs = list(header)
    srcs += sorted(glob.glob(os.path.join(ROOT, "third_party", "libatrac9", "*.c")))   # LibAtrac9 (MIT): the NGS ATRAC9 player's decoder
    # JOBS: compiles at once. A part file is ~10 MB of C: Uncharted's 40 parts all at once under GCC peaked
    # at 27 GB and systemd-oomd killed the build on a 30 GB Linux machine (one that also keeps a language
    # model resident). Default: one per 2 GB (3 under GCC) -- of the memory available now on Linux
    # (MemAvailable: what other residents leave), of RAM on macOS (it compresses) -- at most one per CPU
    jobs = int(ENV.get("JOBS") or 0)
    if not jobs:
        per = 3 if "Free Software Foundation" in " ".join(out_of([CC, "--version"])) else 2
        jobs = max(1, min(memory_gb() // per, os.cpu_count() or 1))
    rt = os.path.join(RECOMPILER, "rt.h")
    # rt.h and the part's header by content (<object>.in: their hash when it was compiled), not by time: a checkout that
    # rewrites rt.h unchanged (a branch switch, a pull, a second worktree) rebuilt every part -- ~20 min for a 30-part
    # game -- for the same object code. An object from before these stamps: the time test once, then a stamp
    sums = {}
    def digest(*files):
        if files not in sums:
            h = hashlib.sha1()
            for f in files:
                try: h.update(open(f, "rb").read())
                except OSError: h.update(b"\0missing")
            sums[files] = h.hexdigest()
        return sums[files]
    def write_stamp(o, text):
        with open(f"{o}.in.{os.getpid()}", "w") as f: f.write(text + "\n")
        os.replace(f"{o}.in.{os.getpid()}", o + ".in")
    work = []
    for s in srcs:
        o = os.path.join(OBJ, os.path.splitext(os.path.basename(s))[0] + ".o")
        ins = (rt, header[s]) if s in header else (rt,); now = digest(*ins)
        try: was = open(o + ".in").read().strip()
        except OSError: was = None
        if not os.path.isfile(o) or newer(s, o): stale = True
        elif was is None:
            stale = any(newer(f, o) for f in ins)
            if not stale: write_stamp(o, now)
        else: stale = was != now
        if stale: work.append(([CC, "-c"] + FLAGS + [s, "-o", o], s, o, CC, lambda o=o, now=now: write_stamp(o, now)))
    # the in-game overlay (runtime/ui/overlay.cpp, this project's one C++ file) and Dear ImGui itself
    # (third_party/imgui/, MIT: see its own README for the pinned release): built once into their own
    # cached objects -- nothing here depends on which game is being linked -- and reused by that game's later builds
    # while newer than overlay.h or their own sources, the same "newer than" test as above.
    # In the game's own object folder (OBJ/imgui), not the install's: an install is never written (a signed Mac app
    # would no longer match its signature). Each object is written to a temporary and renamed into place (whole or not
    # at all), so two builds of one title never see a half-written .o.
    IMGUI = os.path.join(ROOT, "third_party", "imgui"); IMGUIOBJ = os.path.join(OBJ, "imgui"); os.makedirs(IMGUIOBJ, exist_ok=True)
    CXXFLAGS = ["-O2", "-std=c++17", "-w", "-I", IMGUI, "-I", os.path.join(IMGUI, "backends")] + INCLUDES + sdl_cflags
    if WIN: CXXFLAGS += ["-D_FILE_OFFSET_BITS=64", "-include", os.path.join(RT, "platform", "win32_compat.h")]
    imgui_srcs = [os.path.join(IMGUI, f) for f in ("imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp", os.path.join("backends", "imgui_impl_sdl3.cpp"), os.path.join("backends", "imgui_impl_sdlgpu3.cpp"))] + [os.path.join(RT, "ui", "overlay.cpp")]
    # the touch card's input icons (third_party/kenney-prompts/prompts.png) as bytes overlay.cpp includes: a game's binary
    # carries them, wherever it runs from
    PROMPTS = os.path.join(ROOT, "third_party", "kenney-prompts", "prompts.png"); PROMPTS_H = os.path.join(IMGUIOBJ, "prompts_png.h")
    if not os.path.isfile(PROMPTS_H) or newer(PROMPTS, PROMPTS_H):
        data = open(PROMPTS, "rb").read()
        with open(f"{PROMPTS_H}.{os.getpid()}", "w") as f:
            f.write("static const unsigned char prompts_png[] = {\n" + ",\n".join(",".join(map(str, data[i:i + 40])) for i in range(0, len(data), 40)) + "\n};\n")
        os.replace(f"{PROMPTS_H}.{os.getpid()}", PROMPTS_H)
    CXXFLAGS += ["-I", IMGUIOBJ]
    for s in imgui_srcs:
        o = os.path.join(IMGUIOBJ, os.path.splitext(os.path.basename(s))[0] + ".o")
        if not os.path.isfile(o) or newer(s, o) or newer(os.path.join(RT, "ui", "overlay.h"), o) or (s.endswith("overlay.cpp") and newer(PROMPTS_H, o)) or stale_deps(o):
            work.append(deps_job([CXX, "-c"] + CXXFLAGS, s, o, CXX))
    # spirv-cross (SPIRV-Cross's C API): only needed where SDL_GPU doesn't consume SPIR-V directly,
    # i.e. on Metal (Mac); Vulkan (Linux/Windows) takes our SPIR-V as-is, no translation at runtime.
    SPIRV_CROSS = []
    SC = os.path.join(MT, "spirv-cross") if MT else None
    if MT and os.path.isfile(os.path.join(SC, "lib", "libspirv-cross-c.a")):   # (the toolchain's static libraries, Homebrew's release)
        SPIRV_CROSS = ["-I" + os.path.join(SC, "include")] + [l for l in (os.path.join(SC, "lib", "libspirv-cross-" + n + ".a") for n in ("c", "msl", "glsl", "hlsl", "reflect", "cpp", "core", "util")) if os.path.isfile(l)] + ["-lc++"]
        FLAGS.append("-DRECOMP_SPIRV_CROSS")
        SPIRV_CROSS += [os.path.join(RT, "gfx", "mtl_archive.m"), "-framework", "Metal", "-framework", "Foundation"]
        FLAGS.append("-DRECOMP_MTL_ARCHIVE")
    elif not WIN and DARWIN and not MT and pkg_exists("spirv-cross-c"):   # (a Windows build from a Mac: Vulkan's SPIR-V, no Metal)
        # pkg-config only has a .pc for the C shim (spirv-cross-c); the backends it calls into
        # (MSL/GLSL/HLSL/reflect/core/util/cpp) are separate static libs Homebrew doesn't give a .pc
        # for, so link them explicitly, in dependency order, from the same lib dir. It's a C++ library
        # throughout, which cc won't pull libc++ in for on its own.
        scdir = out_of(["pkg-config", "--variable=libdir", "spirv-cross-c"])[0]
        SPIRV_CROSS = out_of(["pkg-config", "--cflags", "spirv-cross-c"]) + ["-L" + scdir] + ["-lspirv-cross-" + n for n in ("c", "msl", "glsl", "hlsl", "reflect", "cpp", "core", "util")] + ["-lc++"]
        FLAGS.append("-DRECOMP_SPIRV_CROSS")
        # this machine's archive of Metal's compiles (runtime/gfx/mtl_archive.m, Objective-C): shaders and pipelines the game
        # made once never compile again, whatever the OS's shader cache lets go
        SPIRV_CROSS += [os.path.join(RT, "gfx", "mtl_archive.m"), "-framework", "Metal", "-framework", "Foundation"]
        FLAGS.append("-DRECOMP_MTL_ARCHIVE")
    # overlay.cpp + Dear ImGui are C++: cc (clang) won't pull its runtime in for object files alone, the
    # same reasoning as the spirv-cross block's own comment above
    CXXLIB = ["-lc++"] if DARWIN or WIN or LT else ["-lstdc++"]   # (zig's libc++ for the install's toolchain)
    # libavcodec (FFmpeg): the H.264 decoder behind sceAvcdec* (video.c) -- linked where pkg-config finds it
    # (Homebrew's ffmpeg, a Linux distribution's libavcodec-dev); without it movies fail to start, as before
    AVCODEC = []
    if LT:   # the toolchain's static H.264-only FFmpeg, as on Windows
        if os.path.isfile(os.path.join(LT, "ffmpeg", "lib", "libavcodec.a")): AVCODEC = ["-DRECOMP_AVCODEC", "-I" + os.path.join(LT, "ffmpeg", "include"), os.path.join(LT, "ffmpeg", "lib", "libavcodec.a"), os.path.join(LT, "ffmpeg", "lib", "libavutil.a")]
    elif MT:   # the Mac toolchain's static H.264-only FFmpeg
        if os.path.isfile(os.path.join(MT, "ffmpeg", "lib", "libavcodec.a")): AVCODEC = ["-DRECOMP_AVCODEC", "-I" + os.path.join(MT, "ffmpeg", "include"), os.path.join(MT, "ffmpeg", "lib", "libavcodec.a"), os.path.join(MT, "ffmpeg", "lib", "libavutil.a")]
    elif not WIN and pkg_exists("libavcodec", "libavutil"):
        AVCODEC = ["-DRECOMP_AVCODEC"] + out_of(["pkg-config", "--cflags", "--libs", "libavcodec", "libavutil"])
    elif WIN and os.path.isfile(os.path.join(WT, "ffmpeg", "lib", "libavcodec.a")):   # Windows: the toolchain's static H.264-only FFmpeg (runtime/platform/win/setup-toolchain.sh)
        AVCODEC = ["-DRECOMP_AVCODEC", "-I" + os.path.join(WT, "ffmpeg", "include"), os.path.join(WT, "ffmpeg", "lib", "libavcodec.a"), os.path.join(WT, "ffmpeg", "lib", "libavutil.a"), "-lole32", "-luser32", "-lbcrypt"]
    # libcurl: SceHttp (http.c) -- a game's HTTPS to its own (or a community) server; without it the HTTP calls stay stubs
    CURL = ["-DRECOMP_CURL"] + out_of(["pkg-config", "--cflags", "--libs", "libcurl"]) if not WIN and not LT and not MT and pkg_exists("libcurl") else []   # (none in the Linux install's toolchain: stubs, as on Windows)
    if MT: CURL = ["-DRECOMP_CURL", "-lcurl"]   # (macOS's own libcurl, its headers in the SDK)
    LIBS = ["-lm", "-lpthread", os.path.join(LT, "zlib", "lib", "libz.a") if LT else "-lz", "-ldl"]
    if WIN: LIBS = ["-L" + os.path.join(WT, "zlib", "lib"), "-lz", "-lpthread", "-lwinmm", "-lws2_32", "-static", "-Wl,--error-limit=0"]   # (ws2_32: net.c's sockets)   # Windows: libc++, libunwind and winpthreads linked in -- only SDL3.dll ships beside the .exe
    # the game's RUN_ENV as the binary's defaults (vitarecomp build exports it; runtime/core/main.c builtin_env): no quotes or backslashes in it
    ENVDEF = [f'-DRECOMP_BUILTIN_ENV="{ENV["RECOMP_RUN_ENV"]}"'] if ENV.get("RECOMP_RUN_ENV") else []
    # the window's icon (window.c): the repository's assets/brand PNG, macOS's drawn on its own inset grid
    icon = os.path.join(ROOT, "assets", "brand", "png", "icon-" + ("macos-512" if DARWIN else "256") + ".png")
    if not WIN and os.path.isfile(icon): ENVDEF.append(f'-DRECOMP_ICON="{icon}"')
    # the guest addresses the game's hooks use for this build of it (vitarecomp: the pack's GAME_ADDRS; hooks.c game_addr)
    if ENV.get("RECOMP_GAME_ADDRS"): ENVDEF.append(f'-DRECOMP_GAME_ADDRS="{ENV["RECOMP_GAME_ADDRS"]}"')
    host = [part(f) for f in (
        "main hle libc io sysmem apputil fios psarc gxp_index kernel gpu video display gesture gxm_sony usse usse_spirv gfx_soft gfx_gpu "
        "gfx_dispatch gfx_coverage gfx_capture guestwatch metrics settings hooks motion window gpulock ngs stallprof waitstats memscan jpeg camera post "
        "ssao fsr/fsr net np http sha1 ssl_local online/m2_server online/m2_wire online/m2_rooms online/m2_proxy online/discovery online/basic_msg online/stun online/overlay online/dht online/upnp online/aes128 online/status online/crypto").split()]
    # the game's own host code (vitarecomp: every .c in packs/<title>/), in this game's build alone
    host += [f for f in (ENV.get("RECOMP_PACK_SOURCES") or "").split(os.pathsep) if f]
    # the runtime's own sources (and the pack's) as cached objects, compiled in parallel with the game's parts -- they were
    # compiled inside the link command, one after another, on every build: ~60 files, minutes for a one-line change.
    # Each is rebuilt when it or a header it includes (the compiler's -MMD list) is newer than its object; the objects of
    # one set of flags (RUN_ENV, GAME_ADDRS, the toolchain ...) in their own folder, OBJ/host_<hash>, so a diagnostic build
    # with other defaults (BUILD_EXTRA_ENV) never overwrites the play build's
    lists = EXTRA + SPIRV_CROSS + AVCODEC + CURL
    hsrcs = PLATFORM + host + [a for a in lists if a.endswith((".c", ".m")) and os.path.isfile(a)]
    hcflags, lwords, k = FLAGS + ENVDEF + ["-Wall"] + sdl_cflags, [], 0
    while k < len(lists):   # the libraries' words: compile flags (-D, -I ...) and link words, each in their order
        a = lists[k]
        if a in ("-I", "-D", "-isystem", "-include"): hcflags += lists[k:k + 2]; k += 2; continue
        if a.startswith(("-D", "-I", "-isystem", "-F")) or a == "-pthread": hcflags.append(a)
        if not a.startswith(("-D", "-I", "-isystem")) and a not in hsrcs: lwords.append(a)
        k += 1
    HOSTOBJ = os.path.join(OBJ, "host_" + hashlib.sha1("\0".join([CC] + hcflags).encode()).hexdigest()[:10]); os.makedirs(HOSTOBJ, exist_ok=True)
    hobjs = []
    for s in hsrcs:
        rel = os.path.relpath(s, ROOT)
        name = rel.replace(os.sep, "__").replace("/", "__") if not rel.startswith("..") else os.path.basename(s) + "_" + hashlib.sha1(s.encode()).hexdigest()[:8]
        o = os.path.join(HOSTOBJ, os.path.splitext(name)[0] + ".o"); hobjs.append(o)
        if not os.path.isfile(o) or newer(s, o) or stale_deps(o): work.append(deps_job([CC, "-c"] + hcflags, s, o, CC))

    def compile_one(w):
        argv, s, o, cc, done = w
        r = subprocess.run(argv)
        if r.returncode:
            if done: _rm(argv[-1]); _rm(o + f".d.{os.getpid()}")
            return s
        if done: done()
        say(f"  {cc} {os.path.basename(s)}")
        return None
    # the biggest first: a game's largest part (a 24 MB part file) is the build's longest compile and starts at once
    # instead of when the alphabet reaches it
    work.sort(key=lambda w: -os.path.getsize(w[1]))
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex: failed = [f for f in ex.map(compile_one, work) if f]
    if failed: die("build: " + ", ".join(os.path.basename(f) for f in failed) + " did not compile")
    objs = sorted(glob.glob(os.path.join(glob.escape(OBJ), "*.o"))) + sorted(glob.glob(os.path.join(glob.escape(IMGUIOBJ), "*.o")))
    # (Linux links libm/pthread explicitly; zlib: PSARC archives) -- objects only: the code generation flags a link
    # needs (RECOMP_CFLAGS' -flto) and the libraries' link words
    link = [CC] + CPUFLAGS + extra + ["-w"] + hobjs + lwords + objs + sdl_libs + CXXLIB + LIBS + ["-o", OUT]
    sys.stdout.flush()
    r = subprocess.run(link)
    if r.returncode: return r.returncode
    def beside(src):   # SDL copied next to the binary, replaced whole: a game still running keeps the file it loaded
        dst = os.path.join(os.path.dirname(OUT), os.path.basename(src)); tmp = dst + f".new{os.getpid()}"
        shutil.copyfile(src, tmp); os.replace(tmp, dst)
    if WIN: beside(os.path.join(SDLW, "bin", "SDL3.dll"))
    if LT: beside(os.path.join(LT, "SDL3", "lib", "libSDL3.so.0"))   # (what -rpath $ORIGIN finds)
    if MT: beside(os.path.join(MT, "SDL3", "lib", "libSDL3.0.dylib"))   # (what -rpath @executable_path finds)
    say(f"built {OUT}")
    return 0


def deps_file(o): return o + ".d"


def stale_deps(o):
    """whether a file the object was compiled from (the compiler's -MMD list: its source and headers) is newer than it;
    no list: stale"""
    try: text = open(deps_file(o)).read()
    except OSError: return True
    i = text.find(": ")   # (after the target: C:\... has a colon of its own)
    if i < 0: return True
    text = re.sub(r"\\\r?\n", " ", text[i + 2:])   # (continued lines; a space in a name is "\ ")
    return any(newer(d.replace("\\ ", " "), o) for d in re.split(r"(?<!\\)\s+", text) if d)


def deps_job(argv, s, o, cc):
    """a compile into a temporary, with its header list (-MMD), both renamed into place when it succeeds: whole or not at
    all, so two builds of one title never see half a file"""
    pid = os.getpid(); _rm(deps_file(o))   # (a compile that dies leaves no list: compiled again next time)
    def done():
        os.replace(f"{o}.{pid}", o); os.replace(f"{o}.d.{pid}", deps_file(o))
    return (argv + [s, "-MMD", "-MF", f"{o}.d.{pid}", "-o", f"{o}.{pid}"], s, o, cc, done)


def _rm(p):
    try: os.remove(p)
    except OSError: pass


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
