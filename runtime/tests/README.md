# Runtime tests

These tests drive parts of the runtime directly on the host, with no game and no SDL window, and check them against
the firmware's behaviour. Run each one from this folder. They all share the runtime's include path:

    INC="-I ../core -I ../gfx -I ../audio -I ../media -I ../input -I ../net -I ../ui -I ../platform -I ../debug -I ../../recompiler -I ../.."

## The kernel: `kernel_test.c`

Checks `runtime/core/kernel.c`, the thread manager, against the firmware's semantics and error codes. It covers:

- **Synchronisation:** threads, semaphores, mutexes, lightweight mutexes and conditions, kernel conditions, event
  flags, simple events, SceUlt objects, reader-writer locks and message pipes.
- **Waking rules:**
  - several waiters on one event flag, each with its own mask and clear mode;
  - a condition wait releasing and re-taking its mutex with the same recursion count;
  - Signal, SignalAll and SignalTo;
  - semaphore queue order and overflow;
  - timeouts returning `SCE_KERNEL_ERROR_WAIT_TIMEOUT` with the remaining time written back;
  - deletion waking waiters with `WAIT_DELETE`.
- **Threads:** info, priority, exit status and thread event handlers.
- **Reader-writer locks:** recursive reads, writer preference, and the order an unlock grants in.
- **Timers:** start, stop, set and read on a hand-set guest clock (the count continues across stop and start, the wide forms use r0:r1), the info structure, and timer events (SetTimerEvent with SET and PULSE types, repeat, Cancel, remaining time, threads waiting with WaitEvent on the timer's uid). Also thread restart: an ended thread started again.
- **Message pipes:** ASAP and FULL transfers, sender order, and `WAIT_CANCEL` from Cancel.
- **Callbacks:** one notified while its owner sleeps in `WaitSemaCB`, `DelayThreadCB` or `WaitThreadEndCB` runs on the
  owner mid-wait with its arguments, and the wait carries on. Also `CheckCallback`, and a callback deleting itself by
  returning non-zero.
- **Fibers:** running, returning to and being resumed by their thread with arguments passed both ways, switching
  between fibers, and a fiber without a context restarting each run.

A final section switches on the Vita scheduler (`RECOMP_SCHED=vita`) and runs real guest threads:

- on one core, a higher-priority thread preempts its starter at the starter's next system call, and the starter
  resumes only once the new thread blocks;
- three equal-priority spinners all make progress by round robin, never two at once;
- a blocked high-priority thread lets a low-priority one run, and a signal from it hands the core back at its next
  system call;
- `ChangeThreadPriority` reorders waiters and preempts at once;
- nothing deadlocks when every thread sleeps;
- the timer evicts a spinner that makes no system calls;
- on two cores, a new thread preempts the lowest-priority holder, and affinity masks pin threads to cores.

A hang in that section fails after 30 seconds with every thread's scheduler state. The test includes `kernel.c` and
supplies the pieces it needs from `main.c` and `display.c`: a 4 GiB guest memory block, the allocator and the
function lookup.

    cc -O1 -g -Wall -Wno-unused-function $INC kernel_test.c -o /tmp/kernel_test -lpthread && /tmp/kernel_test

It prints `scheduler (RECOMP_SCHED=vita): N checks, 0 failed`, then `N checks, 0 failed`, and exits 0. A failed check
names its line. Waiter tests use short sleeps to order threads, so a run takes about three seconds.

## Touch gestures: `gesture_test.c`

Checks `runtime/input/gesture.c`, the Vita's gesture library (SceSystemGesture): its touch tracker and its tap, drag,
hold, pinch and rotation recognisers.

1. **Scripted taps**, as a headless run makes them: a front-panel touch held over a range of frames, fed once a frame
   and also as 64 identical copies a frame, the way Killzone reads them. Each tap must give exactly one TAP, and each
   touch one DRAG BEGIN and one END. If two touches share an ID, the second drag must not begin; that's the firmware's
   rule, and why `display.c` gives each contact its own ID.
2. **The console's own scenario** (`gesture_scenario.h`) runs through our handlers, and its trace is compared record
   by record with one the firmware produced on a real Vita. That `firmware.trace` stays on your machine, since no
   firmware output belongs in the repository. Without it, only the scripted taps run.

```
cc -O1 -g -Wall -Wno-unused-function $INC gesture_test.c -o /tmp/gesture_test -lm -lpthread \
  && /tmp/gesture_test [path/to/firmware.trace]
```

It prints `scripted taps: N checks, 0 failed` and `console trace: 684 records, 0 differ`.

## System dialogs: `commondlg_test.c`

Checks the message dialog and the text-input dialog in `runtime/core/apputil.c` with parameter blocks built in guest
memory the way a game builds them: each message mode and button type, the system messages, the progress bar calls,
the text dialog's title, limit, initial text and flags, and the errors the console gives for bad parameters. It checks
the states a game sees (RUNNING until answered, NOT_FINISHED from `GetResult`, FINISHED after, NONE after `Term`),
answers to a dialog that has gone, `Close` and `Abort`, and the headless auto-answer (including `RECOMP_IME_TEXT`).
It also checks the UTF-16 conversion in `commondlg.h`: surrogate pairs, the length limit never splitting one, and
malformed input, and `sceAppUtilAppParamGetInt`'s SKU flag (a full game's). Drawing and key handling need a window and
aren't covered. The test includes `apputil.c`, so it
builds with `-I stub`, a stand-in for SDL's header.

    cc -O1 -g -Wall -Wno-unused-function $INC -I stub commondlg_test.c -o /tmp/commondlg_test -lpthread && /tmp/commondlg_test

