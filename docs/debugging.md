# Debugging a game

How to find out why a game misbehaves: which logs to read, how to reproduce a moment, and how to take a frame apart.
Every switch here is an environment variable, set on the command line or through `EXTRA_ENV` for `vitarecomp`.

## Start with the imports

When something quietly does nothing, such as silent weapons or a frozen ragdoll, the cause has so far always been a
system call that isn't implemented. Check those before suspecting the renderer.

- `RECOMP_IMPORTS=1 RECOMP_HEADLESS=1 ./vitarecomp run <title>` lists every import of the game and its modules, with how
  it's resolved: `host` (implemented in the runtime), `module` (a loaded Sony library) or `STUB`. Then it exits.
- A stub returns 0. For device and network families we don't emulate (NP, networking, HTTP and SSL, voice chat, Near),
  it returns that family's "not initialised" error instead, so the game takes its offline path rather than polling a
  success that never happened. `RECOMP_STUB_ZERO=1` makes every stub return 0.
- A run names each stubbed import the first time it's called, with its caller. When the window closes, the log gets a
  histogram of import calls and every thread's last call.

## The session log

`./vitarecomp run` writes the game's output to `session.log` beside the game (the run before is kept as
`session.prev.log`). It always includes:

- **hitches:** any frame slower than 50 ms, with what the renderer did in it: shaders built, pipelines made, textures
  uploaded, targets downloaded, waits (`RECOMP_HITCH_LOG=<ms>` sets the threshold, 0 turns it off);
- **stalls:** 150 ms or more without a new frame, with the most common thread stacks (`RECOMP_STALL_PROF=<ms>`);
- **marks:** F5 writes `user mark N at frame F`, for "that just happened".

