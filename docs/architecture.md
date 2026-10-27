# How Project Nativ works

This document explains how a PS Vita game becomes a native program: what is translated, what is rebuilt, and how the
pieces fit together. The last section covers where the design is heading.

## The idea

A Vita game is two things: its own ARM code, and Sony's system software that the code calls. Nativ treats them
differently.

- **Code is translated, not emulated.** The game's ARM code is translated to C ahead of time and compiled into a native
  program. The translation is exact by construction, down to flags, rounding and vector lanes, and it can be checked
  against the original instruction by instruction.
- **Sony's libraries run as they are.** The user-mode libraries a game loads, such as libgxm (graphics), libc and
  libfios2 (files), are translated the same way, from the player's own firmware. Their behaviour is Sony's, not our
  reimplementation of it.
- **Only the bottom layer is rewritten.** Below Sony's libraries sit the system calls into the Vita's kernel and
  hardware. That boundary is where Nativ's runtime takes over, and it follows the firmware's own semantics and error
  codes.

The guiding rule: be exact where exactness is cheap, go native where it pays, and keep an exact reference to check
the fast paths against.

## The layers

```
 GAME PACK        our own text: versions, hooks, settings, shader recipes      packs/<title>/
 ─────────────────────────────────────────────────────────────────────────────────────────────
 GUEST CODE       the game and Sony's user-mode libraries, ARM translated to C  built from the player's copy
 ── system-call boundary ─────────────────────────────────────────────────────────────────────
 RUNTIME          kernel and threads · memory · files · audio · video · input   runtime/core, audio, media,
                  · display and timing · networking and online play             input, net
 GRAPHICS         libgxm's state observed → draw stream → GPU or reference      runtime/gfx
 ── platform interface ───────────────────────────────────────────────────────────────────────
 PLATFORM         SDL3: window, input, audio out, GPU (Metal, Vulkan)            runtime/platform, runtime/ui
 FRONT ENDS       the launcher · the in-game menu (F10) · a headless test runner
```

Nothing a game ships is ever part of the repository or a release: not its code, data, shaders, the generated C, the
captures or the recompiled Sony libraries. Players build everything from their own copy.

## Guest code

The pipeline turns a decrypted game into C in three steps.

1. **Analysis** (`recompiler/analyze.py`) loads the program as the Vita's loader would and finds its functions:
   from exports, calls, code pointers, jump tables and prologues. It takes seconds and needs no external tools.
2. **Recompilation** (`recompiler/recomp.py`) writes one C function per guest function. ARM and Thumb-2 code, VFP and
   NEON included, become operations on a `Cpu` structure. Calls into the system become calls into the runtime.
3. **The runtime header** (`recompiler/rt.h`) supplies what the C needs: the register file, guest memory, and helpers
   for flags, shifts and vector lanes.

Guest memory is one 4 GiB block reserved up front. A guest address is an offset into it, so the game's 32-bit pointers
keep working unchanged.

If the game ever calls code the analysis missed, the run records the address, and the next analysis includes it. Sony's
libraries share these seeds between games.

## The runtime

The runtime implements the system calls the game and Sony's libraries make. Each area follows what the firmware does.

- **Kernel and threads** (`core/kernel.c`). Every guest thread is a host thread. Mutexes, condition variables,
  semaphores and event flags wake their waiters exactly when the Vita's kernel would, with its error codes. An optional
  scheduler (`RECOMP_SCHED=vita`) reproduces the console's strict priorities on three cores.
- **Memory** (`core/sysmem.c`): memory blocks, including the GPU's.
- **Files** (`core/io.c`). Reads and writes go to the player's game folder and saves. Sony's libfios2 runs as recompiled
  code on top, so its archive reading and overlays are Sony's own; the runtime supplies the kernel's overlay table
  under it.
- **Audio** (`audio/ngs.c`): the Vita's NGS audio engine, with ATRAC9 decoding through LibAtrac9.
- **Video** (`media/video.c`): Sony's video player libraries run as recompiled code over an H.264 decoder provided
  by FFmpeg.
- **Input** (`ui/window.c`, `input/`): keyboard, mouse, gamepads, touchpads and touchscreens, mapped onto the Vita's
  buttons, sticks, both touch panels and motion sensors.
