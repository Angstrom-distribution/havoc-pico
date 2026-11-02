#!/bin/sh
# Build and run the placement list tests with AddressSanitizer and UBSan:
# test-kplace (synthetic events) and test-kplace-tsm (a real libtsm screen).
# Run it in the container, from anywhere:  sh tests/run-kplace.sh
set -e
top=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
CFLAGS="-g -O1 -Wall -Wextra -Wno-unused-parameter $SAN -D_XOPEN_SOURCE=700 -DHAVOC_SIXEL=1 -DHAVOC_KITTY=1 -I$top $(pkg-config --cflags xkbcommon)"
export ASAN_OPTIONS=detect_leaks=1

gcc $CFLAGS -Werror -o "$tmp/kplace" "$top/tests/test-kplace.c" "$top/kplace.c"
"$tmp/kplace"

# libtsm has warnings of its own: only ours are held to none
gcc $CFLAGS -c -o "$tmp/kplace.o" "$top/kplace.c" -Werror
gcc $CFLAGS -o "$tmp/kplace-tsm" "$top/tests/test-kplace-tsm.c" "$top/kplace.c" \
	"$top"/tsm/wcwidth.c "$top"/tsm/shl-htable.c "$top"/tsm/tsm-render.c \
	"$top"/tsm/tsm-screen.c "$top"/tsm/tsm-selection.c "$top"/tsm/tsm-unicode.c \
	"$top"/tsm/tsm-vte-charsets.c "$top"/tsm/tsm-vte.c 2>"$tmp/cc.log" || { cat "$tmp/cc.log" >&2; exit 1; }
if grep -E 'warning.*(test-kplace|kplace\.[ch])' "$tmp/cc.log"; then
	exit 1
fi
"$tmp/kplace-tsm"
