#!/usr/bin/env python3
"""Make the test images: a 64x48 RGB gradient with a black border, a 64x48
RGBA version with a transparent disc, and a 240x160 photo-like pattern."""
import math, sys
from PIL import Image
d = sys.argv[1]
im = Image.new("RGB", (64, 48))
for y in range(48):
    for x in range(64):
        im.putpixel((x, y), (x * 4, y * 5, 255 - x * 4) if 2 <= x < 62 and 2 <= y < 46 else (0, 0, 0))
im.save(f"{d}/grad64x48.png")
a = im.convert("RGBA")
for y in range(48):
    for x in range(64):
        if (x - 32) ** 2 + (y - 24) ** 2 < 15 ** 2:
            a.putpixel((x, y), (0, 0, 0, 0))
a.save(f"{d}/alpha64x48.png")
p = Image.new("RGB", (240, 160))
for y in range(160):
    for x in range(240):
        r = int(127 + 127 * math.sin(x / 13.0))
        g = int(127 + 127 * math.sin(y / 9.0 + x / 31.0))
        b = int(127 + 127 * math.cos((x + y) / 21.0))
        p.putpixel((x, y), (r, g, b))
p.save(f"{d}/pattern240x160.png")
