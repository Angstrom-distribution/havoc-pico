#!/bin/sh
# All tests of the picture code, in the container:  sh tests/run.sh
# FUZZ_SECONDS sets the length of each fuzzer run (default 60).
here=$(cd "$(dirname "$0")" && pwd)
rc=0
for t in run-tsm.sh run-sixel.sh run-kitty.sh run-kplace.sh run-c8.sh run-gfx.sh; do
	[ -f "$here/$t" ] || continue
	echo "=== $t"
	sh "$here/$t" || { echo "FAILED: $t"; rc=1; }
done
exit $rc
