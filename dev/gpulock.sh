#!/bin/zsh
# Run a command holding the machine-wide GPU lock: `gpulock.sh build/gfxreplay cap.vfc out.ppm`.
# Two GPU replays at once make one of them silently render on the software path (the device
# creation fails and the result looks like a byte-identical "GPU" frame); every GPU-backed run
# from scripts and agents goes through this.
LOCK=/tmp/vita-gpu.lock
while ! mkdir $LOCK 2>/dev/null; do
	# a lock older than 10 minutes is a crashed holder
	if [[ -n "$(find $LOCK -maxdepth 0 -mmin +10 2>/dev/null)" ]]; then rmdir $LOCK 2>/dev/null; continue; fi
	sleep 1
done
trap 'rmdir $LOCK 2>/dev/null' EXIT INT TERM
"$@"
