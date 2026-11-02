#!/bin/sh
# Build and run the kitty decoder tests with AddressSanitizer and UBSan,
# then fuzz it. Run in the container:
#   sh tests/run-kitty.sh
# FUZZ_SECONDS (default 60) sets the fuzzing time, 0 skips it. REGEN=1
# remakes the captured vectors first (needs the image tools).
set -e
top=$(cd "$(dirname "$0")/.." && pwd)
tests=$top/tests
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
PYTHON="python3 -I"

[ "${REGEN:-0}" = 1 ] && sh "$tests/vectors/kitty/regen.sh"

SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
CC=${CC:-gcc}
CFLAGS="-g -O1 -Wall -Wextra -Werror $SAN -D_XOPEN_SOURCE=700"
PNGLIBS=$(pkg-config --cflags --libs libpng zlib)
export ASAN_OPTIONS=detect_leaks=1

# the references
$PYTHON "$tests/tools/kitty_ref.py" decode "$tmp/ref0" --png 0 "$tests"/vectors/kitty/*.bin
$PYTHON "$tests/tools/kitty_ref.py" decode "$tmp/ref1" --png 1 "$tests"/vectors/kitty/*.bin
$PYTHON "$tests/tools/kitty_ref.py" pngs "$tmp/png"
$PYTHON "$tests/tools/kitty_ref.py" raw "$tmp/raw"

core="$top/kitty.c $top/base64.c $top/kitty-shm.c"
fail=0
run() {
	name=$1; shift
	echo "== $name"
	"$@"
	rc=$?
	if [ $rc -eq 77 ]; then echo "$name: skipped"; return 0; fi
	return $rc
}

$CC $CFLAGS -o "$tmp/base64" "$tests/test-base64.c" "$top/base64.c"
run test-base64 "$tmp/base64" || fail=1

for png in 0 1; do
	extra=; libs=
	if [ $png = 1 ]; then extra="$top/kitty-png.c"; libs=$PNGLIBS; fi
	$CC $CFLAGS -DHAVOC_PNG=$png -Wl,--wrap=malloc -o "$tmp/kitty$png" \
		"$tests/test-kitty.c" $core $extra $libs -lrt
	run "test-kitty PNG=$png" "$tmp/kitty$png" "$tmp/ref$png" "$tests/vectors/kitty" || fail=1
	$CC $CFLAGS -DHAVOC_PNG=$png -o "$tmp/shm$png" \
		"$tests/test-kitty-shm.c" $core $extra $libs -lrt
	rc=0; "$tmp/shm$png" || rc=$?
	[ $rc -eq 0 ] || [ $rc -eq 77 ] || fail=1
done
$CC $CFLAGS -DHAVOC_PNG=1 -o "$tmp/pngbin" "$tests/test-kitty-png.c" \
	$core "$top/kitty-png.c" $PNGLIBS -lrt
run test-kitty-png "$tmp/pngbin" "$tmp/png" "$tmp/raw" || fail=1

# no libpng or zlib symbol in the objects built without HAVOC_PNG
for f in kitty base64 kitty-shm; do
	$CC -O1 -DHAVOC_PNG=0 -c "$top/$f.c" -o "$tmp/$f.o"
done
if nm "$tmp"/kitty.o "$tmp"/base64.o "$tmp"/kitty-shm.o | grep -Ei ' [UT] (png_|inflate|deflate|crc32|adler32)'; then
	echo "libpng or zlib symbols in a HAVOC_PNG=0 build"
	fail=1
fi
# the decoders' byte loops have no divide
if sed -n '/^static bool raw_put/,/^}/p' "$top/kitty.c" | grep -E '[^/]/[^/*]|%'; then
	echo "divide in raw_put"; fail=1
fi
if sed -n '/^long b64_decode/,/^}/p' "$top/base64.c" | grep -E '[^/]/[^/*]|%'; then
	echo "divide in b64_decode"; fail=1
fi

# fuzzing
secs=${FUZZ_SECONDS:-60}
if [ "$secs" != 0 ] && command -v clang >/dev/null 2>&1; then
	if clang -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
		-DHAVOC_PNG=1 -o "$tmp/fuzz" "$tests/fuzz-kitty.c" $core "$top/kitty-png.c" \
		$PNGLIBS -lrt 2>"$tmp/fuzz.log"; then
		mkdir -p "$tmp/corpus"
		for f in "$tests"/vectors/kitty/*.bin; do
			$PYTHON - "$f" "$tmp/corpus" <<'PY'
import os, sys
d = open(sys.argv[1], "rb").read()
parts, i = [], 0
while True:
    a = d.find(b"\x1b_", i)
    b = d.find(b"\x1b\\", a)
    if a < 0 or b < 0:
        break
    parts.append(d[a + 2:b])
    i = b + 2
open(os.path.join(sys.argv[2], os.path.basename(sys.argv[1])), "wb").write(b"\x00" + b"\x1b\\".join(parts))
PY
		done
		echo "== fuzz-kitty ($secs s)"
		(cd "$tmp" && ./fuzz -max_total_time="$secs" -max_len=65536 corpus 2>&1 | tail -3) || fail=1
		ls "$tmp"/crash-* "$tmp"/oom-* "$tmp"/timeout-* 2>/dev/null && fail=1
	else
		echo "fuzzing skipped: libFuzzer does not link"; cat "$tmp/fuzz.log"
	fi
fi
if [ $fail -ne 0 ]; then echo "run-kitty: FAILED"; exit 1; fi
echo "run-kitty: passed"
