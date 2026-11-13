#!/bin/sh
# The default font on a QVGA output: havoc in the fake compositor
# (tests/fake-compositor.c --driver) with a wl_output (--output WxH) and a
# configuration, and the width of the window it commits first. The 18 pixel
# outline font is the reference, measured on a 640x480 output; with the 6x12
# bitmap font the window is 6 pixels per column wide. Also builds with
# PIXFONT_OBJ=pixfont-none.o, where every case must show the outline font.
# Run it in the container, from anywhere:  sh tests/run-qvga-font.sh
#
# Needs what run-gfx.sh needs, without PIL.
set -eu

top=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

pd=$(pkg-config --variable=pkgdatadir wayland-protocols)
for p in stable/xdg-shell/xdg-shell unstable/text-input/text-input-unstable-v3 \
	stable/viewporter/viewporter staging/fractional-scale/fractional-scale-v1; do
	b=$(basename "$p")
	wayland-scanner server-header <"$pd/$p.xml" >"$work/$b-server.h"
	wayland-scanner private-code <"$pd/$p.xml" >"$work/$b-protocol.c"
done
gcc -g -O1 -Wall -I"$work" -o "$work/fake-compositor" "$top/tests/fake-compositor.c" \
	"$work/xdg-shell-protocol.c" "$work/text-input-unstable-v3-protocol.c" \
	"$work/viewporter-protocol.c" "$work/fractional-scale-v1-protocol.c" \
	$(pkg-config --cflags --libs wayland-server xkbcommon)

copy() { # destination
	mkdir -p "$1"
	(cd "$top" && tar cf - --exclude=.git --exclude='*.o' --exclude=havoc .) | (cd "$1" && tar xf -)
}

copy "$work/bitmap"
make -C "$work/bitmap" CFLAGS="-g -O1 -Wall" >"$work/bitmap.log" 2>&1 || { cat "$work/bitmap.log"; exit 1; }
copy "$work/none"
make -C "$work/none" PIXFONT_OBJ=pixfont-none.o CFLAGS="-g -O1 -Wall" >"$work/none.log" 2>&1 || { cat "$work/none.log"; exit 1; }

export XDG_RUNTIME_DIR=$work
export PYTHONDONTWRITEBYTECODE=1
export ASAN_OPTIONS=detect_leaks=0
python3 - "$top/tests" "$work/fake-compositor" "$work/bitmap/havoc" "$work/none/havoc" <<'PY'
import sys

sys.path.insert(0, sys.argv[1])
from harness import Comp  # noqa: E402

comp, bitmap, nobitmap = sys.argv[2:5]
COLS = 80
failed = 0


def width(havoc, output, config=""):
    args = ("--output", output) if output else ()
    with Comp(comp, havoc, config=config, args=args) as c:
        return c.w


# the outline font, on an output no handheld has
ref = width(bitmap, "640x480")
print("reference, outline font, 640x480: window %d px wide, %.2f px per column"
      % (ref, ref / COLS))
if ref % COLS or ref // COLS <= 6:
    print("FAIL: the reference is not a font wider than 6 px")
    sys.exit(1)

cases = [
    # name, havoc, output, config, bitmap font expected
    ("240x320, no [font]", bitmap, "240x320", "", True),
    ("320x240, no [font]", bitmap, "320x240", "", True),
    ("640x480, no [font]", bitmap, "640x480", "", False),
    ("240x320, [font] size=18", bitmap, "240x320", "[font]\nsize=18\n", False),
    ("240x320, [font] pixel=no", bitmap, "240x320", "[font]\npixel=no\n", False),
    ("no output, no [font]", bitmap, None, "", False),
    ("240x320, no [font], PIXFONT_OBJ=pixfont-none.o", nobitmap, "240x320", "", False),
]
for name, havoc, output, config, small in cases:
    w = width(havoc, output, config)
    # the window is a whole number of columns of the font's cell width
    ok = (w == COLS * 6) if small else (w == ref)
    print("%s: %s: window %d px wide, %s" % (
        "PASS" if ok else "FAIL", name, w,
        "6 px cell" if w == COLS * 6 else "not a 6 px cell"))
    failed += not ok
sys.exit(1 if failed else 0)
PY