Release builds of games print their assertions nowhere, so a game that stops often shows its last formatted string in
the log (Shovel Knight's are `ycAssert` texts).

## Catching a moment

| Switch | What it does |
|---|---|
| `RECOMP_LOG=n`, `RECOMP_LOG_FROM=n` | Log system calls: the first n, or from call n. |
| `RECOMP_SHOT=n` | Save frame n as an image. `SHOT_EVERY` (with `vitarecomp test`) saves every nth. |
| `RECOMP_SHOT_FROM=a`, `RECOMP_SHOT_TO=b` | Limit shots to a window of frames: every frame of a flicker, without a whole run's worth. |
| `RECOMP_DUMP_FRAME=n` | Log every draw of frame n. |
| `RECOMP_INPUT=from-to:mask,...` | Scripted buttons by frame range. |
| `RECOMP_TOUCH=from-to:x,y;...` | Scripted front-panel touches by frame range (panel coordinates, 1920x1088). |
| `RECOMP_INPUT_RECORD=<file>`, `RECOMP_INPUT_PLAY=<file>` | Record every controller, touch and motion read during play, then serve it back headlessly. This is how a moment deep in a mission becomes a repeatable run. |
| `RECOMP_SETTINGS_SWITCH=frame:key=value,...` | Change a setting mid-run, as the F10 menu would. Compare settings in one run instead of a run per value. |
| `RECOMP_SKIP_MOVIES=1` | Skip the game's videos. |
| `RECOMP_CLOCK=fixed` | Guest time follows the frame count, so runs repeat exactly. |

**Flickers.** A glitch lasting a frame or two is over before F12's capture can start. F5 also saves the frames before
the mark: the last 180 presents (`RECOMP_FRAME_HISTORY=n`), small, in `captures/f5_<frame>/`, plus a log line each
with what was drawn into the displayed buffer: draws, those the HUD layout took, those skipped while their pipeline
built, and those on the software path. A flashed frame with few draws means the game didn't draw it; the usual count
means the renderer lost it. `RECOMP_FRAME_HISTORY_LOG=1` prints those lines for every frame, and
`RECOMP_FRAME_HISTORY_SAVE_AT=<frame>` saves without a window.

## Audio

| Switch | What it does |
|---|---|
| `RECOMP_AUDIO_STATS=1` | Audio queue and underrun statistics. |
| `RECOMP_AUDIO_WAV=<file>` | The final output as a WAV file. |
| `RECOMP_AUDIO_WAV_DIR=<dir>` | Each audio port to its own WAV, in its real format. |
| `RECOMP_NGS_DEBUG=1` | NGS voice setup and parameters. |
| `RECOMP_NGS_WAV_DIR=<dir>` | NGS taken apart: each master bus before clipping, each voice's output and decoded source, the reverb buses, and `voices.log` with plays, events and patches. |

## Where the time goes

| Switch | What it does |
|---|---|
| `RECOMP_METRICS=1` | One line a second of frame rate, frame times, draws and CPU use, headless too. |
| `RECOMP_GPU_STATS=1` | Each frame's draws (GPU, software, skipped while building), why any fell back, and the renderer's host time by stage. |
| `RECOMP_GPU_TIMERS=1\|0` | Force the per-draw timers on or off. They cost 5-6% of the render thread, so they're off unless something reads them. |
| `RECOMP_GPU_FENCE_STATS=1` | The GPU's own busy time per frame, measured with fences. Frame times can't show it: the host never waits for the GPU at present. `RECOMP_GPU_FENCE_TRACE=1` prints every mark; in a replay, mark k is scene k. |
| `RECOMP_GPU_PRESENT_NOSYNC=1` | Keep a headless frame on the GPU, as a windowed run does, for honest headless timings. |
| `build <title> profile`, `RECOMP_PROFILE=<s>` | The profiler build: where the main thread spends its time. |
| `build <title> verify`, `RECOMP_COVERAGE=<file>` | Entry counts of every guest function, all threads. |

## Graphics off the game

Captures let you take a frame apart in seconds, without running the game.

1. Record a frame: `./vitarecomp capture <title> <frame>`, or F12 while playing.
2. Build the replayer once: `zsh runtime/debug/build_replay.sh` (it writes `build/gfxreplay`).
3. Replay from the game's folder: `RECOMP_GFX=gpu build/gfxreplay captures/frame_N.vfc out.ppm`. Leave out
   `RECOMP_GFX=gpu` for the software path. On Linux without a display, add `SDL_VIDEODRIVER=offscreen`.

| Switch | What it does |
|---|---|
| `RECOMP_REPLAY_DUMP=1` | Print the scenes and draws as text, without rendering. |
| `RECOMP_REPLAY_DRAWS=k` | Stop after draw k and write the target as it stands. |
| `RECOMP_REPLAY_KEEP_TARGETS=1` | Never restore a surface the replay has already drawn. Later passes then read this replay's own output: the way to see a fix's effect downstream, and to read a GPU capture's G-buffer, whose memory copy is stale. |
| `RECOMP_REPLAY_WIDEN=k` | Replay as if the screen were k times wider, to test the wide-aspect paths. |
| `RECOMP_GFX_STATS=1` | Each draw's outcome: culled, clipped, failed stencil or depth, or written. |
| `RECOMP_DEBUG_DRAW=k` | One draw's uniforms and its shader inputs and outputs over a grid of pixels. `=all` with `RECOMP_DEBUG_PIXEL=x,y` lists every draw that touches that pixel: its history through the frame. |
| `RECOMP_TRACE_DRAW=k`, `RECOMP_USSE_TRACE=1` | Trace one draw's shader programs, or every instruction. |
| `RECOMP_REPLAY_SAVE_PROG=<hexaddr>:<file>` | Save a shader program from the capture. |
| `RECOMP_REPLAY_RAW=<file>` | The stopped draw's target bytes as the game sees them (images clamp HDR values). |
| `RECOMP_REPLAY_TEX=<hexaddr>:<file>`, `RECOMP_GPU_DUMP_TEX=<hexaddr>:<file>` | One texture as each backend decodes it. |
| `RECOMP_GPU_TRACE_SYNC=1` | Every target upload and download with its reason, every view built or refused, texture re-uploads, fallbacks, and the texture bound to each sampler. |
| `RECOMP_GPU_TRACE_ADDR=<hex>` | For a texture over that address: which render target or depth surface it resolves to, and every candidate. |

**Shader programs.** `runtime/debug/usse_dis.c` disassembles a saved program, and `runtime/debug/usse_run.c` runs one
fragment program alone with chosen inputs (`usse_run prog.gxp pa=k:r,g,b,a o=r,g,b,a saN=f [nosec]`). Vita3K's shader
translator is a good reference for instruction questions; it's GPL, so read it but never copy from it.

**Register probes.** `RECOMP_PROBE=t|p|o:<word>[@<instr>]` makes a fragment's colour show one register word (temporary,
input or output, after a given instruction) on both backends, so `RECOMP_REPLAY_DRAWS` dumps of a single draw compare
any intermediate value. An upper-case bank letter probes the vertex program instead, shown per triangle.
`RECOMP_PROBE_SCALE=s`, `RECOMP_PROBE_F32=1` and `RECOMP_PROBE_U32=1` change how the word is shown. This is how two
translator bugs were each pinned to a single instruction.

**Caveats.**

