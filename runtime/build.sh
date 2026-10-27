#!/bin/zsh
# Build the native runtime for a recompiled module: build.sh <recomp base> <out binary> [<module base>...] -- build.py beside
# this, the same build on macOS, Linux and Windows (TARGET=windows, JOBS, VERIFY, PROFILE: see there), run on the repository's Python.
H=${0:A:h}; PY=${H:h}/.venv/bin/python; [[ -x $PY ]] || PY=python3
exec $PY $H/build.py "$@"
