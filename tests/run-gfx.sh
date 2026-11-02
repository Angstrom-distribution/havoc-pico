#!/bin/sh
# Window tests of the picture code: havoc built with HAVOC_SIXEL=1 and the
# sanitizers runs in the fake compositor (tests/fake-compositor.c) and is
# checked by graphics.py. Also builds with every HAVOC_* variable at 0 and
# checks that the picture code is not in the binary.
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

# HAVOC_KITTY=1 alone links and has no sixel
copy "$work/kitty"
make -C "$work/kitty" HAVOC_KITTY=1 CFLAGS="-g -O1 -Wall" >"$work/kitty.log" 2>&1 || { cat "$work/kitty.log"; exit 1; }
"$work/kitty/havoc" -v

export XDG_RUNTIME_DIR=$work
export PYTHONDONTWRITEBYTECODE=1
export ASAN_OPTIONS=detect_leaks=0
python3 "$top/tests/graphics.py" "$work/fake-compositor" "$work/on/havoc" "$@"
