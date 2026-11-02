#!/usr/bin/env python3
"""Compare the JSON and RGBA files of sixel_ref.py --json with those of
test-sixel FILE OUT: sizes, gy and every RGBA byte equal, except that a
string with an HLS register may differ by 1 per channel (the C decoder uses
integers, the reference colorsys). A picture with a zero side is 0 x 0."""
import json
import sys

ref_path, dec_path = sys.argv[1], sys.argv[2]
ref = json.load(open(ref_path))
dec = json.load(open(dec_path))
bad = 0
if len(ref) != len(dec):
    print(f"{len(ref)} strings in the reference, {len(dec)} decoded")
    sys.exit(1)
pixels = 0
for n, (a, b) in enumerate(zip(ref, dec)):
    if not a["w"] or not a["h"]:  # the decoder reports nothing drawn as 0 x 0
        a["w"] = a["h"] = 0
    for k in ("aborted", "w", "h", "gy"):
        if a[k] != b[k]:
            print(f"string {n}: {k} {a[k]} (reference) against {b[k]}")
            bad += 1
    ra = open(f"{ref_path}.{n}.rgba", "rb").read()
    rb = open(f"{dec_path}.{n}.rgba", "rb").read()
    if len(ra) != len(rb):
        print(f"string {n}: {len(ra)} RGBA bytes against {len(rb)}")
        bad += 1
        continue
    tol = 1 if a.get("hls") else 0
    for i in range(len(ra)):
        if ra[i] != rb[i] and (abs(ra[i] - rb[i]) > tol or ra[i | 3] != rb[i | 3]):
            print(f"string {n}: pixel {i // 4} channel {i % 4}: {ra[i]} against {rb[i]}")
            bad += 1
            break
    pixels += len(ra) // 4
print(f"{len(ref)} strings, {pixels} pixels" + (f", {bad} differences" if bad else ""))
sys.exit(1 if bad else 0)
