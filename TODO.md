# Contribution TODO

- [x] Implement the `sceKernel` timer imports (CreateTimer/StartTimer/StopTimer/
  GetTimerTime/GetTimerTimeWide) and cover them with a behavioral test. The
  pre-fix contract is mirrored by the test's `-DTIMER_STUB_CONTROL` mode
  (30 checks, 14 failed) and the implementation passes all 30 (plus the
  kernel_test gate, 681 checks). Publication review and the upstream PR
  submission follow as the next step.
