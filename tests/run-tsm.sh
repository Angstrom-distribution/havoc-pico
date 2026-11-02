#!/bin/sh
# Build and run the libtsm tests with AddressSanitizer and UBSan.
# Run it in the container, from anywhere:  sh tests/run-tsm.sh
#
# PICO_TSM    tsm/ directory of the unchanged pico branch, for test-vte-same
#             (default /s/havoc-pico-work/plan-baseline/tsm)
# VECTORS     directory with captured terminal streams for test-vte-same
# FUZZ_SECONDS  length of the fuzzer run, 0 to skip it (default 60)

top=$(cd "$(dirname "$0")/.." && pwd)
PICO_TSM=${PICO_TSM:-/s/havoc-pico-work/plan-baseline/tsm}
FUZZ_SECONDS=${FUZZ_SECONDS:-60}
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT

checks=0
failed=0
ok_build=1

SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
CFLAGS="-g -O1 -Wall -Wextra -Wno-unused-parameter $SAN -D_XOPEN_SOURCE=700 $(pkg-config --cflags xkbcommon) -I$top"
export ASAN_OPTIONS=detect_leaks=1

fail() {
	echo "run-tsm: FAIL: $*" >&2
	failed=$((failed + 1))
}

# libtsm objects for HAVOC_SIXEL=$1 into $tmp/o$1
build_lib() {
	mkdir -p "$tmp/o$1"
	for f in "$top"/tsm/*.c; do
		gcc $CFLAGS -DHAVOC_SIXEL=$1 -c -o "$tmp/o$1/$(basename "$f" .c).o" "$f" 2>"$tmp/cc.log" ||
			{ cat "$tmp/cc.log" >&2; fail "compile $f"; ok_build=0; }
		# the six upstream warnings stay; any other one fails
		grep "warning:" "$tmp/cc.log" |
			grep -v "/tsm/wcwidth\.c:.*\[-Wparentheses\]" |
			grep -v "/tsm/tsm-vte\.c:.*\[-W\(sign-compare\|maybe-uninitialized\|format-overflow=\)\]" \
			>"$tmp/newwarn.log"
		if [ -s "$tmp/newwarn.log" ]; then
			cat "$tmp/newwarn.log" >&2
			fail "new warnings in $f HAVOC_SIXEL=$1"
		fi
	done
}

run_test() { # name defs...
	name=$1
	shift
	d=$1
	if ! gcc $CFLAGS -Werror -DHAVOC_SIXEL=$d -o "$tmp/$name-$d" "$top/tests/$name.c" "$tmp/o$d"/*.o; then
		fail "build $name HAVOC_SIXEL=$d"
		return
	fi
	"$tmp/$name-$d" >"$tmp/out.log" 2>&1
	rc=$?
	last=$(grep "checks," "$tmp/out.log" | tail -1)
	echo "$name HAVOC_SIXEL=$d: $last"
	n=$(echo "$last" | sed -n 's/.*: \([0-9]*\) checks.*/\1/p')
	checks=$((checks + ${n:-0}))
	if [ $rc -ne 0 ]; then
		grep -v "^unhandled\|^unknown\|^OSC" "$tmp/out.log" | tail -20 >&2
		fail "$name HAVOC_SIXEL=$d exit $rc"
	fi
}

for d in 0 1; do
	build_lib $d
done
[ $ok_build = 1 ] || exit 1

for t in test-vte-str test-vte-reply test-screen-event; do
	for d in 0 1; do
		run_test $t $d
	done
done
run_test test-screen-pic 1

# the output with no ops and no graphics must be the one of pico
mkdir -p "$tmp/pico"
for f in "$PICO_TSM"/*.c; do
	gcc -g -O1 -w $SAN -D_XOPEN_SOURCE=700 $(pkg-config --cflags xkbcommon) -I"$(dirname "$PICO_TSM")" \
		-c -o "$tmp/pico/$(basename "$f" .c).o" "$f" || fail "compile pico $f"
done
gcc -g -O1 -Wall $SAN -D_XOPEN_SOURCE=700 -I"$(dirname "$PICO_TSM")" -o "$tmp/same-pico" \
	"$top/tests/test-vte-same.c" "$tmp/pico"/*.o || fail "build test-vte-same for pico"
"$tmp/same-pico" >"$tmp/same-pico.out" 2>"$tmp/same.err"
grep "^test-vte-same" "$tmp/same.err"
for d in 0 1; do
	gcc -g -O1 -Wall -Werror $SAN -D_XOPEN_SOURCE=700 -DHAVOC_SIXEL=$d -I"$top" \
		-o "$tmp/same-$d" "$top/tests/test-vte-same.c" "$tmp/o$d"/*.o || fail "build test-vte-same $d"
	"$tmp/same-$d" >"$tmp/same-$d.out" 2>/dev/null
	checks=$((checks + 1))
	if cmp -s "$tmp/same-pico.out" "$tmp/same-$d.out"; then
		echo "test-vte-same HAVOC_SIXEL=$d: identical to pico ($(wc -l <"$tmp/same-pico.out") lines)"
	else
		fail "test-vte-same HAVOC_SIXEL=$d differs from pico"
		diff "$tmp/same-pico.out" "$tmp/same-$d.out" | head -10 >&2
	fi
done

# fuzzer, when clang can link libFuzzer
if [ "$FUZZ_SECONDS" -gt 0 ] && command -v clang >/dev/null; then
	for d in 0 1; do
		if clang -g -O1 -w -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
			-D_XOPEN_SOURCE=700 -DHAVOC_SIXEL=$d $(pkg-config --cflags xkbcommon) -I"$top" \
			-o "$tmp/fuzz-$d" "$top/tests/fuzz-vte-str.c" "$top"/tsm/*.c 2>"$tmp/fuzz-cc.log"; then
			mkdir -p "$tmp/corpus-$d"
			printf '\000\000\000\033Pq#0;2;100;0;0~~\033\\' >"$tmp/corpus-$d/seed1"
			printf '\007\200\\\033_Ga=T;AAAA\033\\\033Pqx\030' >"$tmp/corpus-$d/seed2"
			printf '\001\203\331\033Pq\033Pq"1;1;64;48~~\033\\\033\\' >"$tmp/corpus-$d/seed3"
			secs=$FUZZ_SECONDS
			if "$tmp/fuzz-$d" -max_total_time=$secs -max_len=65536 -print_final_stats=1 \
				"$tmp/corpus-$d" >"$tmp/fuzz.log" 2>&1; then
				echo "fuzz-vte-str HAVOC_SIXEL=$d: $secs s, $(ls "$tmp/corpus-$d" | wc -l) corpus files, no finding"
				grep "stat::number_of_executed_units" "$tmp/fuzz.log"
			else
				tail -30 "$tmp/fuzz.log" >&2
				cp "$tmp"/crash-* /s/havoc-pico-work/ 2>/dev/null
				fail "fuzz-vte-str HAVOC_SIXEL=$d"
			fi
			checks=$((checks + 1))
		else
			cat "$tmp/fuzz-cc.log" >&2
			echo "run-tsm: the fuzzer does not link here, skipped" >&2
		fi
	done
else
	echo "run-tsm: fuzzer skipped"
fi

echo "run-tsm: $checks checks, $failed failed"
[ $failed -eq 0 ]