- A replay restores each scene's input memory from the run that recorded it. A fix to a pass that feeds a later scene
  only shows downstream with `RECOMP_REPLAY_KEEP_TARGETS=1`, or in a fresh capture.
- The software path draws a scene deferred, so it reads restored memory later than the GPU does. The two can
  legitimately differ on a scene whose inputs were drawn earlier in the same frame.
- Run one GPU replay at a time. Several at once can fail to get a device and quietly render on the software path; the
  backend says so on stderr.

## The GPU backend's switches

Most of these turn one optimisation back to the slower path, for an A/B comparison.

| Switch | What it does |
|---|---|
| `RECOMP_GFX=soft` | Render on the software reference instead of the GPU. |
| `RECOMP_GFX_CHECK=1` | Re-run every GPU draw on the software path from the same starting bytes and compare. |
| `RECOMP_GPU_NO_VIEWS=1` | Read render targets through guest memory instead of GPU views. |
| `RECOMP_GPU_VIEW_HOIST=0` | Don't move view passes to the front of the consuming scene's command buffer. |
| `RECOMP_GPU_VIEW_FAST=0` | Views as RGBA32F with one general shader, instead of narrow formats and per-view shaders. |
| `RECOMP_GPU_CPU_VFETCH=1` | Pack every vertex on the CPU instead of decoding attributes in the vertex shader. |
| `RECOMP_GPU_NO_INDEXED=1` | Draw without index buffers. |
| `RECOMP_GPU_NO_BC=1` | Decode BC-compressed textures on the CPU. The GPU's BC decoder rounds differently, by up to 2/255. |
| `RECOMP_GPU_NO_DEPTH_ONLY=1` | Draw depth-only scenes, such as shadow maps, on the software path. |
| `RECOMP_GPU_NO_POINTS=1` | Drop point-sprite draws instead of sending them to the software path. |
| `RECOMP_GPU_FETCH=copy\|fb` | How a shader that reads the current pixel colour (the Vita's programmable blending) gets it: `fb` reads tile memory directly, the default on Apple GPUs; `copy` copies the target before each such draw, the default elsewhere. `RECOMP_GPU_NO_FETCH=1` sends such draws to the software path. |
| `RECOMP_GPU_F16=masked\|exact\|native` | How half-precision values convert. `masked`, the default, uses the GPU's conversions kept bit-exact with the interpreter; `exact` is the slower software version; `native` rounds differently from the interpreter. |
| `RECOMP_GPU_READBACK_MAX=<pixels>` | Small render targets that only the CPU reads (default up to 4,096 pixels) are copied back asynchronously and land a frame later. 0 turns this off. `RECOMP_GPU_READBACK_AGE`, `RECOMP_GPU_READBACK_WAIT=1` and `RECOMP_GPU_READBACK_PREWAIT=0` adjust when they land and where the wait happens. |
| `RECOMP_GPU_RING_WAIT_ALL=1` | At a vertex or index ring wrap, wait for all GPU work, not just the work over the region being reused. |
| `RECOMP_GPU_DECODE_CHECK=1` | Check the fast texture decoders byte for byte against the generic one. `RECOMP_SOFT_PVRTC_SLOW=1` decodes PVRTC per texel. |
| `RECOMP_GPU_F32F32_RGBA=1` | Store two-channel float targets as four channels, the bandwidth A/B. |
| `RECOMP_GPU_NULL_FS=0` | Run a disabled fragment program behind a zero colour mask instead of an empty shader. |
| `RECOMP_GPU_HDR_RG11B10=1` | Store HDR lighting targets in a 4-byte format. Not usable: games read the alpha this format drops. |
| `RECOMP_GPU_NO_PRESENT=1` | Present through guest memory instead of from the GPU texture. |
| `RECOMP_SHADER_CACHE=<dir>\|0` | Where translated shaders are cached (default `shader_cache/` beside the game), or 0 for no cache. |
| `RECOMP_OSD_CPU=1` | Draw the F1 overlay on the CPU instead of with ImGui. |

**Texture changes** are found by write-protecting the guest pages under cached textures (`runtime/gfx/guestwatch.c`):
the first CPU write to one faults and marks the texture stale. A system call that writes guest memory, such as a file
read, must be wrapped in `gwatch_write_begin`/`gwatch_write_end`, or it fails with `EFAULT`.
`RECOMP_GPU_NO_TEX_WATCH=1` hashes every bound texture each frame instead, and `RECOMP_GPU_TEX_WATCH_VERIFY=1` does
both and reports any change the watch missed. Under lldb, use `process handle SIGBUS -s false -p true`.
