# Working on Nativ

This is the starting point for working on the code. It covers the pipeline that turns a game into a program, how
games, versions and licences are handled, and how to work across machines. The other guides go deeper:

- [How it works](architecture.md): the design, layer by layer.
- [The runtime in detail](runtime.md): display timing, audio, the GPU backend, controls, hooks and the rest.
- [Verification](verification.md): how we check the translation and the renderer are right.
- [Debugging a game](debugging.md): logs, captures, replays and the renderer's switches.
- [Online play](online.md), [the shader compiler](shader-compiler.md) and [the USSE instruction set](usse-isa.md).

## The pipeline

```
game package ──add──> decrypted game ──import──> ELF ──analyze──> functions ──recompile──> C + memory image
                                                                                               │
                                                       native program <──build── C + runtime ──┘
```

`./vitarecomp <stage> <title>` runs one stage; run it without arguments for the full list. Each stage writes into the
game's folder in your library (`dump/decrypted/<title>/` in a checkout):

| Stage | What it does |
|---|---|
| `firmware <PUP>` | Decrypts Sony's firmware update, whose libraries games load. |
| `scan <folder>` | Finds games, updates, add-ons and firmware files at any depth, as the launcher's Add dialog does. |
| `add <copy>` | Decrypts a game or update and keeps it, then runs the stages below when the game has a pack. |
| `import <title>` | Turns the game's executables into ELF files and identifies which release it is. |
| `analyze <title>` | Finds every function, in seconds (`recompiler/analyze.py`). |
| `recompile <title>` | Translates the functions to C and writes the memory image (`recomp.py`, `mkimage.py`). |
| `build <title>` | Compiles the C with the runtime into the game's program (`runtime/build.py`). |
| `run <title>` | Plays it, writing `session.log`. |
| `test <title>` | Runs it headless to a set frame and saves a picture. |
| `check <titles>` | The regression gate: tests each game and compares the picture with its reference. |
| `capture <title> <frame>` | Records a frame's graphics for replaying. |
| `dist <folder>` | Writes an install containing only what ships. |

`build` takes a variant: `verify` (the in-place checker) or `profile` (the profiler). `RECOMP_BIN=<name>` builds a
second program beside the play one (`game_<name>`, tested in `test_<name>/`), so experiments never touch the binary
you play.

## Packs, versions and updates

Each game's pack, `packs/<title>/pack.toml`, holds what's specific to it: its releases, the Sony libraries it loads,
settings, a scripted test, and code hooks. The format is defined in `pipeline/packconf.py`.

A pack names the releases it supports by the SHA-256 of the main program, each marked `recommended` or `supported`.
`import` recognises a copy's release by that hash, refuses one it doesn't know, and records it in
`dump/decrypted/<title>/version`. Settings that differ per release, such as hook addresses, sit in a section per
version: Killzone 1.00 and 1.12 share one pack this way. `vitarecomp versions <title>` lists a pack's releases and says
which one your copy is.

A game's update is kept beside the game (`patch0`) and decrypted with the game's own licence. When the pack plays the
updated release, the update is layered over the game as hard links (`app0_patched`).

## Licences

Licences belong to the player and are kept once, in `dump/licenses/<content id>.rif` (`vitarecomp licenses`,
`pipeline/licenses.py`). They can come from licence files (a `.rif` or `work.bin`), the Vita itself over VitaShell's
FTP, or a Vita3K install. `add` looks a copy's licence up by its content ID, so later packages, updates and add-ons of
the same game add without asking. Only NoNpDrm-style licences decrypt away from the console; a PSN licence as the Vita
keeps it is bound to that console. Licences are never shared between players.

## Working on more than one machine

GitHub is the hub.

- `main` is the line that passed the regression gate and plays.
- Work happens on topic branches pushed to GitHub, and merges into `main` once the gate passes.
- Every machine has its own checkout and its own library. The library (`dump/`) never goes into git.
- Code moves between machines only through GitHub.

Two scripts support this:

- `dev/build-all.sh [title ...]` brings a machine's play builds up to date. On a clean `main` it pulls first. It builds
  the launcher, then every game in the library that has a pack, regenerating a game's code first whenever
  `recompiler/` or its `pack.toml` changed. It skips any game that's running.
- `dev/remote.sh <host> <command> <title>` runs a vitarecomp command on another machine. It refuses unless that
  checkout is clean and at the same commit as this one; `REMOTE_RSYNC=1` copies this checkout's files over it instead,
  discarding its changes.

Settings that belong to your own machines go in `dev/local.env`. Git ignores it, and the scripts read it when it's
there:

```sh
NATIV_REMOTE=mybox        # online_lan.sh's second machine, an ssh host
NATIV_REMOTE_DIR=nativ    # the checkout there, under its home (default project-nativ)
```

A pack's new hooks only take effect in regenerated code, which is why `build-all.sh` regenerates when a pack changes.

The Vulkan versions of the renderer's built-in shaders live in `runtime/gfx/vk_shaders.h`, generated by
`runtime/gfx/shaders/build.sh` from the GLSL beside it. It needs `glslc`, from the Vulkan SDK or shaderc. The Metal versions
are MSL inside `gfx_gpu.c` and `post.c`; change both sides together.

## Making a release

A release is three downloads, one per OS, each the whole install: the tools (`vitarecomp dist`), the toolchain a game
build needs, a standalone Python and the launcher. On a Mac that is one app, Project Nativ.app, in a `.dmg` beside a
link to Applications (the tools in its `Contents/Resources`, the launcher as its program); on Windows and Linux a zip
holding a `ProjectNativ` folder. `dev/macos-install.sh`,
`dev/windows-install.sh` (made on macOS or Linux) and `dev/linux-install.sh` (made on Linux) build them, each from a
toolchain built first by `runtime/platform/<os>/setup-toolchain.sh`. Every download those scripts make is pinned by
its SHA-256.

1. Tag the commit (`git tag v0.1.0`) and check it out with nothing uncommitted.
2. On the Mac: `NATIV_RELEASES=<owner>/<repo> dev/release.sh v0.1.0 mac windows`; on Linux the same with `linux`.
   Copy the Linux zip into the Mac's `build/release/v0.1.0/` and run `dev/release.sh v0.1.0` there once more to write
   `SHA256SUMS` over all three.
3. Publish: the script prints the `gh release create` command.

`NATIV_RELEASES` names the GitHub repository whose releases are the installs' updates: `vitarecomp dist` writes it to
`updates.txt`, and the launcher runs `vitarecomp update-check` at start (GitHub asked at most once a day), which shows a
newer release's tag above the library. Versions compare as `vX.Y.Z`; a build that isn't a release is never told.

Signing needs your own accounts, set in the ignored `dev/local.env`: `NATIV_SIGN_ID` (a Developer ID Application
identity) signs every program and library in the Mac app, the app and the `.dmg`, with the hardened runtime;
`NATIV_NOTARY_PROFILE` (a `notarytool` profile) notarises the app and then the `.dmg`, stapling both, so a player's
Mac opens it with one "downloaded from the internet" question. `NATIV_SIGN_CMD` signs the Windows launcher. Nothing
writes inside the app once it's made (a signed app must match its signature): builds go to the library and compiled
Python to the settings folder.

## House rules

- Run the gate (`./vitarecomp check PCSE00640 PCSA00014 PCSA00029`, about 18 minutes) after any change to the runtime
  or the recompiler, and don't edit sources while it builds.
- Never commit anything from a game or the firmware. Everything generated stays in the library.
- Never rebuild a game while it's running: relinking a running program can crash it.
