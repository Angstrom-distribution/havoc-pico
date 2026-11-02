#!/bin/sh
# Window tests of the picture code: havoc built with HAVOC_SIXEL=1 and the
# sanitizers runs in the fake compositor (tests/fake-compositor.c) and is
# checked by graphics.py; built with HAVOC_SIXEL=1 HAVOC_KITTY=1
# HAVOC_PNG=1 it is checked by kitty.py as well. Also builds with every
# HAVOC_* variable at 0 and checks that the picture code is not in the
# binary, and fuzzes the store (tests/fuzz-gfx.c) for FUZZ_SECONDS (default
# 60, 0 skips it).
# Run it in the container, from anywhere:  sh tests/run-gfx.sh [scenario ...]
#
# Needs gcc, make, pkg-config, wayland-scanner, wayland-protocols, the
# development files of libwayland and libxkbcommon (plus xkb-data at run
# time), and python3 with PIL. The tree is copied, so it stays clean.
set -eu

top=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

san="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"

pd=$(pkg-config --variable=pkgdatadir wayland-protocols)
for p in stable/xdg-shell/xdg-shell unstable/text-input/text-input-unstable-v3 \
	stable/viewporter/viewporter staging/fractional-scale/fractional-scale-v1; do
	b=$(basename "$p")
	wayland-scanner server-header <"$pd/$p.xml" >"$work/$b-server.h"
	wayland-scanner private-code <"$pd/$p.xml" >"$work/$b-protocol.c"
done
gcc -g -O1 -Wall $san -I"$work" -o "$work/fake-compositor" "$top/tests/fake-compositor.c" \
	"$work/xdg-shell-protocol.c" "$work/text-input-unstable-v3-protocol.c" \
	"$work/viewporter-protocol.c" "$work/fractional-scale-v1-protocol.c" \
	$(pkg-config --cflags --libs wayland-server xkbcommon)

copy() { # destination
	mkdir -p "$1"
	(cd "$top" && tar cf - --exclude=.git --exclude='*.o' --exclude=havoc .) | (cd "$1" && tar xf -)
}

# every variable 0: the binary has no picture code
copy "$work/off"
make -C "$work/off" CFLAGS="-g -O1 -Wall" >"$work/off.log" 2>&1 || { cat "$work/off.log"; exit 1; }
if nm "$work/off/havoc" | grep -E ' [A-Za-z] (gfx_|sixel_|kitty_|kplace_|b64_|png_)'; then
	echo "FAIL: picture code in a build with every HAVOC_* variable 0" >&2
	exit 1
fi
"$work/off/havoc" -v

# HAVOC_SIXEL=1 with the sanitizers; only our own code is held to
# building without warnings
copy "$work/on"
make -C "$work/on" HAVOC_SIXEL=1 CFLAGS="-g -O1 -Wall -Wextra -Wno-unused-parameter $san" \
	LDFLAGS="$san" >"$work/on.log" 2>&1 || { cat "$work/on.log"; exit 1; }
if grep -E 'warning.*(graphics\.[ch]|sixel\.[ch]|paint_tile|collect_cell|gfx_)' "$work/on.log"; then
	echo "FAIL: the picture code builds with warnings" >&2
	exit 1
fi
"$work/on/havoc" -v

# HAVOC_KITTY=1 alone links, has no sixel and no libpng or zlib
copy "$work/kitty"
make -C "$work/kitty" HAVOC_KITTY=1 CFLAGS="-g -O1 -Wall -Wextra -Wno-unused-parameter" >"$work/kitty.log" 2>&1 || { cat "$work/kitty.log"; exit 1; }
if grep -E 'warning.*(graphics\.[ch]|kitty\.[ch]|kplace\.[ch]|base64\.[ch])' "$work/kitty.log"; then
	echo "FAIL: the kitty code builds with warnings" >&2
	exit 1
fi
if nm "$work/kitty/havoc" | grep -E ' [A-Za-z] (sixel_|png_|inflate)'; then
	echo "FAIL: sixel or PNG code in a HAVOC_KITTY=1 build" >&2
	exit 1
fi
"$work/kitty/havoc" -v

