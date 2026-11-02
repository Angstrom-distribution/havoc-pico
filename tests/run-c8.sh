#!/bin/sh
# Unit test of the C8 picture dither (c8.c) with AddressSanitizer and UBSan.
# Run it in the container, from anywhere:  sh tests/run-c8.sh
set -e
top=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
gcc -g -O1 -Wall -Wextra -Werror -Wno-unused-parameter $SAN -I"$top" \
	-o "$tmp/c8" "$top/tests/test-c8-dither.c" "$top/c8.c"
"$tmp/c8"
