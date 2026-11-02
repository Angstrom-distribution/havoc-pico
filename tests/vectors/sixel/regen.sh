#!/bin/sh
# Remake the captures with the installed tools. Needs python3 with PIL,
# img2sixel, chafa, timg and mpv. The test images are generated here.
# Run from anywhere: sh tests/vectors/sixel/regen.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
tools=$here/../../tools
img=$(mktemp -d)
trap 'rm -rf "$img"' EXIT
python3 -I "$tools/mkimg.py" "$img"
C="python3 -I $tools/ptycap.py"
G=$img/grad64x48.png
A=$img/alpha64x48.png
P=$img/pattern240x160.png
run() { name=$1; shift; $C --profile havoc-now --out "$here/$name.bin" "$@"; rm -f "$here/$name.bin.queries"; }

run img2sixel-grad -- img2sixel $G
run img2sixel-alpha -- img2sixel $A
run img2sixel-pattern -- img2sixel $P
run img2sixel-pattern-p16 -- img2sixel -p 16 $P
run chafa-sixel-grad -- chafa -f sixels $G
run chafa-sixel-pattern -- chafa -f sixels $P
run chafa-sixel-alpha -- chafa -f sixels $A
run timg-sixel -- timg -p sixel $G
run mpv-sixel --timeout 20 -- mpv --really-quiet --no-audio --frames=2 --vo=sixel av://lavfi:testsrc=size=96x64:rate=5
