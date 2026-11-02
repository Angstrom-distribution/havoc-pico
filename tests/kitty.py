#!/usr/bin/env python3
"""Window tests for kitty pictures in havoc (HAVOC_KITTY=1, HAVOC_PNG=1,
[graphics] kitty=yes).

    kitty.py FAKE_COMPOSITOR HAVOC [scenario ...]

Runs in the fake compositor like graphics.py (which it borrows the harness
from). Each scenario sends kitty commands through the pty and compares the
window with a model of the screen: the placements as a list of pictures
with the geometry the protocol gives them, moved, cut and deleted by the
rules of the design, drawn with the same fixed-point sampling the window
uses. Text under or over a picture is taken from a snapshot of the window
taken without the picture. Every scenario runs twice, incrementally and
with HAVOC_FULL_REDRAW; the windows must be equal after every step.
"""
import base64
import io
import os
import random
import re
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "tools"))

from PIL import Image, ImageChops  # noqa: E402

import graphics as G  # noqa: E402
from graphics import (E, COLS, ROWS, CW, CH, BG, check, START, cup, lf,  # noqa: E402
                      at_bottom)

KCFG = "kitty=yes\n"
UNDER_BG = -1073741824

# ------------------------------------------------------------ the protocol

T5 = [((v >> 3) << 3) | ((v >> 3) >> 2) for v in range(256)]
T6 = [((v >> 2) << 2) | ((v >> 2) >> 4) for v in range(256)]


def quant(im):
    """The picture as the window holds it: RGB565, expanded again."""
    r, g, b, a = im.convert("RGBA").split()
    return Image.merge("RGBA", (r.point(T5), g.point(T6), b.point(T5), a))


