# Contribution TODO

- [x] Preserve LR for non-linking branches across emitted functions. SETJMP and
  external-target paths validated and behavioral regression coverage added
  (recompiler/tests/fixture_branch_lr.py: emitted-text + executed synthetic test --
  33/33 checks pass on the fixed emitter, 11 failures on the base upstream/main
  emitter, exact outputs in the change description). Publication review and the
  upstream PR submission follow as the next step.
