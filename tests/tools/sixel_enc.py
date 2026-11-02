"""A small sixel encoder for the window tests: an image with a palette of at
most 256 colours becomes DCS ... ST. Pixels of palette index `clear` are left
unset (transparent). The expected pixels are not taken from the input but
from sixel_ref.py's decode of the stream, so rounding of the colours to
percent does not matter.
"""
from PIL import Image


def encode(img, p1=9, p2=1, clear=None, raster=True, st=True):
    """img: a PIL "P" image. Returns bytes."""
    w, h = img.size
    pal = img.getpalette()
    px = img.load()
    out = [b"\x1bP%d;%dq" % (p1, p2)]
    if raster:
        out.append(b'"1;1;%d;%d' % (w, h))
    used = sorted({px[x, y] for y in range(h) for x in range(w)})
    for i in used:
        if i == clear:
            continue
        out.append(b"#%d;2;%d;%d;%d" % (i, *(round(pal[3 * i + k] * 100 / 255)
                                              for k in range(3))))
    for band in range(0, h, 6):
        first = True
        for i in used:
            if i == clear:
                continue
            cols = []
            for x in range(w):
                m = 0
                for b in range(6):
                    y = band + b
                    if y < h and px[x, y] == i:
                        m |= 1 << b
                cols.append(m)
            if not any(cols):
                continue
            if not first:
                out.append(b"$")
            first = False
            out.append(b"#%d" % i)
            x = 0
            while x < w:
                n = 1
                while x + n < w and cols[x + n] == cols[x]:
                    n += 1
                c = bytes([63 + cols[x]])
                out.append(b"!%d%s" % (n, c) if n > 3 else c * n)
                x += n
        out.append(b"-")
    if st:
        out.append(b"\x1b\\")
    return b"".join(out)


def pattern(w, h, seed, blocks=3, colours=12):
    """Blocks of random colours: any shift or cut shows."""
    import random
    r = random.Random(seed)
    pal = [(r.randrange(256), r.randrange(256), r.randrange(256))
           for _ in range(colours)]
    im = Image.new("P", (w, h))
    flat = []
    for c in pal:
        flat += c
    im.putpalette(flat + [0] * (768 - len(flat)))
    px = im.load()
    for by in range(0, h, blocks):
        for bx in range(0, w, blocks):
            c = r.randrange(colours)
            for y in range(by, min(by + blocks, h)):
                for x in range(bx, min(bx + blocks, w)):
                    px[x, y] = c
    return im