# HAVOC_PNG=1 needs HAVOC_KITTY=1
if make -C "$work/kitty" -n HAVOC_PNG=1 HAVOC_KITTY=0 >"$work/png.log" 2>&1 ||
   ! grep -q 'needs HAVOC_KITTY=1' "$work/png.log"; then
	echo "FAIL: HAVOC_PNG=1 without HAVOC_KITTY=1 does not stop make" >&2
	exit 1
fi

# all three, with the sanitizers: the build the kitty window tests run
copy "$work/all"
make -C "$work/all" HAVOC_SIXEL=1 HAVOC_KITTY=1 HAVOC_PNG=1 \
	CFLAGS="-g -O1 -Wall -Wextra -Wno-unused-parameter $san" \
	LDFLAGS="$san" >"$work/all.log" 2>&1 || { cat "$work/all.log"; exit 1; }
if grep -E 'warning.*(graphics\.[ch]|sixel\.[ch]|kitty[a-z-]*\.[ch]|kplace\.[ch]|base64\.[ch]|paint_layers|print1?_over|collect_cell|gfx_)' "$work/all.log"; then
	echo "FAIL: the picture code builds with warnings" >&2
	exit 1
fi
"$work/all/havoc" -v

# the store with a real screen, the sixel and kitty decoders and the
# placement lists under libFuzzer, seeded with the vectors behind a cap and
# cell-size header and window actions between them
FUZZ_SECONDS=${FUZZ_SECONDS:-60}
if [ "$FUZZ_SECONDS" -gt 0 ] && command -v clang >/dev/null &&
	clang -g -O1 -w -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
		-DHAVOC_SIXEL=1 -DHAVOC_KITTY=1 -DHAVOC_PNG=1 -DVERSION='"0.7.0-git"' \
		-D_XOPEN_SOURCE=700 $(pkg-config --cflags xkbcommon libpng zlib) -I"$top" \
		-o "$work/fuzz-gfx" "$top/tests/fuzz-gfx.c" "$top/sixel.c" "$top/kitty.c" \
		"$top/kitty-png.c" "$top/kitty-shm.c" "$top/base64.c" "$top/kplace.c" \
		"$top"/tsm/*.c $(pkg-config --libs libpng zlib) -lrt \
		2>"$work/fuzz-cc.log"; then
	mkdir "$work/corpus"
	n=0
	for f in "$top"/tests/vectors/kitty/*.bin; do
		n=$((n + 1))
		{ printf '\200\026'; cat "$f"; printf '\001\000\001\011'; cat "$f"; printf '\001\006'; } >"$work/corpus/k$n"
		{ printf '\010\046\033[?1049h'; cat "$f"; printf '\033[?1049l\001\000\033[2J\033[3J\001\000'; } >"$work/corpus/ka$n"
	done
	for f in "$top"/tests/vectors/sixel/*.bin; do
		n=$((n + 1))
		{ printf '\200\026'; cat "$f"; printf '\001\000\001\011\001\002\001\063\001\000'
		  cat "$f"; printf '\001\006'; } >"$work/corpus/v$n"
		{ printf '\010\046\033[?1049h'; cat "$f"; printf '\033[?1049l\001\000'
		  cat "$f"; printf '\033[2J\033[3J\001\000'; } >"$work/corpus/a$n"
	done
	if (cd "$work" && ./fuzz-gfx -max_total_time="$FUZZ_SECONDS" -max_len=65536 \
		-print_final_stats=1 corpus >fuzz.log 2>&1); then
		echo "fuzz-gfx: $FUZZ_SECONDS s, no finding"
		grep "stat::number_of_executed_units" "$work/fuzz.log"
	else
		tail -40 "$work/fuzz.log" >&2
		echo "FAIL: fuzz-gfx" >&2
		exit 1
	fi
else
	echo "fuzz-gfx: skipped (FUZZ_SECONDS=0 or no clang with libFuzzer)"
fi

export XDG_RUNTIME_DIR=$work
export PYTHONDONTWRITEBYTECODE=1
export ASAN_OPTIONS=detect_leaks=0
python3 "$top/tests/graphics.py" "$work/fake-compositor" "$work/on/havoc" "$@"
python3 "$top/tests/kitty.py" "$work/fake-compositor" "$work/all/havoc" "$@"
