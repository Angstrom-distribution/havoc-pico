#!/usr/bin/env python3
"""Reference sixel decoder for test vectors (independent of the C decoder).

    sixel_ref.py FILE [OUT.png] [--json OUT] [--max-w N]

Decodes every DCS ... q string in FILE (7-bit ESC P and ESC \\ only) and prints,
per image: P1;P2, raster attributes, decoded size, the graphics cursor y at the
end (gy, the top of the band the next sixel would go to), the bounding box of
set pixels, and the text rows a 10 px cell height gives with the foot rule
(ceil(gy / ch)). Writes the last image as PNG (unset pixels transparent).
A string ended by ESC followed by anything but '\\' is reported as aborted.

--json OUT writes a list with one object per string ({"p", "w", "h", "gy",
"hls", "aborted"}) and OUT.<n>.rgba for string n (w * h * 4 bytes, unset
pixels 0,0,0,0). --max-w N drops pixels at and past column N, as the C decoder
does with its limit. Colour registers wrap modulo 256 and registers 16 to 255
start black.
"""
import argparse
import colorsys
import json
import re

from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument("file")
ap.add_argument("png", nargs="?")
ap.add_argument("--json")
ap.add_argument("--max-w", type=int, default=4096)
args = ap.parse_args()
data = open(args.file, "rb").read()
SIDE = 4096

VT340 = [(0, 0, 0), (20, 20, 80), (80, 13, 13), (20, 80, 20), (80, 20, 80), (20, 80, 80),
         (80, 80, 20), (53, 53, 53), (26, 26, 26), (33, 33, 60), (60, 26, 26), (33, 60, 33),
         (60, 33, 60), (33, 60, 60), (60, 60, 33), (80, 80, 80)]


def pct(v):
    return min(255, (v * 255 + 50) // 100)


def decode(params, body):
    p = [int(x) if x else 0 for x in params.split(b";")] if params else []
    p1 = p[0] if p else 0
    aspect = 5 if p1 == 2 else 3 if p1 in (3, 4) else 1 if p1 in (7, 8, 9) else 2
    pal = {i: tuple(pct(c) for c in VT340[i]) for i in range(16)}
    px = {}
    x = y = 0
    colour = 0
    raster = None
    hls = False
    data_seen = False
    maxx = 0
    i = 0
    n = len(body)
    while i < n:
        c = body[i]
        if c == 0x22:  # raster attributes
            m = re.match(rb'"(\d*);?(\d*);?(\d*);?(\d*)', body[i:])
            v = [int(t) if t else 0 for t in m.groups()]
            if not data_seen:  # later ones are ignored
                raster = v
                if v[0] and v[1]:
                    aspect = min(5, max(1, v[0] // v[1]))
            i += m.end()
            continue
        if c == 0x23:  # colour introducer
            m = re.match(rb"#(\d*)(?:;(\d*);(\d*);(\d*);(\d*))?", body[i:])
            reg = int(m.group(1) or 0) % 256
            if m.group(2) is not None:
                pu, a, b, cc = (int(m.group(k) or 0) for k in (2, 3, 4, 5))
                if pu == 2:
                    pal[reg] = (pct(a), pct(b), pct(cc))
                elif pu == 1:  # HLS: hue a (0 = blue), lightness b, saturation cc
                    hls = True
                    r, g, bl = colorsys.hls_to_rgb(((a + 240) % 360) / 360, b / 100, cc / 100)
                    pal[reg] = (round(r * 255), round(g * 255), round(bl * 255))
            colour = reg
            i += m.end()
            continue
        if c == 0x21:  # repeat
            m = re.match(rb"!(\d*)([?-~])", body[i:])
            if not m:
                i += 1
                continue
            cnt = int(m.group(1) or 1) or 1
            bits = m.group(2)[0] - 0x3f
            data_seen = True
            for k in range(6):
                if bits & (1 << k):
                    for xx in range(x, min(x + cnt, args.max_w)):
                        for r in range(aspect):
                            px[(xx, y + k * aspect + r)] = colour
            x += cnt
            maxx = max(maxx, x)
            i += m.end()
            continue
        if c == 0x24:
            x = 0
        elif c == 0x2d:
            x = 0
            y += 6 * aspect
        elif 0x3f <= c <= 0x7e:
            bits = c - 0x3f
            data_seen = True
            for k in range(6):
                if bits & (1 << k) and x < args.max_w:
                    for r in range(aspect):
                        px[(x, y + k * aspect + r)] = colour
            x += 1
            maxx = max(maxx, x)
        i += 1
    ph = min(raster[2], SIDE) if raster else 0
    pv = min(raster[3], SIDE) if raster else 0
    w = min(max(maxx, ph), args.max_w)
    bbox = None
    if px:
        xs = [k[0] for k in px]
        ys = [k[1] for k in px]
        bbox = (min(xs), min(ys), max(xs), max(ys))
    # height: the last set pixel row; with P2 != 1 the declared raster area is
    # background, so at least Pv
    p2 = p[1] if len(p) > 1 else 0
    bottom = bbox[3] + 1 if bbox else 0
    h = bottom if p2 == 1 else max(bottom, pv)
    return dict(p=p, raster=raster, w=w, h=h, gy=y, bbox=bbox, pal=pal, px=px, hls=hls)


def rgba(d):
    buf = bytearray(d["w"] * d["h"] * 4)
    for (x, y), c in d["px"].items():
        if x < d["w"] and y < d["h"]:
            r, g, b = d["pal"].get(c, (0, 0, 0))
            o = (y * d["w"] + x) * 4
            buf[o:o + 4] = bytes((r, g, b, 255))
    return bytes(buf)


strings = re.finditer(rb"\x1bP([0-9;]*)q(.*?)(?=\x1b(.))", data, re.S)
last = None
res = []
for n, m in enumerate(strings):
    if m.group(3) != b"\\":
        print(f"aborted string: {len(m.group(2))} bytes, ended by ESC {m.group(3)!r}")
        res.append(dict(p=[], w=0, h=0, gy=0, hls=False, aborted=True))
        if args.json:
            open(f"{args.json}.{n}.rgba", "wb").write(b"")
        continue
    d = decode(m.group(1), m.group(2))
    ch = 10
    rows = max(1, -(-d["h"] // ch))
    rows_foot = min(max(1, -(-d["gy"] // ch)), rows)
    print(f"P={d['p']} raster={d['raster']} size={d['w']}x{d['h']} gy={d['gy']} "
          f"set-bbox={d['bbox']} rows(ch=10)={rows} cursor-row=+{rows_foot - 1}")
    last = d
    res.append(dict(p=d["p"], w=d["w"], h=d["h"], gy=d["gy"], hls=d["hls"], aborted=False))
    if args.json:
        open(f"{args.json}.{n}.rgba", "wb").write(rgba(d) if d["w"] and d["h"] else b"")
if args.json:
    json.dump(res, open(args.json, "w"))
if args.png and last:
    im = Image.frombytes("RGBA", (last["w"], last["h"]), rgba(last))
    im.save(args.png)
