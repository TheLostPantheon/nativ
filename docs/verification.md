# Verification

Nativ's promise is that a recompiled game behaves exactly like the original. That's only worth something if it's
checked, so every layer has a way to compare our result with a reference: the original instructions for the
translated code, the firmware's behaviour for the runtime, and the software renderer for the GPU.

## The regression gate

```
./vitarecomp check PCSE00640 PCSA00014 PCSA00029
```

The gate rebuilds each game, plays it headless to a set frame on the fixed clock, and compares the picture with the
game's stored reference. A frame that differs by more than the pack's tolerance fails. The three games take about 18
minutes. Run it after every change to the runtime or the recompiler, and don't edit sources while it builds.

- **References.** `./vitarecomp accept <title>` makes the last test's frame the game's reference.
- **The software renderer.** The tests use the software path, so references stay exact; GPU parity is checked
  separately (below).
- **Noise.** Unit 13's frame sometimes drifts by about 1.6% even with no change. Before accepting a new reference after
  a failure, rerun the test two or three times: identical reruns mean the old reference stands.
- **On another machine.** `dev/remote.sh <host> check <titles>` runs the gate there once that checkout is at the same
  commit, and reports the PASS and FAIL lines as they land.

Killzone isn't in the gate yet, so test it separately after host changes.

## The tests that need no game

```
dev/tests/run_all.sh [quick]
```

These run in CI (`.github/workflows/tests.yml`, macOS and Linux) on every push to main and every pull request, and
before a push here:

- **`dev/tests/recomp_exec_test.py`:** the recompiler end to end. C programs (`dev/tests/recomp_exec/progs.c`) are
  compiled for the Vita's CPU by clang, as Thumb and as ARM, then recompiled and run on random inputs. Each result must
  match Unicorn running the original code. It covers calls, recursion, indirect calls, switch tables, IT blocks, 64-bit
  arithmetic, every load and store width, struct copies, VFP and NEON. It runs the generated code both as built and with
  `--no-reg-locals` (registers, flags and float registers in memory), so a change to how functions are emitted is
  checked against the plain translation and the CPU itself.
- `pipeline/packconf.py --check` and `pipeline/packs.py selftest`.
- The kernel, touch gesture and JPEG tests in `runtime/tests/`.

## Measuring performance

```
dev/bench.sh <title> <recording folder> [frames] [from]
```

This replays a recorded session (input.txt and savedata0, recorded with `RECOMP_INPUT_RECORD` on the fixed clock)
headless. From frame `<from>` it runs as fast as the game can (`RECOMP_UNPACED`), and reports frames per second over
each 300 frames plus each thread's CPU time. Start it after the recording's loads: unpaced loading screens race ahead
of the recorded input. `REPEAT=n` gives the median of n runs after a warm-up.

To compare two builds, interleave their runs (A, B, A, B...) on a quiet machine, with nothing else building. Uncharted's
replay is deterministic. Killzone's isn't: its runs drift apart in what they show, and so does their frame rate.

`vitarecomp pgo <title> <recording>` builds a game with a profile from a recorded session (about +7% on Uncharted).

## Checking the translated code

**`dev/difftest.py`** runs each function against Unicorn, an ARM emulator, from random starting states. It's fast, but
random memory can't exercise code that walks real objects.

**The in-place checker** compares against the original while the game plays.

```
./vitarecomp build <title> verify
./vitarecomp verify <title>
```

Sampled calls are re-run from the game's real state, in copy-on-write forks: once as the original instructions under
Unicorn, once as our translation. Registers, floating-point state, where the call stops and every byte it writes must
match. A `PROFILE=1` build replays a mismatch in lockstep and names the first instruction that differs. This found the
bugs that blocked Shovel Knight's boot within minutes, and later 310 NEON conversions translated as scalar ones.

- Calls into Sony's libraries are followed on both sides; only a call into the host ends a comparison
  (`RECOMP_VERIFY_FOLLOW=0` stops at every import instead).
- After each function's first `RECOMP_VERIFY=k` calls, the checker keeps sampling: calls whose count is a power of two,
  or every `RECOMP_VERIFY_EVERY`-th, within a `RECOMP_VERIFY_RATE` budget of checks a second. Playing in a verify
  build checks whatever you happen to do, which matters: a mistake that depends on the data can pass thousands of
  first-call checks.
- `RECOMP_VERIFY_REPORT=<file>` writes a line per checked function: calls, checks, passes and the first mismatch.
- `RECOMP_VERIFY_RANGE=lo-hi` checks one address range in depth, such as a codec.

**Snapshot execution** (a verify build) loads one moment of play into memory and calls any function from it, original
against translated: `RECOMP_SNAP=<memory dump>` with `RECOMP_SNAP_CALLS=addr:regs[;...]`. `RECOMP_SNAP_COMMIT` and
`RECOMP_SNAP_REPEAT` chain calls with their state, and `RECOMP_SNAP_TIME` drives both sides' clocks. It's
deterministic and needs no replay; it found the mistranslated rotation that left Unit 13's NPCs standing still.

**`dev/insnsweep.py <analysis.data.json> <generated .c>`** collects every distinct instruction form in a module, with
registers named by first appearance so aliasing counts as its own form, and runs a few of each through the translation
and Unicorn from random states. Forms whose Unicorn run crashes (its NEON support is broken on Apple Silicon) are
reported, not dropped.

**`dev/tests/neon_test.py`** checks NEON and VFP lane behaviour against a Python model written from the ARM manual,
not Unicorn. Each case is a short Thumb-2 sequence: clang assembles it, the recompiler translates it, and a harness
runs the C over random states with edge values mixed in (saturation bounds, shift counts, denormals, NaNs). It covers
171 forms. Run it after any change to the NEON or VFP translation or the lane helpers in `rt.h`.

## Checking the runtime

`runtime/tests/` holds tests of the runtime itself, run on host threads. `kernel_test.c` asserts the kernel's
synchronisation semantics, such as who wakes when and with which error, against the firmware's behaviour. The
[tests README](../runtime/tests/README.md) has the build lines.

## Checking the graphics

- **Coverage.** `RECOMP_GFX_COVERAGE` (read with `dev/gfxcov.py`) lists what a game's graphics ask for against what
  each backend supports, including libgxm state the decoder doesn't read yet.
- **Captures.** F12 in the window, or `./vitarecomp capture <title> <frame>`, records a frame: the graphics stream plus
  the memory it reads. `build/gfxreplay` plays it through either backend in seconds. [Debugging a
  game](debugging.md#graphics-off-the-game) covers replay options.
- **GPU against software.** `dev/gfxbisect.py <capture> --interp --threshold N` finds the first draw whose target
  differs between the GPU and the interpreter by more than N%. `RECOMP_GFX_CHECK=1` re-runs every GPU draw on the
  software path from the same starting bytes.
- **Parity in a live run.** A capture restores memory from the run that recorded it, so it can hide differences in
  what earlier scenes produced. The check without that gap is the test harness itself:
  `EXTRA_ENV="RECOMP_GFX=gpu" ./vitarecomp test <title>`, then `dev/framediff.py` against the reference.
- **Replay sweeps.** After a renderer change, replay a set of captures with the old and new `gfxreplay` and compare
  the pictures. Unchanged paths should come out byte-identical.