It prints `N checks, 0 failed`.

## JPEG: `jpeg_test.c`

Checks `runtime/media/jpeg.c` (SceJpeg and SceJpegEnc) the way Uncharted's photo minigame uses it: a 960x544 picture
encoded, then decoded twice, once as YCbCr at quarter size (the 240x136 thumbnail) and once as RGBA, checking the
pitches and the colours that come back. There's no console reference yet; the layout follows Vita3K's MJPEG module.

    cc -O1 -g -Wall -Wno-unused-function $INC jpeg_test.c -o /tmp/jpeg_test && /tmp/jpeg_test

It prints `all passed`.

## The camera: `camera_test.c`

Checks `runtime/media/camera.c` (SceCamera) the way Uncharted's hold-it-to-the-light parchment puzzles use it: the
rear camera opened with YUV420 planes at 640x480, and frames read with the lamp off and on (F7), checking the planes
come out dark and then bright, along with the frame index and status. Then buffers given per read, and a 32-bit ABGR
format. The behaviour follows Vita3K's SceCamera module; there's no console reference.

    cc -O1 -g -Wall -Wno-unused-function $INC $(pkg-config --cflags --libs sdl3) camera_test.c -o /tmp/camera_test && /tmp/camera_test

It prints `all passed`.

## Metal's scratch folders: `mtl_archive_test.m` (macOS)

