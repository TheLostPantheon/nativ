# The runtime in detail

[How it works](architecture.md) gives the overview. This document goes through the runtime's parts one at a time:
what each does, where it lives, and the settings and switches that change it.

## Loading and running a game

`runtime/core/main.c` loads the game's memory image, starts `module_start` on its own thread, and routes every call
the game makes into the system: each import is resolved once, by its stub's address, to one of three things:

- a **host implementation** in the runtime;
- an export of a **Sony library** loaded with the game, such as libc or libgxm;
- a **stub**, for the few calls nothing implements yet.

The runtime header, `recompiler/rt.h`, gives the generated C its register file, guest memory access, flag, shift and
NEON helpers, and an exclusive monitor that turns `ldrex`/`strex` into compare-and-swap.

## Display and timing

`runtime/gfx/display.c` handles frame flips and the vertical blank.

- **Flips.** Sony's display queue thread calls the game's flip callback, which arrives here as
  `sceDisplaySetFrameBuf`.
- **Vertical blank** runs on a fixed 60 Hz grid, as on the hardware. A wait sleeps half the remaining time
  repeatedly, then spins for the last `RECOMP_VBLANK_SPIN` microseconds (default 200), because macOS wakes ordinary
  sleeps up to 50% late.
- **Waits count edges like the hardware does.** `sceDisplayWaitVblankStartMulti(n)` counts from the edge the thread
  last waited for, and the `WaitSetFrameBuf` calls from the edge the flip lands on.
- **A fixed clock** (`RECOMP_CLOCK=fixed`) makes guest time a function of the frame count, so test runs produce the
  same frames every time.

Audio ports and the controller and touch reads also live here.

## Frame rate

The `frame_rate` setting (Game page of the F10 menu; `RECOMP_FRAME_RATE=60`) lets a game present every vertical blank
instead of every second one. It's experimental and per game, because games assume their own rate in many places:

- **Unit 13** holds about 60 fps in its levels. Its places that treat 1/30 s as one frame get the real frame time
  through instruction hooks while 60 is on.
- **Killzone** runs only the player at 60. Its other systems, written for 30 Hz and 15 Hz steps, keep them
  (`packs/PCSA00107/hooks.c`).

`RECOMP_FRAME_RATE_FROM=n` switches to 60 at frame n, so a recorded session can reach a level at 30 and measure at
60. At 30 fps, and in the gate, every game runs its own code untouched.

## Audio

`runtime/audio/ngs.c` implements NGS, the Vita's voice engine (a DSP on the hardware): racks, voices (simple, Scream
PCM and ATRAC9), patches and buses. ATRAC9 is decoded by LibAtrac9 (MIT, in `third_party/libatrac9`), and output goes
through SDL. Reverb sends are dropped for now.

## Graphics

### The software backend

`runtime/gfx/gfx_soft.c` is a CPU rasteriser drawing into the game's own render targets: depth, stencil, blending and
alpha test; triangle lists, strips and fans; and point sprites, expanded to quads. Scenes are drawn deferred, in bands
across all cores. It has two shading modes:

- parameter-name stand-ins, the default, which suit 2D games;
- the real shader interpreter (`RECOMP_SHADERS=interp`), which runs each draw's own programs through `gfx/usse.c`.

It decodes every texture format the games use, including the block-compressed BC and PVRTC families, and caches
decoded levels.

### The GPU backend

`runtime/gfx/gfx_gpu.c` is the renderer for play (`RECOMP_GFX=soft` switches to the software reference). It runs
through SDL's GPU API: Metal on macOS, by way of SPIRV-Cross, and Vulkan elsewhere.

- **Shaders.** Each program is translated to SPIR-V from the interpreter's decoded instructions
  (`gfx/usse_spirv.c`) and cached by its content, since the shader patcher moves programs around between frames.
- **Everything stays on the GPU.** Render targets keep the precision of their Vita format. Guest memory is only
  written when something reads it: a screenshot, a CPU write into a target, or a format pairing without a view.
- **Views.** When a game reads a target's memory as a texture in another format, layout or region, the backend
  builds a view on the GPU that holds what the software decoder would have read. A view is rebuilt only when its
  target changes.
- **Vertices.** The draw's raw vertex streams and indices are copied as they are, and the vertex shader, specialised
  for the draw's attribute layout, decodes each attribute with the CPU packer's exact conversions.

Anything the translator doesn't cover falls back to the software path for that draw.

### Render scale

The GPU backend can render at 1 to 4 times the Vita's resolution (`RECOMP_RENDER_SCALE=n`, or F2 in the window).
Render targets, depth surfaces, views and copies all live on the GPU at that scale, with viewports and scissors to
match. Guest memory stays at 1x: downloads point-sample each pixel's block and uploads replicate it, so screenshots,
the gate and the game's own CPU reads all see Vita-sized surfaces.