- **Display and timing** (`gfx/display.c`): frame flips and vertical blanks. A fixed clock makes test runs repeatable
  frame for frame.
- **Networking** (`net/`): real sockets, HTTPS, stand-ins for Sony's PSN services, and online play between copies.
  [Online play](online.md) covers it.

## Graphics

The Vita draws through libgxm, which Nativ runs as Sony's own recompiled code. Below it, the runtime emulates the GPU
driver's interface (`gfx/gpu.c`).

1. **Observing.** Before each scene and draw, `gfx/gxm_sony.c` reads the state libgxm keeps (programs, vertex layout,
   textures, blending, depth and stencil) and hands the renderer an API-neutral draw.
2. **Rendering.** `gfx/gfx_dispatch.c` passes each draw to one of two backends:
   - **The GPU backend** (`gfx/gfx_gpu.c`), used for play. It runs through SDL's GPU API: Metal on macOS, Vulkan on
     Linux and Windows.
   - **The software backend** (`gfx/gfx_soft.c`), a CPU interpreter of the Vita's USSE shader instruction set
     (`gfx/usse.c`). It is slow but exact, and it's the reference every GPU result is compared with.
3. **Shaders.** Each Vita shader program is translated to SPIR-V (`gfx/usse_spirv.c`). Vulkan takes it directly; on
   macOS, spirv-cross turns it into Metal's shading language. Translated shaders are cached, and each game's shader
   recipes let the cache be rebuilt ahead of play.
4. **Render targets.** Games often read a target back in a different format or layout than it was drawn in, such as
   depth read as a texture or a float buffer read as bytes. The GPU backend builds a *view* for each such read, on the
   GPU, rather than copying through memory.

On top of an exact picture, the GPU backend adds optional upgrades: higher render scales, wider aspect ratios with the
HUD kept at its native shape, ambient occlusion, anti-aliasing and sharpening.

## Game packs

Everything specific to one game lives in its pack, `packs/<title>/`, and is our own text:

- `pack.toml`: the releases it supports (by SHA-256), the Sony libraries to load, settings, and code hooks;
- `hooks.c`: the host code those hooks call, such as Killzone's 60 fps scheduling or a widescreen camera;
- `shaders.txt`: the game's shader recipes, as hashes and draw state, with no game bytes.

A hook names a guest address. The recompiler inserts a call to the hook there, so a game fix never edits the generated
C by hand.

## Front ends

- **The launcher** (`launcher/hub.cpp`): your library, first-run setup, settings, and the pipeline behind a button.
- **The in-game menu** (F10, `ui/overlay.cpp`): settings, controls and status while playing, drawn with Dear ImGui.
- **The headless runner**: the same program without a window, for tests and scripted runs.

## Checking the work

Exactness only matters if it's checked. [Working on Nativ](developers.md) covers the tools: the regression gate,
which plays each game to a known frame and compares the picture; the in-place checker, which re-runs sampled calls
against the original instructions; and graphics captures, which replay a frame on either backend in seconds.

## Where it's going

These are the next big steps. None of them is built yet.

- **A shader compiler.** Today's translator mirrors the Vita's register file and half-precision packing bit for bit.
  A compiler would lift each program into an intermediate form, optimise it, and emit plain native shaders. It would
  have an exact mode for verification and a fast mode for play. [The shader compiler](shader-compiler.md) has the
  design.
- **A resource model.** Instead of building views, the compiler would decode another format right inside the shader
  that reads it, so the extra passes disappear.
- **A frame graph.** Knowing each frame's scenes and dependencies up front would let us merge passes and skip storing
  attachments nobody reads. Tile-based GPUs, Apple's and Android's, benefit most.
- **Enhancement packs.** Replacement shaders, higher-resolution textures and per-target resolution scales, all keyed by
  content hashes and kept out of the core.
- **Android.** An APK of our own code, with each player exporting their games from their desktop.
- **Ports.** Other platforms implement the documented platform interface in `runtime/platform/<name>/` instead of
  forking. The rules that keep this possible: guest memory through a base pointer, no compiler-specific builtins in the
  generated C, and no platform-only APIs outside their own backend.
- **A licence**, chosen before anything goes public. Vita3K, which is GPL, is read as a reference and never copied.
