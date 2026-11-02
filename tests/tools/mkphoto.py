#!/usr/bin/env python3
"""A 240x320 noisy photo-like PNG (smooth field plus noise) for decode timing."""
import random, sys
from PIL import Image, ImageFilter
random.seed(1)
im = Image.new("RGB", (240, 320))
px = im.load()
for y in range(320):
    for x in range(240):
        px[x, y] = ((x * 255) // 239 ^ random.randrange(24), (y * 255) // 319 ^ random.randrange(24), ((x + y) * 3) % 256)
im = im.filter(ImageFilter.GaussianBlur(1))
im.save(sys.argv[1] + "/photo240x320.png")
im.save(sys.argv[1] + "/photo240x320-c0.png", compress_level=0)