A read that reinterprets a target's bytes at another texel size takes its word from the 1x pixel under the sample
(`snap_fetch` in `usse_spirv.c`). `RECOMP_RENDER_SCALE_SWITCH=frame:n,...` changes the scale in a headless run, which
is how to test that path. `RECOMP_GPU_DUMP_PRESENT=path` writes the presented frame at the render scale.

### The shader archive (macOS)

`runtime/gfx/mtl_archive.m` keeps this machine's own copy of what Metal compiled for a game, in the game's
`shader_cache/`. The OS's shader cache is shared by every program and limited in size, so it can lose a game's shaders;
the archive can't. Each shader's compiled library is kept in its cache file, and every pipeline's GPU code in a binary
archive (`shader_cache/metal/<GPU>_<OS build>/seg_*.bin`), one segment per write.

SDL has no way to be given an archive, so the one Metal call it creates pipelines with is redirected to try the archive
first. With the OS cache emptied, Killzone took all 3,593 shaders and 5,762 pipelines from its archive and compiled
none.

- Segments are written when the warm-up finishes, ten minutes after a new pipeline appears, and at exit.
- Past 48 segments, the next session gathers every pipeline it uses into new segments and drops the old ones when it
  exits.
- An OS update makes the folder stale, and it's deleted.
- Every pipeline is kept except the prediction pass's guesses. `RECOMP_MTL_ARCHIVE=0` turns the archive off.

Linux and Windows rely on the Vulkan drivers' own caches.

## Controls

`runtime/ui/window.c` maps the host's input onto the Vita's controls.

- **Keyboard and mouse.** The mouse looks and aims while captured; hold Alt for a cursor. Left button is the front
  touch panel when the cursor is free; right button is the rear panel.
- **Gamepads.** The triggers are L and R. The bumpers are touch modifiers: hold LB (front) or RB (rear), and either
  stick moves a cursor on that panel while Cross touches. A marker shows the cursor.
- **Touchpads** on DualShock 4 and DualSense pads touch the front panel finger for finger, or the rear with RB held as
  a finger lands.
- **Touchscreens** touch the front panel where a finger lands on the picture, up to six fingers, or the rear with RB
  held. The mouse SDL makes from a finger is dropped, so a tap never fires or captures the mouse.
- **Motion.** A pad's gyroscope and accelerometer become the Vita's motion sensor, with R3 recentring.

Each touch keeps its own ID while held. The front panel's coordinates run 0 to 1919 by 0 to 1087; the rear's active
rows are 108 to 889.

Test aids: `RECOMP_INPUT_RECORD=<file>` records everything the game reads, and `RECOMP_INPUT_PLAY=<file>` plays it
back. `RECOMP_LOG_TOUCH=1` and `RECOMP_LOG_MOTION=1` show touch and motion reads. Without a touchscreen,
`RECOMP_TOUCH_ANY=1` treats any touch device as one (a Mac trackpad becomes an absolute multi-touch surface), and
`RECOMP_FINGER_SCRIPT="frame:down|move|up:finger:x,y;..."` pushes finger events into a windowed run.

## Code hooks

Sometimes a feature has to reach into one particular guest function rather than a system call: a field-of-view
setting, a 60 fps fix, a widescreen camera. A pack's hooks do this.

- `HOOKS = "0xADDR:name ..."` makes the recompiler call `recomp_hook_<name>(c)` at the start of the function at that
  address, before its body. The hook can read or change the registers. When one function serves several callers, the
  return address in `c->r[14]` tells them apart.
- `IHOOKS = "0xADDR:name ..."` calls `recomp_ihook_<name>(c, reg)` right after the instruction at that address, with
  its destination register, for a value a function builds mid-way, such as a 1/30 s constant.

The hook functions are written per game in `packs/<title>/hooks.c`, with shared ones in `runtime/core/hooks.c`.
Without hooks, the generated code is unchanged. The recompiler warns when an address isn't a function entry or an
instruction.

Example: Unit 13's field-of-view setting widens its camera update's fields for the length of the update, so the
projection, culling and stored field of view all agree, while the weapon camera stays as it was. Widening only the
projection had left objects popping in and out at the new edges.

## Metrics

`runtime/core/metrics.c` measures play: frames per second and frame times (average, worst, 1% low), game speed,
scenes and draws per frame, renderer and present time, CPU use, and the audio queue. They show in the window title,
in an overlay that F1 cycles (`RECOMP_OSD=0|1|2` to start with one), or as one line a second on stderr with
`RECOMP_METRICS=1`, headless too. The overlay is drawn on the window only, never into guest memory or screenshots.
