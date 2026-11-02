#!/bin/sh
# Sixel decoder tests: built-in cases, every vector against the reference
# decoder, the synthetic vectors, then a fuzzing run.
#   sh tests/run-sixel.sh
#   REGEN=1 FUZZ_SECONDS=10 sh tests/run-sixel.sh
# Needs gcc, clang with libFuzzer (libclang-rt-dev) and python3 with PIL.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
t=$root/tests
v=$t/vectors/sixel
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

[ -z "$REGEN" ] || sh "$v/regen.sh"

CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
$CC -g -O1 -Wall -Wextra -Werror -c $SAN -o "$tmp/sixel.o" "$root/sixel.c"
$CC -g -O1 -Wall -Wextra $SAN -o "$tmp/test-sixel" "$t/test-sixel.c" "$tmp/sixel.o"

$CC -g -O1 -Wall -Wextra $SAN -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc \
	-o "$tmp/test-sixel-oom" "$t/test-sixel-oom.c" "$tmp/sixel.o"

echo "== built-in cases"
"$tmp/test-sixel"

echo "== failed allocations"
"$tmp/test-sixel-oom"

echo "== vectors against the reference"
python3 -I "$t/tools/mksynth.py" "$tmp/synthetic.bin"
fail=0
for f in "$v"/*.bin "$tmp/synthetic.bin"; do
	name=$(basename "$f" .bin)
	for mw in 4096 100; do
		python3 -I "$t/tools/sixel_ref.py" "$f" --json "$tmp/ref.json" --max-w $mw >/dev/null
		"$tmp/test-sixel" "$f" "$tmp/dec.json" $mw
		if python3 -I "$t/tools/sixel_cmp.py" "$tmp/ref.json" "$tmp/dec.json" > "$tmp/cmp.txt"; then
			echo "ok   $name max_w=$mw: $(cat "$tmp/cmp.txt")"
		else
			echo "FAIL $name max_w=$mw"; cat "$tmp/cmp.txt"; fail=1
		fi
		rm -f "$tmp"/ref.json* "$tmp"/dec.json*
	done
done
[ $fail = 0 ] || { echo "FAILED: vectors differ"; exit 1; }

secs=${FUZZ_SECONDS:-60}
if [ "$secs" -gt 0 ]; then
	echo "== fuzzing $secs s"
	clang -g -O1 -Wall -Wextra -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
		-o "$tmp/fuzz-sixel" "$t/fuzz-sixel.c" "$root/sixel.c"
	mkdir "$tmp/corpus"
	python3 -I "$t/tools/sixel_seeds.py" "$tmp/corpus" "$v"/*.bin "$tmp/synthetic.bin"
	# a finding stops the fuzzer with an error: keep its input here
	if (cd "$tmp" && ./fuzz-sixel -max_total_time="$secs" -max_len=65536 corpus >fuzz.log 2>&1); then
		tail -n 4 "$tmp/fuzz.log"
	else
		tail -n 40 "$tmp/fuzz.log"
		cp "$tmp"/crash-* "$tmp"/oom-* "$tmp"/timeout-* . 2>/dev/null
		echo "FAILED: fuzz-sixel"
		exit 1
	fi
fi
echo "PASSED"