def mkimg(w, h, seed=1, alpha=False):
    im = Image.new("RGBA", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            a = 255
            if alpha:
                a = (x * y * 5 + seed) & 255
                if (x + y) % 5 == 0:
                    a = 255
                if (x + 2 * y) % 7 == 0:
                    a = 0
            px[x, y] = ((x * 7 + y * 3 + seed * 29) & 255,
                        (x * 5 + y * 11 + seed * 13) & 255,
                        ((x ^ y) * 9 + seed * 7) & 255, a)
    return im


def solid(w, h, rgb):
    return Image.new("RGBA", (w, h), tuple(rgb) + (255,))


def apc(keys, payload=b"", chunk=None):
    """The APC string(s) of one command; chunk: base64 characters per
    chunk (a multiple of 4), the keys go with the first one."""
    b64 = base64.b64encode(payload)
    ST = b"\x1b\\"
    if chunk is None or len(b64) <= chunk:
        return (b"\x1b_G" + keys.encode() +
                (b";" + b64 if payload else b"") + ST)
    out = b""
    for i in range(0, len(b64), chunk):
        m = 1 if i + chunk < len(b64) else 0
        k = (keys + "," if i == 0 else "") + "m=%d" % m
        out += b"\x1b_G" + k.encode() + b";" + b64[i:i + chunk] + ST
    return out


def raw(im, fmt=32):
    return im.convert("RGBA" if fmt == 32 else "RGB").tobytes()


def tx(im, keys="", fmt=32, chunk=None, zl=False):
    """a=T (or what keys say) with the picture as raw pixels."""
    k = "a=T,f=%d,s=%d,v=%d" % (fmt, im.width, im.height)
    if keys:
        k += "," + keys
    data = raw(im, fmt)
    if zl:
        data = zlib.compress(data)
        k += ",o=z"
    return apc(k, data, chunk)


def png(im):
    b = io.BytesIO()
    im.convert("RGBA").save(b, "PNG")
    return b.getvalue()


# ---------------------------------------------------------------- the model

def mul(a, b):
    return (a * b + 255) >> 8


class Pl:
    """One expected placement, in screen rows (row 0 is the first row of
    the screen, negative in the scrollback)."""

    order = 0

    def __init__(self, img, col, row, cw, ch, anon=False, **kw):
        self.img = quant(img)
        self.col, self.row = col, row
        self.cw0, self.ch0 = cw, ch
        self.z = kw.get("z", 0)
        self.pid = kw.get("p", 0)
        self.iid = kw.get("i", 0)
        self.anon = anon
        self.skip = 0
        self.kw = kw
        W, H = img.size
        x, y = kw.get("x", 0), kw.get("y", 0)
        w, h = kw.get("w", 0) or W - x, kw.get("h", 0) or H - y
        self.sx, self.sy = x, y
        self.sw, self.sh = min(w, W - x), min(h, H - y)
        self.dx = min(kw.get("X", 0), cw - 1)
        self.dy = min(kw.get("Y", 0), ch - 1)
        c, r = kw.get("c", 0), kw.get("r", 0)
        sw, sh = self.sw, self.sh
        if c:
            dw = c * cw - self.dx
        elif r:
            dw = sw * (r * ch - self.dy) // sh
        else:
            dw = sw
        if r:
            dh = r * ch - self.dy
        elif c:
            dh = sh * (c * cw - self.dx) // sw
        else:
            dh = sh
        self.dw, self.dh = max(dw, 1), max(dh, 1)
        self.cols = (self.dx + self.dw + cw - 1) // cw
        self.rows = (self.dy + self.dh + ch - 1) // ch
        Pl.order += 1
        self.seq = Pl.order
        self.tiles = {}

    def shown_rows(self):
        return range(self.row + self.skip, self.row + self.rows)

    def tile(self, tx_, ty, cw, ch):
        """The pixels of the cell tx_, ty of the box in a cell of cw x ch:
        the window's 16.16 stepping, as an RGBA image (alpha 0: untouched)."""
        key = (tx_, ty, cw, ch)
        if key in self.tiles:
            return self.tiles[key]
        im = Image.new("RGBA", (cw, ch), (0, 0, 0, 0))
        out = im.load()
        src = self.img.load()
        bx = tx_ * self.cw0 - self.dx
        by = ty * self.ch0 - self.dy
        ax0 = (bx * self.sw * 65536) // self.dw
        ay0 = (by * self.sh * 65536) // self.dh
        fx = ((self.sw * self.cw0) << 16) // (self.dw * cw)
        fy = ((self.sh * self.ch0) << 16) // (self.dh * ch)
        for py in range(ch):
            ay = ay0 + py * fy
            if ay < 0:
                continue
            if ay >= self.sh << 16:
                break
            for px in range(cw):
                ax = ax0 + px * fx
                if ax < 0:
                    continue
                if ax >= self.sw << 16:
                    break
                p = src[self.sx + (ax >> 16), self.sy + (ay >> 16)]
                if p[3]:
                    out[px, py] = p
        self.tiles[key] = im
        return im


def blend(canvas, tile, x0, y0):
    """The tile over the canvas: opaque pixels replace, others blend with
    the window's premultiplied formula."""
    mask = tile.getchannel("A").point(lambda a: 255 if a == 255 else 0)
    canvas.paste(tile.convert("RGB"), (x0, y0), mask)
    cp, tp = canvas.load(), tile.load()
    for py in range(tile.height):
        for px in range(tile.width):
            r, g, b, a = tp[px, py]
            if 0 < a < 255:
                d = cp[x0 + px, y0 + py]
                ca = 255 - a
                cp[x0 + px, y0 + py] = tuple(
                    min(255, mul(s, a) + mul(t, ca))
                    for s, t in zip((r, g, b), d))


class Screen:
    """The expected placements of the main and the alternate screen."""

    def __init__(self, cw=CW, ch=CH, cols=COLS, rows=ROWS):
        self.cw, self.ch, self.cols, self.rows = cw, ch, cols, rows
        self.lists = [[], []]
        self.alt = 0
        self.sb = 0		# lines in the scrollback
        self.view = 0		# lines the view is scrolled back

    @property
    def pl(self):
        return self.lists[self.alt]

    def add(self, img, col, row, **kw):
        p = Pl(img, col, row, self.cw, self.ch, **kw)
        pid, iid = kw.get("p", 0), kw.get("i", 0)
        if pid:
            self.pl[:] = [q for q in self.pl
                          if not (q.iid == iid and q.pid == pid)]
        self.pl.append(p)
        return p

    def anon_replace(self, p):
        """The earlier frame of a video on the same box goes."""
        self.pl[:] = [q for q in self.pl if q is p or not (
            q.anon and p.anon and (q.row, q.col, q.rows, q.cols, q.z) ==
            (p.row, p.col, p.rows, p.cols, p.z))]

    def lf(self, n):
        """n line feeds at the bottom of a full-screen region."""
        self.region(0, self.rows - 1, n, to_sb=not self.alt)

    def region(self, t, b, n, to_sb=None):
        """Rows t..b move up by n (down when n < 0)."""
        to_sb = (t == 0 and not self.alt) if to_sb is None else to_sb
        keep = []
        for p in self.pl:
            first = p.row + p.skip
            last = min(p.row + p.rows - 1, self.rows - 1)
            if n > 0:
                inside = (t == 0 or first >= t) and last <= b and (
                    first >= 0 or (t == 0 and (to_sb or self.sb == 0)))
                if inside:
                    p.row -= n
                    first = p.row + p.skip
                    if first < t and not (t == 0 and to_sb):
                        p.skip += t - first
                        if p.skip >= p.rows:
                            continue
                elif to_sb and first < 0:
                    p.row -= n
            else:
                if first >= t and last <= b:
                    p.row += -n
                    if p.row + p.rows - 1 > b:
                        p.rows = b - p.row + 1
                        if p.rows <= p.skip:
                            continue
            keep.append(p)
        if n > 0 and to_sb and not self.alt:
            self.sb = min(self.sb + n, 100)
        self.lists[self.alt] = [p for p in keep
                                if p.row + p.rows > -(0 if self.alt else self.sb)]

    def erase_screen(self):
        self.pl[:] = [p for p in self.pl if not (
            p.row + p.skip <= self.rows - 1 and p.row + p.rows - 1 >= 0)]

    def clear_sb(self):
        keep = []
        for p in self.lists[0]:
            if p.row + p.rows <= 0:
                continue
            if p.row + p.skip < 0:
                p.skip = -p.row
            keep.append(p)
        self.lists[0] = keep
        self.sb = 0

    def enter_alt(self):
        self.alt = 1
        self.lists[1] = []

    def leave_alt(self):
        self.alt = 0

    def render(self, text=None, cols=None, coloured=(), cw=None, ch=None):
        """The window: background, pictures below the text, the text's
        pixels (text: a snapshot of the window made without pictures),
        pictures above. coloured: (row, col) cells whose background is not
        the default one, which hide z below -2^30."""
        cols = self.cols if cols is None else cols
        cw, ch = cw or self.cw, ch or self.ch
        win = Image.new("RGB", (cols * cw, self.rows * ch), BG)
        bgimg = win.copy()
        tx_img = None
        if text is not None:
            tx_img = text.convert("RGB").crop((0, 0, cols * cw, self.rows * ch))
            # a cell with a colour of its own: its background, which
            # pictures below -2^30 do not cover
            for (r_, c_) in coloured:
                col_ = tx_img.getpixel((c_ * cw, r_ * ch))
                box = (c_ * cw, r_ * ch, (c_ + 1) * cw, (r_ + 1) * ch)
                win.paste(col_, box)
                bgimg.paste(col_, box)
        view = 0 if self.alt else self.view
        order = sorted(self.pl, key=lambda p: (p.z, p.seq))

        def draw(p):
            for ty in p.shown_rows():
                vy = ty + view
                if not 0 <= vy < self.rows:
                    continue
                for tx_ in range(p.cols):
                    vx = p.col + tx_
                    if not 0 <= vx < cols:
                        continue
                    if p.z < UNDER_BG and (vy, vx) in coloured:
                        continue
                    blend(win, p.tile(tx_, ty - p.row, cw, ch), vx * cw,
                          vy * ch)

        for p in order:
            if p.z < 0:
                draw(p)
        if tx_img is not None:
            # glyph pixels: whatever the snapshot has that is not the
            # background of its cell
            diff = ImageChops.difference(tx_img, bgimg)
            gl = diff.convert("L").point(lambda v: 255 if v else 0)
            win.paste(tx_img, (0, 0), gl)
        for p in order:
            if p.z >= 0:
                draw(p)
        return win


# ---------------------------------------------------------------- helpers

class KRun(G.Run):
    def reply_log(self):
        return self.replies()

    def mark(self):
        self._mark = len(self.replies())

    def since(self):
        return self.replies()[getattr(self, "_mark", 0):]


def step(r, scr, label, **kw):
    """The window must equal the model."""
    img = r.snap().convert("RGB")
    r.snaps[label] = img.tobytes()
    if r.full:
        return
    exp = scr.render(**kw)
    if img.size != exp.size:
        check(False, "%s: %s: window %s, model %s" % (r.name, label,
                                                     img.size, exp.size))
        return
    diff = ImageChops.difference(img, exp)
    bb = diff.getbbox()
    if bb:
        if os.environ.get("KITTY_KEEP"):
            img.save("/tmp/%s-%s-act.png" % (r.name, label))
            exp.save("/tmp/%s-%s-exp.png" % (r.name, label))
        check(False, "%s: %s: window differs from the model at %s (cell %s)" %
              (r.name, label, bb, (bb[0] // CW, bb[1] // CH)))
    else:
        check(True, "")


def put(run, scr, im, col, row, keys="", fmt=32, chunk=None, zl=False,
        **kw):
    """Send a=T for im with its top left cell at (col, row) of the screen
    and add it to the model, with the cursor moves the protocol makes.
    kw: the numeric keys (c, r, x, y, w, h, X, Y, z, p, i, C, q)."""
    run.feed(E + "[%d;%dH" % (row + 1, col + 1))
    k = keys
    for name in ("c", "r", "x", "y", "w", "h", "X", "Y", "z", "p", "i", "C",
                 "q"):
        if name in kw:
            k += ("," if k else "") + "%s=%d" % (name, kw[name])
    run.feed(tx(im, k, fmt, chunk, zl))
    run.sync()
    mk = {n: v for n, v in kw.items() if n not in ("C", "q")}
    p = scr.add(im, col, row, anon=kw.get("i", 0) == 0, **mk)
    if p.anon:
        scr.anon_replace(p)
    if kw.get("C") != 1:
        room = scr.rows - 1 - row
        if p.rows > room:
            scr.lf(p.rows - room)
    return p


def debug(r):
    return r.debug()


# -------------------------------------------------------------- scenarios

def sc_basic(r):
    """1: a=T of a raw picture at the origin, the cursor, the reply."""
    scr = Screen()
    r.sync(START)
    im = mkimg(64, 48, 1)
    r.mark()
    put(r, scr, im, 0, 0, i=7)
    step(r, scr, "picture")
    check(r.since() == b"\x1b_Gi=7;OK\x1b\\", "basic: reply %r" % r.since())
    r.mark()
    r.feed(E + "[6n")
    r.sync()
    # 11 x 5 cells; the cursor goes right by 11 and down by 5
    check(r.since() == b"\x1b[6;12R", "basic: cursor %r" % r.since())
    d = r.debug()
    check(d and d["kitty"] == 1 and d["pm"] == 1 and d["pixels"] == 3072,
          "basic: debug %r" % d)
    # C=1: the cursor stays
    r.feed(E + "[20;5H")
    r.mark()
    put(r, scr, mkimg(30, 20, 2), 4, 19, C=1, i=8)
    r.feed(E + "[6n")
    r.sync()
    check(r.since().endswith(b"\x1b[20;5R"), "basic: C=1 cursor %r" % r.since())
    step(r, scr, "second")
    # 24-bit pixels
    put(r, scr, mkimg(40, 30, 3), 20, 2, fmt=24, i=9)
    step(r, scr, "rgb")
    # q=1 and q=2 silence the replies
    r.mark()
    put(r, scr, mkimg(6, 10, 4), 30, 25, i=10, q=1, C=1)
    put(r, scr, mkimg(6, 10, 4), 32, 25, i=11, q=2, C=1)
    check(r.since() == b"", "basic: replies with q %r" % r.since())
    step(r, scr, "q")
    r.mark()
    # an error with q=1 is told
    r.feed(apc("a=T,f=32,s=0,v=0,i=12,q=1", b"AAAA"))
    r.sync()
    check(r.since().startswith(b"\x1b_Gi=12;EINVAL"), "basic: q=1 error %r" %
          r.since())


def sc_formats(r):
    """2: chunks, zlib, PNG, a=t then a=p with placement ids."""
    scr = Screen()
    r.sync(START)
    im = mkimg(40, 30, 5)
    r.mark()
    put(r, scr, im, 0, 0, chunk=100, i=1)
    step(r, scr, "chunked")
    check(r.since() == b"\x1b_Gi=1;OK\x1b\\", "formats: chunked reply %r" %
          r.since())
    put(r, scr, mkimg(40, 30, 6), 10, 0, zl=True, i=2)
    step(r, scr, "zlib")
    imp = mkimg(50, 20, 7, alpha=True)
    imp.putalpha(255)
    r.feed(E + "[10;1H")
    r.mark()
    r.feed(apc("a=T,f=100,i=3", png(imp), 200))
    r.sync()
    check(r.since() == b"\x1b_Gi=3;OK\x1b\\", "formats: png reply %r" % r.since())
    scr.add(imp, 0, 9, i=3)
    step(r, scr, "png")
    # PNG with alpha, compressed on top of that (o=z): a=t, then placed
    ima = mkimg(24, 24, 8, alpha=True)
    r.feed(apc("a=t,f=100,i=4,o=z", zlib.compress(png(ima))))
    r.feed(E + "[20;20H" + "\x1b_Ga=p,i=4,C=1\x1b\\")
    r.sync()
    scr.add(ima, 19, 19, i=4)
    step(r, scr, "png zlib alpha")
    # a=t then a=p twice: p=1 twice replaces, p=2 adds
    imt = mkimg(18, 20, 9)
    r.mark()
    r.feed(apc("a=t,f=32,s=18,v=20,i=5", raw(imt)))
    r.feed(E + "[1;30H" + "\x1b_Ga=p,i=5,p=1,C=1\x1b\\")
    r.feed(E + "[6;30H" + "\x1b_Ga=p,i=5,p=1,C=1\x1b\\")
    r.feed(E + "[12;30H" + "\x1b_Ga=p,i=5,p=2,C=1\x1b\\")
    r.sync()
    scr.add(imt, 29, 5, i=5, p=1)
    scr.add(imt, 29, 11, i=5, p=2)
    check(r.since() == b"\x1b_Gi=5;OK\x1b\\" +
          b"\x1b_Gi=5,p=1;OK\x1b\\" * 2 + b"\x1b_Gi=5,p=2;OK\x1b\\",
          "formats: reply %r" % r.since())
    step(r, scr, "placement ids")
    d = r.debug()
    check(d and d["kitty"] == 5 and d["pm"] == 6, "formats: debug %r" % d)
    # the same id again: the old picture and its placements are gone
    imn = mkimg(12, 12, 10)
    r.feed(apc("a=t,f=32,s=12,v=12,i=5", raw(imn)))
    scr.pl[:] = [p for p in scr.pl if p.iid != 5]
    r.sync()
    step(r, scr, "retransmitted")
    r.mark()
    r.feed(E + "[1;1H" + "\x1b_Ga=p,i=5,p=3,C=1\x1b\\")
    r.sync()
    scr.add(imn, 0, 0, i=5, p=3)
    # a=p of an unknown id
    r.feed("\x1b_Ga=p,i=99\x1b\\".encode())
    r.sync()
    check(r.since().startswith(b"\x1b_Gi=5,p=3;OK") and
          r.since().endswith(b"\x1b_Gi=99;ENOENT:no such image\x1b\\"),
          "formats: ENOENT %r" % r.since())
    step(r, scr, "after")


def sc_geometry(r):
    """3: c and r scale the picture into a box; x, y, w, h cut the source;
    X and Y move it inside its first cell."""
    scr = Screen()
    r.sync(START)
    blue = solid(20, 10, (0, 0, 255))
    put(r, scr, blue, 0, 0, c=3, r=2, i=1)
    step(r, scr, "c r")
    img = r.snap().convert("RGB")
    # exactly 3 x 2 cells are blue, nothing outside
    box = img.crop((0, 0, 3 * CW, 2 * CH))
    check(box.getcolors() == [(3 * CW * 2 * CH, (0, 0, 255))],
          "geometry: box is %r" % box.getcolors())
    check(img.crop((3 * CW, 0, COLS * CW, ROWS * CH)).getcolors() ==
          [((COLS - 3) * CW * ROWS * CH, BG)], "geometry: outside the box")
    # only c: the height follows the aspect ratio
    im = mkimg(60, 30, 2)
    put(r, scr, im, 10, 0, c=5, i=2)
    step(r, scr, "c only")
    put(r, scr, im, 20, 0, r=3, i=3)
    step(r, scr, "r only")
    # a source rectangle
    put(r, scr, mkimg(60, 40, 3), 0, 5, x=10, y=8, w=30, h=20, i=4)
    step(r, scr, "source")
    put(r, scr, mkimg(60, 40, 3), 12, 5, x=50, y=30, i=5)	# w, h to the edge
    step(r, scr, "source edge")
    # an offset inside the first cell, clamped below the cell size
    put(r, scr, mkimg(30, 20, 4), 0, 12, X=4, Y=7, i=6)
    step(r, scr, "offset")
    put(r, scr, mkimg(30, 20, 4), 10, 12, X=50, Y=70, i=7)
    step(r, scr, "offset clamped")
    # a picture over the right edge is cut by the window
    put(r, scr, mkimg(60, 20, 5), 36, 20, i=8)
    step(r, scr, "right edge")
    # EINVAL: a source rectangle outside the picture
    r.mark()
    r.feed(apc("a=t,f=32,s=10,v=10,i=20", raw(mkimg(10, 10, 1))))
    r.sync()
    check(r.since().startswith(b"\x1b_Gi=20;OK"), "geometry: stored %r" % r.since())
    r.mark()
    r.feed("\x1b_Ga=p,i=20,x=10\x1b\\".encode())
    r.sync()
    check(r.since().startswith(b"\x1b_Gi=20;EINVAL"), "geometry: x outside %r" %
          r.since())
    step(r, scr, "after einval")


def cup_s(row, col):
    """Cursor to a 0-based screen row and column."""
    return E + "[%d;%dH" % (row + 1, col + 1)


def sc_survive(r):
    """4: pictures survive erased and overwritten text; z-index against
    text."""
    scr = Screen()
    r.sync(START)
    im = mkimg(66, 40, 1)
    # the text alone first; its snapshot is what the glyph pixels are
    r.sync(cup_s(4, 3) + "hello kitty, text under a picture")
    text0 = r.snap()
    put(r, scr, im, 2, 2, z=-1, i=1, C=1)	# below the text
    step(r, scr, "under the glyphs", text=text0)
    # erase the text in all the ways; the picture stays whole each time
    r.sync(cup_s(4, 3) + E + "[K")
    step(r, scr, "el")
    r.sync(cup_s(5, 4) + "abcdefgh" + cup_s(5, 5) + E + "[3X")
    r.sync(cup_s(5, 4) + E + "[K")
    step(r, scr, "ech")
    r.sync(cup_s(6, 4) + "abcdefgh" + cup_s(6, 5) + E + "[2P")
    r.sync(cup_s(6, 4) + E + "[K")
    step(r, scr, "dch")
    r.sync(cup_s(7, 4) + "abcdefgh" + cup_s(7, 4) + E + "[J")
    step(r, scr, "ed0")
    r.sync(cup_s(4, 0) + "xx yy" + cup_s(4, 0) + E + "[2K")
    step(r, scr, "el 2")
    # a fill of the screen with text and a clear: the placement is not text
    # z >= 0: the picture is over the glyphs
    r.sync(E + "[2J")	# deletes the placement; place it again over text
    scr.erase_screen()
    r.sync(cup_s(4, 3) + "over the glyphs " * 3)
    text1 = r.snap()
    put(r, scr, im, 2, 2, z=5, i=2, C=1)
    step(r, scr, "over the text", text=text1)
    r.feed(apc("a=d,d=I,i=2"))
    r.sync()
    scr.pl[:] = []
    check(r.snap().convert("RGB").tobytes() == text1.convert("RGB").tobytes(),
          "survive: the text is not back after the picture is deleted")


def sc_zorder(r):
    """5: z order of placements, text between layers, the background
    exception."""
    scr = Screen()
    r.sync(START)
    # two overlapping pictures: the higher z on top whatever the order
    a = mkimg(60, 40, 1)
    b = mkimg(60, 40, 2)
    put(r, scr, a, 2, 2, z=3, i=1, C=1)
    put(r, scr, b, 6, 3, z=1, i=2, C=1)
    step(r, scr, "z order")
    put(r, scr, mkimg(30, 30, 3), 8, 4, z=1, i=3, C=1)	# equal z: later on top
    step(r, scr, "equal z")
    # with text: write it, take the snapshot with the pictures gone
    r.feed(apc("a=d,d=A"))
    scr.pl[:] = []
    r.sync(cup_s(5, 2) + "THE QUICK BROWN FOX JUMPS" + cup_s(6, 2) +
           "over the lazy dog 0123456789")
    txt = r.snap()
    under = mkimg(120, 50, 4)
    # transparent holes show the glyphs and the picture below
    over = mkimg(60, 30, 5, alpha=True)
    put(r, scr, under, 1, 3, z=-1, i=11, C=1)
    put(r, scr, over, 12, 4, z=2, i=12, C=1)
    step(r, scr, "text between layers", text=txt)
    # coloured backgrounds: z below -2^30 is hidden there, shown on the rest
    r.feed(apc("a=d,d=A"))
    scr.pl[:] = []
    r.sync(E + "[2J")
    # blue blanks on rows 10..12, columns 4..9
    for row in (10, 11, 12):
        r.feed(cup_s(row, 4) + E + "[44m" + " " * 6 + E + "[0m")
    r.sync()
    blue = r.snap().convert("RGB").getpixel((5 * CW, 10 * CH + 3))
    check(blue != BG, "zorder: background colour")
    coloured = {(row, c) for row in (10, 11, 12) for c in range(4, 10)}
    text0 = r.snap()
    put(r, scr, mkimg(100, 60, 6), 2, 9, z=UNDER_BG - 1, i=21, C=1)
    step(r, scr, "under background", text=text0, coloured=coloured)
    # z = -2^30 exactly is above the backgrounds
    put(r, scr, mkimg(60, 40, 7), 20, 9, z=UNDER_BG, i=22, C=1)
    step(r, scr, "z minus 2^30", text=text0, coloured=coloured)


def sc_scroll(r):
    """6: placements move with the text: line feeds, the scrollback, back."""
    scr = Screen()
    r.sync(START)
    put(r, scr, mkimg(64, 48, 1), 0, 0, i=1, C=1)
    put(r, scr, mkimg(40, 30, 2), 20, 3, i=2, C=1)
    step(r, scr, "placed")
    r.feed(E + "[%d;1H" % ROWS)
    for n in (1, 4, 5, 6):
        r.sync(lf(n))
        scr.lf(n)
        step(r, scr, "lf%d" % n)
    # view the scrollback
    for n in (6, 8):
        for _ in range(n):
            r.c.cmd("mods 5")
            r.c.keys("k")
            r.c.cmd("mods 0")
        r.sync()
        scr.view = min(scr.view + n, scr.sb)
        step(r, scr, "scrollback %d" % scr.view)
    r.c.cmd("mods 5")
    r.c.cmd("key 107")
    r.c.cmd("mods 0")
    r.sync()
    scr.view = 0
    step(r, scr, "bottom")
    # reverse index and scroll down at the top: pictures wholly in the
    # screen move down with the rows
    r.sync(cup_s(0, 0) + E + "M")
    scr.region(0, ROWS - 1, -1)
    step(r, scr, "reverse index")
    r.sync(E + "[2T")
    scr.region(0, ROWS - 1, -2)
    step(r, scr, "scroll down")
    r.sync(E + "[3S")
    scr.region(0, ROWS - 1, 3)
    step(r, scr, "scroll up")
    # a tall picture scrolls the screen as it is placed
    scr2 = scr
    r.sync(E + "[3J")
    scr2.clear_sb()
    r.sync(E + "[2J")
    scr2.erase_screen()
    step(r, scr2, "cleared")
    tall = mkimg(60, 200, 5)
    put(r, scr2, tall, 4, 20, i=3)
    step(r, scr2, "tall")
    d = r.debug()
    check(d and d["kitty"] >= 1, "scroll: debug %r" % d)


def sc_region(r):
    """6b: a scroll region: a picture inside moves, one outside stays,
    insert and delete lines."""
    scr = Screen()
    r.sync(START)
    put(r, scr, mkimg(60, 30, 1), 0, 0, i=1, C=1)		# rows 0..2, above
    put(r, scr, mkimg(60, 30, 2), 20, 8, i=2, C=1)		# rows 8..10, inside
    put(r, scr, mkimg(60, 30, 3), 10, 25, i=3, C=1)	# rows 25..27, below
    step(r, scr, "placed")
    r.feed(E + "[6;20r" + E + "[20;1H")	# rows 5..19
    r.sync(lf(2))
    scr.region(5, 19, 2, to_sb=True)
    step(r, scr, "region lf")
    r.sync(cup_s(5, 0) + E + "M")
    scr.region(5, 19, -1)
    step(r, scr, "region ri")
    r.sync(E + "[r")
    r.sync(cup_s(8, 0) + E + "[3L")
    scr.region(8, ROWS - 1, -3)
    step(r, scr, "insert lines")
    r.sync(cup_s(8, 0) + E + "[2M")
    scr.region(8, ROWS - 1, 2, to_sb=False)
    step(r, scr, "delete lines")
    # a region whose top is below the picture's top cuts it
    r.sync(E + "[4;30r")
    r.feed(cup_s(29, 0))
    r.sync(lf(5))
    scr.region(3, 29, 5, to_sb=True)
    step(r, scr, "cut at the region top")


def sc_erase(r):
    """7: ED 2, ED 3, the alternate screen, a hard reset."""
    scr = Screen()
    r.sync(START)
    put(r, scr, mkimg(64, 48, 1), 0, 0, i=1, C=1)
    put(r, scr, mkimg(64, 48, 2), 20, 12, i=2, C=1)
    step(r, scr, "placed")
    # scroll the first into the scrollback, then ED 2: it is not on the
    # screen and stays, the one on the screen goes
    r.feed(E + "[32;1H")
    r.sync(lf(8))
    scr.lf(8)
    r.sync(E + "[2J")
    scr.erase_screen()
    step(r, scr, "ed2")
    d = r.debug()
    check(d and d["pm"] == 1, "erase: after ED 2 %r" % d)
    # the scrollback one is still there: view it
    for _ in range(14):
        r.c.cmd("mods 5")
        r.c.keys("k")
        r.c.cmd("mods 0")
    r.sync()
    scr.view = min(14, scr.sb)
    step(r, scr, "scrollback after ed2")
    r.c.cmd("mods 5")
    r.c.cmd("key 107")
    r.c.cmd("mods 0")
    r.sync()
    scr.view = 0
    r.sync(E + "[3J")
    scr.clear_sb()
    d = r.debug()
    check(d and d["pm"] == 0 and d["kitty"] == 2 and d["pixels"] == 6144,
          "erase: after ED 3 %r" % d)
    # a placement that straddles the scrollback and the screen
    put(r, scr, mkimg(60, 100, 3), 5, 25, i=3)
    step(r, scr, "straddle")
    r.sync(E + "[3J")
    scr.clear_sb()
    step(r, scr, "ed3 cuts")
    # the alternate screen
    r.sync(E + "[2J")
    scr.erase_screen()
    put(r, scr, mkimg(64, 48, 4), 2, 2, i=4, C=1)
    r.sync(E + "[?1049h" + E + "[?25l")
    scr.enter_alt()
    step(r, scr, "alt empty")
    put(r, scr, mkimg(64, 48, 5), 10, 10, i=5, C=1)
    step(r, scr, "alt picture")
    r.feed(E + "[32;1H")
    r.sync(lf(3))
    scr.lf(3)
    step(r, scr, "alt scrolled")
    d = r.debug()
    check(d and d["kitty"] == 5 and d["pm"] == 1 and d["pa"] == 1,
          "erase: alt debug %r" % d)
    r.sync(E + "[?1049l")
    scr.leave_alt()
    step(r, scr, "main again")
    # the alternate screen starts empty again
    r.sync(E + "[?1049h" + E + "[?25l")
    scr.enter_alt()
    step(r, scr, "alt cleared on entry")
    d = r.debug()
    check(d and d["pa"] == 0, "erase: alt list after entry %r" % d)
    r.sync(E + "[?1049l")
    scr.leave_alt()
    # a hard reset removes everything
    r.sync(E + "c" + E + "[?25l")
    scr.lists = [[], []]
    scr.sb = 0
    step(r, scr, "ris")
    d = r.debug()
    check(d and d["kitty"] == 0 and d["pixels"] == 0 and d["pm"] == 0,
          "erase: after RIS %r" % d)


def sc_delete(r):
    """8: a=d with the selectors."""
    scr = Screen()
    r.sync(START)

    def again(im, col, row, **kw):
        """a=p of a stored picture at a cell, and the model."""
        k = "a=p,C=1," + ",".join("%s=%d" % kv for kv in kw.items())
        r.feed(cup_s(row, col) + apc(k).decode())
        r.sync()
        scr.add(im, col, row, **kw)

    def three():
        scr.pl[:] = []
        r.feed(apc("a=d,d=A"))
        im1 = mkimg(30, 20, 1)
        put(r, scr, im1, 2, 2, i=1, p=1, z=1, C=1)
        put(r, scr, mkimg(30, 20, 2), 10, 2, i=2, p=2, z=2, C=1)
        again(im1, 2, 10, i=1, p=3, z=2)

    three()
    step(r, scr, "three")
    r.mark()
    r.feed(apc("a=d,d=i,i=1,p=3"))
    r.sync()
    scr.pl[:] = [p for p in scr.pl if not (p.iid == 1 and p.pid == 3)]
    step(r, scr, "d=i with p")
    check(r.since() == b"", "delete: a delete has no OK reply %r" % r.since())
    r.feed(apc("a=d,d=i,i=1"))
    r.sync()
    scr.pl[:] = [p for p in scr.pl if p.iid != 1]
    step(r, scr, "d=i")
    d = r.debug()
    check(d and d["kitty"] == 2 and d["pm"] == 1, "delete: lowercase keeps the "
          "picture %r" % d)
    r.feed(apc("a=d,d=I,i=2"))
    r.sync()
    scr.pl[:] = []
    step(r, scr, "d=I")
    d = r.debug()
    check(d and d["kitty"] == 1 and d["pm"] == 0, "delete: uppercase frees %r" % d)
    three()
    # c: the placements over the cursor; the cursor is at (3, 3)
    r.feed(cup_s(3, 3))
    r.feed(apc("a=d,d=c"))
    r.sync()
    scr.pl[:] = [p for p in scr.pl if p.iid != 1 or p.pid != 1]
    step(r, scr, "d=c")
    # p: the cell x, y (1-based)
    r.feed(apc("a=d,d=p,x=12,y=4"))
    r.sync()
    scr.pl[:] = [p for p in scr.pl if p.pid != 2]
    step(r, scr, "d=p")
    three()
    r.feed(apc("a=d,d=x,x=11"))
    r.sync()
    scr.pl[:] = [p for p in scr.pl if p.pid != 2]
    step(r, scr, "d=x")
    r.feed(apc("a=d,d=y,y=11"))
    r.sync()
    scr.pl[:] = [p for p in scr.pl if p.pid != 3]
    step(r, scr, "d=y")
    three()
    r.feed(apc("a=d,d=z,z=2"))
    r.sync()
    scr.pl[:] = [p for p in scr.pl if p.z != 2]
    step(r, scr, "d=z")
    r.feed(apc("a=d,d=A"))
    r.sync()
    scr.pl[:] = []
    step(r, scr, "d=A")
    d = r.debug()
    # image 1 lost its last placement here and is freed; image 2 lost its
    # placement to the lowercase d=z and stays as a stored picture
    check(d and d["kitty"] == 1 and d["pixels"] == 600 and d["pm"] == 0,
          "delete: d=A frees what it emptied %r" % d)
    # d=a of the visible placements only: one in the scrollback stays
    put(r, scr, mkimg(30, 20, 1), 2, 2, i=1, C=1)
    r.feed(E + "[32;1H")
    r.sync(lf(10))
    scr.lf(10)
    put(r, scr, mkimg(30, 20, 2), 2, 5, i=2, C=1)
    r.feed(apc("a=d,d=a"))
    r.sync()
    scr.pl[:] = [p for p in scr.pl if p.row + p.rows <= 0]
    d = r.debug()
    check(d and d["pm"] == 1, "delete: d=a keeps the scrollback one %r" % d)
    # errors
    r.mark()
    r.feed(apc("a=d,d=i,i=77"))
    r.feed(apc("a=d,d=q,i=1"))
    r.sync()
    check(r.since() == b"\x1b_Gi=77;ENOENT:no such image\x1b\\" +
          b"\x1b_Gi=1;EINVAL:bad or unsupported key\x1b\\",
          "delete: error replies %r" % r.since())


def sc_anon(r):
    """9: a video: anonymous frames on the same box do not pile up."""
    scr = Screen()
    r.sync(START)
    for i in range(6):
        put(r, scr, mkimg(48, 40, i), 4, 3, C=1)
        step(r, scr, "frame %d" % i)
    d = r.debug()
    check(d and d["kitty"] == 1 and d["pm"] == 1 and d["pixels"] == 48 * 40,
          "anon: debug %r" % d)
    # another box: a second picture; a different z is a different box
    put(r, scr, mkimg(48, 40, 9), 4, 3, z=-1, C=1)
    d = r.debug()
    check(d and d["kitty"] == 2 and d["pm"] == 2, "anon: z %r" % d)
    step(r, scr, "z")
    # an image with an id is never replaced by the rule
    for i in range(3):
        put(r, scr, mkimg(20, 20, i), 20, 3, i=50 + i, C=1)
    d = r.debug()
    check(d and d["kitty"] == 5, "anon: ids kept %r" % d)
    # an anonymous picture goes with its last placement
    r.feed(apc("a=d,d=a"))
    r.sync()
    d = r.debug()
    check(d and d["pm"] == 0 and d["kitty"] == 3, "anon: after d=a %r" % d)


def sc_memory(r):
    """10: the pixel cap and what goes first."""
    scr = Screen()
    r.sync(START)
    # 20 x 20 = 400 pixels each; the cap is 1500
    r.mark()
    r.feed(apc("a=t,f=32,s=20,v=20,i=1", raw(mkimg(20, 20, 1))))
    r.feed(apc("a=t,f=32,s=20,v=20,i=2", raw(mkimg(20, 20, 2))))
    r.feed(apc("a=t,f=32,s=20,v=20,i=3", raw(mkimg(20, 20, 3))))
    r.sync()
    d = r.debug()
    check(d and d["kitty"] == 3 and d["pixels"] == 1200, "memory: three %r" % d)
    # the fourth: the oldest unplaced goes first
    r.feed(apc("a=t,f=32,s=20,v=20,i=4", raw(mkimg(20, 20, 4))))
    r.feed(apc("a=t,f=32,s=20,v=20,i=5", raw(mkimg(20, 20, 5))))
    r.sync()
    d = r.debug()
    check(d and d["kitty"] == 3 and d["pixels"] == 1200, "memory: evicted %r" % d)
    check(r.since().count(b"OK") == 5, "memory: replies %r" % r.since())
    # placing a picture that was evicted: ENOENT; the newest ones are there
    r.mark()
    r.feed("\x1b_Ga=p,i=1,C=1\x1b\\".encode())
    r.feed("\x1b_Ga=p,i=5,C=1\x1b\\".encode())
    r.sync()
    check(r.since().startswith(b"\x1b_Gi=1;ENOENT") and
          b"\x1b_Gi=5;OK" in r.since(), "memory: ENOENT %r" % r.since())
    scr.add(mkimg(20, 20, 5), 0, 0, i=5)
    step(r, scr, "placed")
    # everything placed and in view: no room
    r.feed(cup_s(0, 10) + "\x1b_Ga=p,i=4,C=1\x1b\\")
    r.feed(cup_s(0, 20) + "\x1b_Ga=p,i=3,C=1\x1b\\")
    r.sync()
    scr.add(mkimg(20, 20, 4), 10, 0, i=4)
    scr.add(mkimg(20, 20, 3), 20, 0, i=3)
    r.mark()
    r.feed(apc("a=t,f=32,s=40,v=20,i=6", raw(mkimg(40, 20, 6))))
    r.sync()
    check(r.since() == b"\x1b_Gi=6;ENOMEM:picture too large\x1b\\",
          "memory: in view %r" % r.since())
    step(r, scr, "all kept")
    # a picture over the cap
    r.mark()
    r.feed(apc("a=t,f=32,s=60,v=30,i=7", raw(mkimg(60, 30, 7))))
    r.sync()
    check(r.since() == b"\x1b_Gi=7;ENOMEM:picture too large\x1b\\",
          "memory: over the cap %r" % r.since())
    # scrolled out of view: the oldest goes
    r.feed(E + "[32;1H")
    r.sync(lf(40))
    scr.lf(40)
    r.mark()
    r.feed(apc("a=t,f=32,s=40,v=20,i=8", raw(mkimg(40, 20, 8))))
    r.sync()
    check(r.since() == b"\x1b_Gi=8;OK\x1b\\", "memory: after scroll %r" % r.since())
    d = r.debug()
    check(d and d["pixels"] <= 1500, "memory: cap %r" % d)


def sc_shm(r):
    """11: t=s from POSIX shared memory of the same user."""
    scr = Screen()
    r.sync(START)
    im = mkimg(32, 24, 3)
    name = "havoc-kt-%d" % os.getpid()
    path = "/dev/shm/" + name
    if not os.path.isdir("/dev/shm"):
        print("note: no /dev/shm, skipped")
        return
    with open(path, "wb") as f:
        f.write(raw(im))
    r.mark()
    r.feed(E + "[1;1H")
    r.feed(apc("a=T,f=32,s=32,v=24,t=s,i=1", name.encode()))
    r.sync()
    scr.add(im, 0, 0, i=1)
    step(r, scr, "shm")
    check(r.since() == b"\x1b_Gi=1;OK\x1b\\", "shm: reply %r" % r.since())
    check(not os.path.exists(path), "shm: object not unlinked")
    # a name with a slash, a missing object, a short object
    r.mark()
    r.feed(apc("a=t,f=32,s=32,v=24,t=s,i=2", b"../etc/passwd"))
    r.feed(apc("a=t,f=32,s=32,v=24,t=s,i=3", b"nonexistent-object"))
    with open(path, "wb") as f:
        f.write(b"short")
    r.feed(apc("a=t,f=32,s=32,v=24,t=s,i=4", name.encode()))
    r.sync()
    check(r.since().count(b"EBADF:shared memory not usable") == 3,
          "shm: failures %r" % r.since())
    os.unlink(path) if os.path.exists(path) else None
    # t=f and t=t are refused
    r.mark()
    r.feed(apc("a=t,f=32,s=1,v=1,t=f,i=5", b"/etc/passwd"))
    r.feed(apc("a=t,f=32,s=1,v=1,t=t,i=6", b"/tmp/x"))
    r.sync()
    check(r.since().count(b"EBADF:transmission medium not supported") == 2,
          "shm: t=f %r" % r.since())


def sc_shm_off(r):
    """11b: without kitty shm=yes t=s is refused and the object stays."""
    r.sync(START)
    name = "havoc-kt-off-%d" % os.getpid()
    path = "/dev/shm/" + name
    if not os.path.isdir("/dev/shm"):
        return
    with open(path, "wb") as f:
        f.write(raw(mkimg(8, 8, 1)))
    r.mark()
    r.feed(apc("a=T,f=32,s=8,v=8,t=s,i=1", name.encode()))
    r.sync()
    check(r.since().startswith(b"\x1b_Gi=1;EBADF"), "shm off: %r" % r.since())
    check(os.path.exists(path), "shm off: object unlinked")
    os.unlink(path)


def sc_replies(r):
    """12: queries answered in input order, like icat's detection."""
    r.sync(START)
    r.mark()
    r.feed(apc("a=q,i=31,s=1,v=1,t=d,f=24", b"\x00\x00\x00") + (E + "[c").encode() +
           apc("a=q,i=32,s=1,v=1,t=d,f=24", b"\x00\x00") + (E + "[5n").encode() +
           apc("a=q,i=33,s=1,v=1,t=d,f=32", b"\x00\x00\x00\x00"))
    r.sync()
    want = (b"\x1b_Gi=31;OK\x1b\\" + b"\x1b[?62;1;4;6;9;15c" +
            b"\x1b_Gi=32;ENODATA:data short or too long\x1b\\" + b"\x1b[0n" +
            b"\x1b_Gi=33;OK\x1b\\")
    check(r.since() == want, "replies: %r instead of %r" % (r.since(), want))
    d = r.debug()
    check(d is None or d["kitty"] == 0 and d["pixels"] == 0,
          "replies: a=q stored something %r" % d)
    # a chunked transfer cut by a new command: ENODATA, then the command
    r.mark()
    r.feed(b"\x1b_Ga=t,f=32,s=2,v=2,i=40,m=1;AAAA\x1b\\" +
           apc("a=q,i=41,s=1,v=1,t=d,f=24", b"\x01\x02\x03"))
    r.sync()
    check(r.since() == b"\x1b_Gi=40;ENODATA:data short or too long\x1b\\" +
          b"\x1b_Gi=41;OK\x1b\\", "replies: chunk cut %r" % r.since())
    # unsupported keys
    r.mark()
    r.feed(apc("a=t,f=32,s=1,v=1,i=50,I=3", b"\0\0\0\0") +
           apc("a=t,f=32,s=1,v=1,i=51,U=1", b"\0\0\0\0") +
           apc("a=f,i=52") + apc("a=T,f=100,i=53", b"not a png") +
           apc("a=t,f=32,i=54", b"\0\0\0\0"))
    r.sync()
    for i in (50, 51, 52, 54):
        check(b"\x1b_Gi=%d;EINVAL" % i in r.since(), "replies: EINVAL %d %r" %
              (i, r.since()))
    check(b"\x1b_Gi=53;EINVAL" in r.since(), "replies: bad png %r" % r.since())


def sc_replies_off(r):
    """12b: kitty=no: APC is ignored and the text after it shows."""
    r.sync(START)
    r.mark()
    r.feed(apc("a=q,i=31,s=1,v=1,t=d,f=24", b"\x00\x00\x00") + b"ok" +
           (E + "[c").encode())
    r.sync()
    check(r.since() == b"\x1b[?62;1;4;6;9;15c", "replies off: %r" % r.since())
    img = r.snap().convert("RGB")
    check(img.crop((0, 0, 2 * CW, CH)).getbbox() is not None, "replies off: text")


def sc_hostile(r):
    """13: hostile input: nothing crashes, the cap holds."""
    rnd = random.Random(11)
    r.sync(START)
    big = b"a" * 100000
    streams = [
        apc("a=T,f=32,s=100000,v=100000,i=1", b"x" * 100),
        apc("a=T,f=32,s=4096,v=4096,i=1", b"x" * 100),
        apc("a=T,f=24,s=0,v=0,i=2"),
        b"\x1b_Ga=T,f=32,s=1,v=1;" + big + b"\x1b\\",
        b"\x1b_Ga=T,f=32,s=2,v=2,m=1;AAAA\x1b\\" * 50 + b"\x1b\\",
        b"\x1b_G" + b"i=1," * 300 + b"a=T\x1b\\",
        b"\x1b_Ga=T,f=32,s=1,v=1;!!!!\x1b\\",
        b"\x1b_Ga=T,f=32,s=1,v=1,c=999999,r=999999;AAAAAA==\x1b\\",
        b"\x1b_Ga=T,f=100;" + base64.b64encode(b"\x89PNG\r\n\x1a\n" + b"\0" * 50) + b"\x1b\\",
        b"\x1b_Ga=T,f=32,s=3,v=3,o=z;" + base64.b64encode(zlib.compress(b"\0" * 100000)) + b"\x1b\\",
        b"\x1b_Ga=p,i=4294967295,p=4294967295,z=-2147483648,c=4096,r=4096\x1b\\",
        b"\x1b_Ga=d,d=x,x=0\x1b\\", b"\x1b_Ga=d,d=y,y=99999\x1b\\",
        b"\x1b_Ga=T,f=32,s=1,v=1,t=s;" + base64.b64encode(b"/" + b"a" * 300) + b"\x1b\\",
        b"\x1b_G\xc3\xa9=1\x1b\\", b"\x1b_Gx\x1b\\", b"\x1b_G;\x1b\\", b"\x1b_G\x1b\\",
        b"\x1b_Ga=T,f=32,s=1,v=1;AAAAAAAA\x9c",
        b"\x1b_Ga=T,f=32,s=1,v=1;AA\x1b[2JAA\x1b\\",
        b"\x1b_Ga=T,f=32,s=1,v=1,i=1;AAAAAA==\x18more",
    ]
    for i, s in enumerate(streams):
        r.feed(s)
        r.sync(b"\x1b[0m\x1bc\x1b[?25l")
        check(r.alive(), "hostile: havoc stopped after stream %d" % i)
        d = r.debug()
        check(d is None or d["pixels"] <= d["cap"], "hostile: over the cap %r" % d)
    # more placements than the list holds
    one = mkimg(6, 10, 1)
    r.feed(apc("a=t,f=32,s=6,v=10,i=100", raw(one)))
    r.feed(b"".join(("\x1b[%d;%dH" % (1 + i % 32, 1 + i // 32 % 40)).encode() +
                    b"\x1b_Ga=p,i=100,C=1,z=%d\x1b\\" % (i % 7)
                    for i in range(600)))
    r.sync()
    d = r.debug()
    check(d and d["pm"] <= 256, "hostile: placements %r" % d)
    # a flood of anonymous pictures
    r.feed(b"".join(tx(one, "C=1") for _ in range(400)))
    r.sync()
    d = r.debug()
    check(d and d["pixels"] <= d["cap"] and d["kitty"] <= 1024,
          "hostile: flood %r" % d)
    r.sync(b"\x1b[2J")
    r.feed(b"".join(tx(one) + b"\n" for _ in range(200)))
    r.sync()
    # random bytes
    for i in range(30):
        n = rnd.randrange(1, 4000)
        data = bytes(rnd.randrange(256) for _ in range(n))
        r.feed(b"\x1b_Ga=T,f=32,s=2,v=2;" + data if i % 2 else b"\x1b_G" + data)
    r.sync(b"\x1b\\\x1b[0m")
    check(r.alive(), "hostile: havoc stopped after random bytes")
    r.sync(b"\x1bc\x1b[?25l")
    r.snaps["done"] = r.snap().tobytes()
    d = r.debug()
    check(d and d["kitty"] == 0 and d["pixels"] == 0 and d["pm"] == 0,
          "hostile: the store holds pictures after a hard reset %r" % d)


def sc_resize(r):
    """14: narrower and back; the hidden columns return."""
    scr = Screen()
    r.sync(START)
    put(r, scr, mkimg(64, 48, 1), 25, 4, i=1, C=1)
    put(r, scr, mkimg(64, 48, 2), 0, 12, i=2, C=1)
    step(r, scr, "full")
    r.c.cmd("configure 180 320")
    r.sync()
    step(r, scr, "narrow", cols=30)
    r.c.cmd("configure 240 320")
    r.sync()
    step(r, scr, "wide again")
    # fewer rows: the screen scrolls the pictures up with the text
    r.sync(E + "[32;1H ")
    r.c.cmd("configure 240 200")
    r.sync()
    # 20 rows now; the text on the last of 32 rows needs 12 lines up
    scr.rows = 20
    scr.region(0, 31, 12, to_sb=True)
    scr.rows = 20
    step(r, scr, "fewer rows")


def sc_scale(r):
    """14b: a bigger cell scales the pictures; the box stays in its cells."""
    cw, ch = r.w // COLS, r.h // ROWS
    scr = Screen(cw=cw, ch=ch)
    r.sync(START)
    put(r, scr, mkimg(64, 48, 1), 3, 2, i=1, C=1)
    put(r, scr, mkimg(60, 40, 2), 20, 8, i=2, C=1, c=6, r=3)
    step(r, scr, "scale 1")
    r.c.cmd("scale 240")
    r.sync()
    snap = r.snap()
    cw2, ch2 = snap.width // COLS, snap.height // ROWS
    check((cw2, ch2) != (cw, ch), "scale: window %s" % (snap.size,))
    scr.cw, scr.ch = cw2, ch2
    step(r, scr, "scaled", cw=cw2, ch=ch2)
    # a picture placed at the bigger size, then back to the small cell
    scr.cw, scr.ch = cw2, ch2
    put(r, scr, mkimg(50, 30, 3), 20, 20, i=3, C=1)
    step(r, scr, "new", cw=cw2, ch=ch2)
    r.c.cmd("scale 120")
    r.sync()
    scr.cw, scr.ch = cw, ch
    step(r, scr, "back", cw=cw, ch=ch)
    r.keys(5, "i")
    snap = r.snap()
    cw3, ch3 = snap.width // COLS, snap.height // ROWS
    scr.cw, scr.ch = cw3, ch3
    step(r, scr, "zoom", cw=cw3, ch=ch3)


def sc_alpha(r):
    """15: pictures with alpha blend over the background and over each
    other."""
    scr = Screen()
    r.sync(START)
    put(r, scr, mkimg(60, 40, 1), 2, 2, i=1, C=1)
    put(r, scr, mkimg(60, 40, 2, alpha=True), 4, 3, i=2, z=1, C=1)
    step(r, scr, "alpha over a picture")
    put(r, scr, mkimg(60, 40, 3, alpha=True), 15, 12, i=3, z=-5, C=1)
    step(r, scr, "alpha over the background")


def sc_cursor(r):
    """16: the cursor and a selection invert what is composed."""
    scr = Screen()
    r.sync(E + "[2J" + E + "[H")
    put(r, scr, mkimg(64, 48, 1), 0, 0, i=1, C=1, z=-1)
    r.sync(E + "[?25h" + cup_s(2, 3))
    img = r.snap().convert("RGB")
    r.snaps["cursor"] = img.tobytes()
    exp = scr.render()
    box = (3 * CW, 2 * CH, 4 * CW, 3 * CH)
    check(ImageChops.invert(exp.crop(box)).tobytes() ==
          img.crop(box).tobytes() or exp.crop(box).tobytes() ==
          img.crop(box).tobytes(), "cursor: cell is neither plain nor "
          "inverted")
    # a drag over the picture
    r.sync(E + "[?25l")
    x0, y0 = 1 * CW + 3, 1 * CH + 5
    r.c.ptr("enter", x0, y0)
    r.c.ptr("motion", x0, y0)
    r.c.ptr("button", 1)
    r.sync()
    for x in range(x0 + CW, 7 * CW + 4, CW):
        r.c.ptr("motion", x, y0)
        r.sync()
    r.c.ptr("button", 0)
    r.sync()
    img = r.snap().convert("RGB")
    exp = scr.render()
    inv = odd = 0
    for c in range(0, 12):
        box = (c * CW, 1 * CH, (c + 1) * CW, 2 * CH)
        a, e = img.crop(box), exp.crop(box)
        if a.tobytes() == e.tobytes():
            continue
        if a.tobytes() == ImageChops.invert(e).tobytes():
            inv += 1
        else:
            odd += 1
    check(odd == 0, "cursor: %d selected cells are neither plain nor inverted" % odd)
    check(inv >= 2, "cursor: %d inverted cells in the selection" % inv)
    r.snaps["selection"] = img.tobytes()


def sc_tools(r):
    """17: the streams of real programs, when they are installed."""
    from shutil import which
    r.sync(START)
    png_path = os.path.join(r.dir, "t.png")
    im = mkimg(66, 50, 4)
    im.convert("RGB").save(png_path)
    n = 0
    if which("chafa"):
        s = subprocess.run(["chafa", "-f", "kitty", "-s", "11x5", png_path],
                           capture_output=True, check=True).stdout
        r.feed(cup_s(0, 0))
        r.mark()
        r.sync(s)
        n += 1
        img = r.snap().convert("RGB")
        check(img.crop((0, 0, 11 * CW, 5 * CH)).getbbox() is not None,
              "tools: chafa drew nothing")
        r.snaps["chafa"] = img.tobytes()
        r.feed(E + "[6n")
        r.sync()
        check(r.since().endswith(b"R"), "tools: chafa cursor %r" % r.since())
        print("note: chafa leaves the cursor at %r" % r.since()[-8:])
    if which("timg"):
        s = subprocess.run(["timg", "-p", "kitty", "-g", "11x5", png_path],
                           capture_output=True, check=True).stdout
        r.feed(cup_s(8, 0))
        r.sync(s)
        n += 1
        img = r.snap().convert("RGB")
        check(img.crop((0, 8 * CH, 11 * CW, 14 * CH)).getbbox() is not None,
              "tools: timg drew nothing")
        r.snaps["timg"] = img.tobytes()
    if which("kitten"):
        try:
            s = subprocess.run(["kitten", "icat", "--stdin=no", "--transfer-mode=stream",
                                "--place=11x5@0x16", png_path],
                               capture_output=True, timeout=10,
                               env=dict(os.environ, TERM="xterm-kitty")).stdout
        except Exception as e:
            print("note: kitten icat: %s" % e)
            s = b""
        if s:
            r.feed(cup_s(16, 0))
            r.sync(s)
            n += 1
            r.snaps["icat"] = r.snap().tobytes()
    if not n:
        print("note: no picture tool installed, skipped")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    G.COMP, G.HAVOC = (os.path.abspath(a) for a in sys.argv[1:3])
    G.SELECTED = set(sys.argv[3:])
    # Run uses G.Run; the scenarios use the helpers of KRun
    G.Run = KRun
    S = lambda name, fn, **kw: G.run_scenario(name, fn, **kw)  # noqa: E731
    S("basic", sc_basic, extra=KCFG)
    S("formats", sc_formats, extra=KCFG)
    S("geometry", sc_geometry, extra=KCFG)
    S("survive", sc_survive, extra=KCFG)
    S("zorder", sc_zorder, extra=KCFG)
    S("scroll", sc_scroll, extra=KCFG)
    S("region", sc_region, extra=KCFG)
    S("erase", sc_erase, extra=KCFG)
    S("delete", sc_delete, extra=KCFG)
    S("anon", sc_anon, extra=KCFG)
    S("memory", sc_memory, extra=KCFG + "memory=1500\n", runs=(False,))
    S("shm", sc_shm, extra=KCFG + "kitty shm=yes\n", runs=(False,))
    S("shm_off", sc_shm_off, extra=KCFG, runs=(False,))
    S("replies", sc_replies, extra=KCFG, runs=(False,))
    S("replies_off", sc_replies_off, extra="kitty=no\n", runs=(False,))
    S("hostile", sc_hostile, extra=KCFG)
    S("resize", sc_resize, extra=KCFG)
    S("scale", sc_scale, extra=KCFG, fractional=True, config=G.ZOOM_CONFIG)
    S("alpha", sc_alpha, extra=KCFG)
    S("cursor", sc_cursor, extra=KCFG)
    S("tools", sc_tools, extra=KCFG)
    print("%s: %d checks, %d failed" % ("PASSED" if not G.failures else
                                        "FAILED", G.checks, len(G.failures)))
    return 1 if G.failures else 0


if __name__ == "__main__":
    sys.exit(main())