Checks the sweep in `runtime/gfx/mtl_archive.m`. Metal gives every binary archive a pipeline is added to a scratch
folder (`com.apple.gpuarchiver/gpuarchiver-<hex>` in the user's cache folder) and never removes it; the archive keeps
one per shader library, so a week of test runs once left 43 GB of them. The sweep removes the ones untouched for 20
minutes. The test checks, in a temporary folder, that only idle `gpuarchiver-*` folders go. Then, with Metal itself, it
checks the two facts the sweep relies on: the folder is where the sweep looks, and a written archive (and its file)
still finds its pipelines once the folder is gone. A macOS update that changes either fails here.

    clang -O1 -g -Wall -I ../gfx mtl_archive_test.m -framework Metal -framework Foundation -o /tmp/mtl_archive_test && /tmp/mtl_archive_test

It prints `all passed`.

## NGS voice rules: `ngs_rules_test.c`

Checks `runtime/audio/ngs_rules.h`, three small rules of the NGS voices: the start buffer of a player is the signed byte at
+81 of its parameters (Vita3K's layout; +80 is reserved); a player buffer with no whole frame in it is passed over to its
next (FMOD's hardware streams start on an empty buffer 0, with the stream's ring in buffer 1), while a slot with no data
address, a missing next or a cycle of empty buffers ends the voice; and `sceNgsVoicePlay` starts a voice that is at rest
but leaves a playing one alone and answers `SCE_NGS_ERROR`, so a game that calls Play on its music voice every update does
not restart it each time. The voices the runtime runs from their creation (master, reverb, buses) count as at rest until
the game's own first Play.

    cc -O1 -g -Wall $INC ngs_rules_test.c -o /tmp/ngs_rules_test && /tmp/ngs_rules_test

It prints `all N passed`.

## Frame generation: `framegen_test.c`

Runs `runtime/gfx/framegen` on the GPU without a window (Metal through SPIRV-Cross, or Vulkan). A textured scene panned
by five amounts under a still, text-like HUD: the frame made halfway against the scene panned by half (beside a plain
blend's error) and the HUD against itself. Then a dark object over a bright background at 2880x1632 (a quarter working
size, as at render scale 3), the object still, moving, or moving against the background: the error in a band around its
edges and how much of that band came out much brighter (a light outline). A thin object moving further than its own
width is checked loosely: it used to vanish from the made frames, and the forward search now keeps most of it. `framegen_test prev.png
cur.png out.png [t]` makes the frame at t between two real frames, and `--time w h` measures the GPU time per frame.

    ./framegen_test.sh

It prints `all passed`.

## The microphone: `audioin_test.c`

Checks `runtime/audio/audioin.c`, the silent microphone: a port of each type opens, a second one of a type and an
unknown type or size are refused with the console's errors, an input hands back exactly its grain of silence, ten
inputs take ten buffers' time on the host clock, and under the fixed clock the wait goes to the guest clock's pacing
instead. Release, a port that isn't open or isn't one, and a null buffer are checked too.

    cc -O1 -g -Wall $INC audioin_test.c -o /tmp/audioin_test && /tmp/audioin_test

It prints `all passed`.

## The network offline: `net_offline_test.c`

Checks `runtime/net/net.c` with online play off: the network a Vita has with its Wi-Fi off. The byte-order and address
helpers work; netctl says disconnected and has no address to give; the resolver has no DNS; a socket bound to "any
address" is bound to this machine's loopback, never a port on the player's network; a datagram to the internet or the
LAN, and a connection anywhere off this machine, fail with ENETUNREACH; and a datagram between two sockets here is
delivered. `runtime/net/http.c` too: a template, connection and request are made, and sending it fails as an unreachable
network without asking any server. It uses real loopback sockets.

    cc -O1 -g -w $INC net_offline_test.c -o /tmp/net_offline_test -lpthread && /tmp/net_offline_test

It prints `all passed`.

## PSN comparisons and offline state: `np_test.c`

Checks the parts of `runtime/net/np.c` every game gets. SceNpCommon's comparisons, as the firmware's `np_common` has
them: the same player, one id without a platform (still the same player), another online id, platform or option bytes
(not), an id the system didn't make, a null argument, and the ordered comparison's -1, 0 and 1 (an id without a platform
counts as "psp2"). And offline PSN: the service state reads signed out, and a registered state callback hears it once.

    cc -O1 -g -w $INC np_test.c -o /tmp/np_test -lpthread && /tmp/np_test

It prints `all passed`.

## Trophies: `trophy_test.c`

Checks `runtime/core/trophy.c`, the game's trophies kept on this machine. It writes a small trophy pack into a temporary
folder (a platinum, a bronze and a hidden gold counting towards it, an add-on group's trophy that doesn't, the list in
English and French, an icon) and checks the game's, a group's and a trophy's details at the firmware's structure sizes,
an unlock with its toast, a second unlock of the same trophy, the platinum refused directly and unlocking itself with
the last trophy that counts towards it, the unlock state's flags and count, the unlocks read back by a new context, the
game language's list, an icon by size and then bytes, a copy without its pack (the context made all the same, any id
unlocking and kept), and the errors for a wrong size, trophy or context.

    cc -O1 -g -Wall $INC trophy_test.c -o /tmp/trophy_test -lpthread && /tmp/trophy_test

It prints `all passed`.

## The audio decoder: `audiodec_test.c`

Checks `runtime/audio/audiodec.c` (SceAudiodec), driven through guest-memory `SceAudiodecCtrl` and `SceAudiodecInfo`
structures as a game does. Always: the init, create, delete and clear calls and their error codes for bad parameters,
the ATRAC9 info filled in from a configuration word, and a hand-built silent ATRAC9 frame through Decode (the bytes a
frame takes, the superframe's padding on its last frame, the PCM size). LibAtrac9 has no encoder, so ATRAC9 has no
reference audio. With `-DRECOMP_AVCODEC` and libavcodec's development files, also the AAC (ADTS and raw frames) and MP3
decoders: `audiodec/tone.aac` and `audiodec/tone.mp3` are 0.14 s sine tones made with the ffmpeg command line, and
each Decode's PCM is compared with libavcodec decoding the same frames directly. Without them the test says it skips
AAC and MP3 and checks that they fail with INVALID_TYPE; `dev/tests/run_all.sh` adds the flag when `pkg-config` finds
libavcodec.

    cc -O1 -g -w $INC audiodec_test.c ../../third_party/libatrac9/*.c -lm -lpthread -o /tmp/audiodec_test [-DRECOMP_AVCODEC $(pkg-config --cflags --libs libavcodec libavutil)] && /tmp/audiodec_test

It prints `all passed`.
