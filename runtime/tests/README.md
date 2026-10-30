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

## Controller layout

`controller_layout_test.c` checks the pack-default parser and the Automatic choice for controller, keyboard and mouse,
headless, and no-gamepad runs:

    cc -O1 -Wall controller_layout_test.c -o /tmp/controller_layout_test && /tmp/controller_layout_test

It prints `controller layout: 15 checks, 0 failed`.

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
