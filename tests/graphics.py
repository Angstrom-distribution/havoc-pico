#!/usr/bin/env python3
"""Window tests for sixel pictures in havoc (HAVOC_SIXEL=1, [graphics] sixel=yes).

    graphics.py FAKE_COMPOSITOR HAVOC [scenario ...]

Each scenario feeds sixel through the pty of a havoc running in the fake
compositor (tests/fake-compositor.c --driver) and compares the window with a
model of what the screen must show: the pictures decoded by the independent
reference decoder (tools/sixel_ref.py), pasted at the cells they were
placed in, with scrolling, erasing and overwriting applied to the model.
Every scenario runs twice, incrementally and with HAVOC_FULL_REDRAW (every
cell painted into a cleared buffer); the windows must be equal after every
step, which is the check of the damage code. The compositor itself fails a
commit that changed pixels outside its damage. The stderr of havoc is
searched for sanitizer reports.
"""
import hashlib
import json
import os
import random
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "tools"))

from PIL import Image, ImageChops  # noqa: E402

import sixel_enc  # noqa: E402
from harness import Comp  # noqa: E402

E = "\x1b"
COLS, ROWS, CW, CH = 40, 32, 6, 10
BG = (0, 0, 0)
SIXEL_REF = os.path.join(HERE, "tools", "sixel_ref.py")
TMP = tempfile.mkdtemp(prefix="gfx-test-")

CONFIG = """[font]
pixel=6x10
[terminal]
columns=40
rows=32
scrollback=100
[colors]
background=#000000
foreground=#ffffff
[bind]
C-S-k=scroll up
C-S-j=scroll down
C-S-End=scroll to bottom
C-S-Home=scroll to top
C-S-i=zoom in
C-S-o=zoom out
[graphics]
sixel=yes
%s"""

# cells of 7 x 13 pixels that scale by whole multiples
ZOOM_CONFIG = CONFIG.replace("pixel=6x10", "pixel=yes\nsize=13")

# the program of the terminal: feeds what the driver sends and keeps what
# the terminal answers in $HAVOC_REPLIES
REPLIES_CHILD = ('stty raw -echo; cat "$HAVOC_CTL" & '
                 'exec cat > "$HAVOC_REPLIES"')

checks = 0
failures = []


def check(cond, msg):
    global checks
    checks += 1
    if not cond:
        failures.append(msg)
        print("FAIL: " + msg, file=sys.stderr)


# ------------------------------------------------------------ the reference

_ref_cache = {}


def ref(stream, max_w):
    """The reference decode of the first string in stream: an RGBA image
    (unset pixels alpha 0) or None."""
    key = (hashlib.md5(stream).hexdigest(), max_w)
    if key not in _ref_cache:
        path = os.path.join(TMP, "ref.bin")
        with open(path, "wb") as f:
            f.write(stream)
        js = os.path.join(TMP, "ref.json")
        subprocess.run([sys.executable, "-I", SIXEL_REF, path, "--json", js,
                        "--max-w", str(max_w)], check=True,
                       stdout=subprocess.DEVNULL)
        d = json.load(open(js))[0]
        img = None
        if d["w"] and d["h"]:
            img = Image.frombytes("RGBA", (d["w"], d["h"]),
                                  open(js + ".0.rgba", "rb").read())
        _ref_cache[key] = img
    return _ref_cache[key]


# ---------------------------------------------------------------- the model

class Model:
    """What the screen shows, as one tall canvas of rows (the scrollback
    above the screen included); rows are absolute, top is the first row of
    the window."""

    def __init__(self, cols=COLS, rows=ROWS, cw=CW, ch=CH, lines=500):
        self.cols, self.rows, self.cw, self.ch = cols, rows, cw, ch
        self.canvas = Image.new("RGB", (cols * cw, lines * ch), BG)
        self.top = 0
        self.low = 0		# the oldest row the scrollback still has
        self.unknown = set()	# (row, col): text, compared with the full redraw

    def clear(self, row, c0, c1, r1=None):
        r1 = row if r1 is None else r1
        self.canvas.paste(BG, (c0 * self.cw, row * self.ch,
                               (c1 + 1) * self.cw, (r1 + 1) * self.ch))
        for r in range(row, r1 + 1):
            for c in range(c0, c1 + 1):
                self.unknown.discard((r, c))

    def pic(self, rgba, col, row, scale=1, avail=None):
        """A decoded picture placed with its top left cell at (col, row);
        avail: the columns the window had there."""
        if scale != 1:
            rgba = rgba.resize((rgba.width * scale, rgba.height * scale),
                               Image.NEAREST)
        avail = self.cols - col if avail is None else avail
        cols = min((rgba.width + self.cw - 1) // self.cw, avail)
        rows = (rgba.height + self.ch - 1) // self.ch
        self.clear(row, col, col + cols - 1, row + rows - 1)
        self.canvas.paste(rgba.convert("RGB"), (col * self.cw, row * self.ch),
                          rgba.getchannel("A"))
        self.top = max(self.top, row + rows - self.rows)
        return rows

    def pic_nn(self, rgba, col, row, cw0, ch0, avail=None):
        """A picture placed with cells of cw0 x ch0 pixels, drawn in cells
        of self.cw x self.ch: each tile is stepped through in 16.16 fixed
        point, nearest neighbour, as the window does."""
        avail = self.cols - col if avail is None else avail
        w, h = rgba.size
        cols = min((w + cw0 - 1) // cw0, avail)
        rows = (h + ch0 - 1) // ch0
        self.clear(row, col, col + cols - 1, row + rows - 1)
        fx = (cw0 << 16) // self.cw
        fy = (ch0 << 16) // self.ch
        src, dst = rgba.load(), self.canvas.load()
        for r in range(rows):
            for c in range(cols):
                for py in range(self.ch):
                    sy = r * ch0 + ((py * fy) >> 16)
                    if sy >= h:
                        break
                    for px in range(self.cw):
                        sx = c * cw0 + ((px * fx) >> 16)
                        if sx >= w:
                            break
                        p = src[sx, sy]
                        if p[3]:
                            dst[(col + c) * self.cw + px,
                                (row + r) * self.ch + py] = p[:3]
        self.top = max(self.top, row + rows - self.rows)

    def text(self, row, c0, c1):
        for c in range(c0, c1 + 1):
            self.unknown.add((row, c))

    def scroll(self, n):
        """Line feeds at the bottom of the screen."""
        self.top += n

    def clear_screen(self):
        self.clear(self.top, 0, self.cols - 1, self.top + self.rows - 1)

    def clear_scrollback(self):
        if self.top:
            self.clear(0, 0, self.cols - 1, self.top - 1)
        self.low = self.top

    def shift(self, t, b, n, up=True):
        """Rows t..b of the screen (0-based) move up or down by n; what
        enters is blank."""
        r0, r1 = self.top + t, self.top + b
        reg = self.canvas.crop((0, r0 * self.ch, self.canvas.width,
                                (r1 + 1) * self.ch))
        blank = Image.new("RGB", reg.size, BG)
        h = n * self.ch
        if up:
            blank.paste(reg.crop((0, h, reg.width, reg.height)), (0, 0))
        else:
            blank.paste(reg.crop((0, 0, reg.width, reg.height - h)), (0, h))
        self.canvas.paste(blank, (0, r0 * self.ch))

    def invert(self, row, col):
        box = (col * self.cw, row * self.ch, (col + 1) * self.cw,
               (row + 1) * self.ch)
        self.canvas.paste(ImageChops.invert(self.canvas.crop(box)), box)

    def view(self, cols=None):
        cols = self.cols if cols is None else cols
        return self.canvas.crop((0, self.top * self.ch, cols * self.cw,
                                 (self.top + self.rows) * self.ch))


# ---------------------------------------------------------------- one run

class Run:
    def __init__(self, full, extra="", **kw):
        self.full = full
        self.dir = tempfile.mkdtemp(prefix="gfx-run-", dir=TMP)
        self.errpath = os.path.join(self.dir, "stderr")
        self.err = open(self.errpath, "w")
        self.replies_path = os.path.join(self.dir, "replies")
        env = dict(HAVOC_GFX_DEBUG="1", HAVOC_REPLIES=self.replies_path,
                   **kw.pop("env", {}))
        child = kw.pop("child", REPLIES_CHILD)
        self.size_path = os.path.join(self.dir, "ptysize")
        if kw.pop("ptysize", False):
            env["HAVOC_PTYSIZE"] = self.size_path
            child = 'exec python3 -I "%s" "$HAVOC_PTYSIZE"' % os.path.join(
                HERE, "tools", "ptysize.py")
        cfg = kw.pop("config", CONFIG) % extra
        self.c = Comp(COMP, HAVOC, cfg, full_redraw=full, cols=COLS,
                      rows=ROWS, env=env, stderr=self.err, child=child, **kw)
        self.snaps = {}
        self.w, self.h = self.c.w, self.c.h

    def feed(self, data):
        self.c.feed(data)

    def sync(self, data=b""):
        if data:
            self.c.feed(data)
        return self.c.wait()

    def snap(self):
        path = os.path.join(self.dir, "snap.raw")
        w, h = self.c.snap(path)
        raw = open(path, "rb").read()
        return Image.frombuffer("RGBA", (w, h), raw, "raw", "BGRA", 0, 1)

    def replies(self):
        try:
            return open(self.replies_path, "rb").read()
        except FileNotFoundError:
            return b""

    def debug(self):
        """The last gfx debug line as numbers."""
        last = None
        for l in open(self.errpath):
            if l.startswith("gfx: "):
                last = l
        if not last:
            return None
        m = re.match(r"gfx: sixel=(\d+) kitty=(\d+) places=(\d+)\+(\d+) "
                     r"pixels=(\d+)/(\d+)", last)
        return dict(zip(("sixel", "kitty", "pm", "pa", "pixels", "cap"),
                        (int(v) for v in m.groups()))) if m else None

    def check(self, label, model, cols=None, ignore=()):
        """The window must equal the model (outside text cells) and, across
        the two runs of a scenario, the other run."""
        img = self.snap()
        self.snaps[label] = img.tobytes()
        if self.full:
            return
        exp = model.view(cols)
        act = img.convert("RGB")
        if act.size != exp.size:
            check(False, "%s: %s: window %s, model %s" %
                  (self.name, label, act.size, exp.size))
            return
        diff = ImageChops.difference(act, exp)
        for (r, c) in list(model.unknown) + list(ignore):
            r -= model.top
            if 0 <= r < model.rows and 0 <= c < (cols or model.cols):
                diff.paste(BG, (c * model.cw, r * model.ch,
                                (c + 1) * model.cw, (r + 1) * model.ch))
        bb = diff.getbbox()
        check(bb is None, "%s: %s: window differs from the model at %s "
              "(cell %s)" % (self.name, label, bb,
                             bb and (bb[0] // model.cw, bb[1] // model.ch)))

    def close(self):
        s = self.c.stats()
        what = "%s (%s)" % (self.name, "full" if self.full else "incremental")
        check(s["stale"] == "0", "%s: commits left pixels outside the damage"
              % what)
        check(s["held_writes"] == "0", "%s: write to a held buffer" % what)
        check(s["no_full"] == "0", "%s: new buffer size without full damage"
              % what)
        check(s["failed"] == "0", "%s: the compositor failed" % what)
        rc = self.c.close()
        self.err.close()
        err = open(self.errpath).read()
        bad = re.search(r"AddressSanitizer|runtime error|LeakSanitizer", err)
        check(not bad, "%s: sanitizer report in stderr: %s" %
              (what, err[bad.start():bad.start() + 600] if bad else ""))
        check(rc == 0, "%s: compositor exit status %s" % (what, rc))

    def alive(self):
        """havoc still draws."""
        self.feed(E + "[H" + "alive")
        got = self.sync()
        return bool(got)

    def keys(self, mods, text):
        self.c.cmd("mods %d" % mods)
        self.c.keys(text)
        self.c.cmd("mods 0")
        return self.sync()

    def scroll_back(self, m, n):
        """C-S-k n times; the model's window follows."""
        for _ in range(n):
            if m.top > m.low:
                m.top -= 1
        self.c.cmd("mods 5")
        for _ in range(n):
            self.c.keys("k")
        self.c.cmd("mods 0")
        self.sync()

    def scroll_bottom(self, m, top):
        m.top = top
        self.c.cmd("mods 5")
        self.c.cmd("key 107")	# End
        self.c.cmd("mods 0")
        self.sync()

    def scroll_top(self, m):
        m.top = m.low
        self.c.cmd("mods 5")
        self.c.cmd("key 102")	# Home
        self.c.cmd("mods 0")
        self.sync()


def run_scenario(name, fn, extra="", runs=(False, True), **kw):
    """fn(run) twice; the windows after each step must be equal."""
    if SELECTED and name not in SELECTED:
        return
    print("scenario %s" % name, flush=True)
    all_snaps = []
    for full in runs:
        r = Run(full, extra, **dict(kw))
        r.name = name
        try:
            fn(r)
        except Exception as e:  # a failure of the scenario itself
            import traceback
            traceback.print_exc()
            check(False, "%s: %s" % (name, e))
        r.close()
        all_snaps.append(r.snaps)
    if len(all_snaps) == 2:
        a, b = all_snaps
        check(a.keys() == b.keys(), "%s: steps differ between runs" % name)
        for k in a:
            check(a[k] == b.get(k), "%s: %s: incremental window differs from "
                  "the full redraw" % (name, k))


# --------------------------------------------------------------- pictures

def picture(seed, w=64, h=48, **kw):
    """(stream, image) of a random block picture of w x h pixels."""
    return sixel_enc.encode(sixel_enc.pattern(w, h, seed), **kw)


def cup(m, row, col):
    """Cursor to the absolute row and column of the model."""
    return E + "[%d;%dH" % (row - m.top + 1, col + 1)


def place(r, m, stream, col, row, avail=None):
    """Feed the picture with its top left cell at (col, row), and put it in
    the model. avail: the columns right of the cursor when the window is
    narrower than the model. Returns its rows."""
    avail = m.cols - col if avail is None else avail
    r.feed(cup(m, row, col))
    r.feed(stream)
    r.sync()
    return m.pic(ref(stream, avail * m.cw), col, row, avail=avail)


START = (E + "[?25l" + E + "[2J" + E + "[H").encode()


def lf(n):
    return b"\n" * n


def at_bottom(m):
    return (E + "[%d;1H" % m.rows).encode()


# -------------------------------------------------------------- scenarios

def sc_basic(r):
    """1: a picture at the prompt, cursor on its last row."""
    m = Model()
    r.sync(START)
    s = picture(1)
    place(r, m, s, 0, 0)
    r.check("picture", m)
    r.feed(E + "[6n")
    r.sync()
    # 64 x 48 pixels in 6 x 10 cells: 11 x 5, cursor on row 5, column 1
    check(r.replies().endswith(b"\x1b[5;1R"),
          "basic: cursor report %r" % r.replies())
    d = r.debug()
    check(d and d["sixel"] == 1 and d["pixels"] == 64 * 48,
          "basic: debug line %r" % d)
    # a picture at a column: clipped by the right margin
    wide = picture(2, 300, 20)
    place(r, m, wide, 30, 8)
    r.check("clipped", m)
    # P1 = 0 and the default aspect: sixel rows are two pixels tall
    p0 = sixel_enc.encode(sixel_enc.pattern(30, 12, 3), p1=0, p2=0, raster=False)
    place(r, m, p0, 12, 12)
    r.check("aspect", m)


def sc_scroll(r):
    """2: line feeds, the scrollback and back."""
    m = Model()
    r.sync(START)
    place(r, m, picture(1), 0, 0)
    place(r, m, picture(2, 40, 30), 20, 3)
    r.check("placed", m)
    r.feed(at_bottom(m))
    for n in (1, 4, 5, 6):
        r.sync(lf(n))
        m.scroll(n)
        r.check("lf%d" % n, m)
    # the pictures are in the scrollback now
    r.scroll_back(m, 14)
    r.check("scrollback", m)
    r.scroll_back(m, 5)
    r.check("scrollback more", m)
    r.scroll_bottom(m, 16)
    r.check("bottom", m)
    # reverse index and scroll down at the top: the screen content moves
    r.sync(cup(m, m.top, 0) + E + "M")
    r.feed(E + "[2T")
    r.sync()
    m.shift(0, ROWS - 1, 1, up=False)
    m.shift(0, ROWS - 1, 2, up=False)
    r.check("scroll down", m)
    r.sync(E + "[3S")
    m.scroll(3)
    r.check("scroll up", m)
    # a picture that is higher than the room below the cursor
    m2 = Model()
    r.sync(START)
    r.sync(b"\x1b[3J")
    m2.top = 0
    r.check("cleared", m2)
    tall = picture(5, 60, 200)
    place(r, m2, tall, 4, 20)
    r.check("tall", m2)
    d = r.debug()
    check(d and d["sixel"] >= 1, "scroll: debug %r" % d)


def sc_region(r):
    """2b: a scroll region: pictures inside move, the rest stays."""
    m = Model()
    r.sync(START)
    place(r, m, picture(1), 0, 0)		# rows 0..4, outside
    place(r, m, picture(2, 60, 30), 20, 8)	# rows 8..10, inside
    r.check("placed", m)
    # region rows 6..20 (1-based), cursor to its last row, two line feeds;
    # the screen's first rows leave nowhere and the region's rows move up
    r.feed(E + "[6;20r" + E + "[20;1H")
    r.sync(lf(2))
    m.shift(5, 19, 2, up=True)
    r.check("region lf", m)
    r.feed(E + "[6;1H" + E + "M")
    r.sync()
    m.shift(5, 19, 1, up=False)
    r.check("region ri", m)
    # insert and delete lines
    r.sync(cup(m, 8, 0) + E + "[3L")
    m.shift(8, 19, 3, up=False)
    r.check("insert lines", m)
    r.sync(cup(m, 8, 0) + E + "[2M")
    m.shift(8, 19, 2, up=True)
    r.check("delete lines", m)
    r.sync(E + "[r")


def sc_erase(r):
    """3: text over a tile row, erases, overwrite, reset."""
    m = Model()
    r.sync(START)
    place(r, m, picture(1, 66, 60), 2, 2)	# 11 x 6 cells at rows 2..7
    r.check("placed", m)
    # text over one tile row
    r.sync(cup(m, 4, 2) + "hello tile")
    m.text(4, 2, 11)
    r.check("text", m)
    # erase to the end of the line, from the middle of the picture
    r.sync(cup(m, 5, 6) + E + "[K")
    m.clear(5, 6, COLS - 1)
    r.check("el", m)
    r.sync(cup(m, 6, 4) + E + "[3X")
    m.clear(6, 4, 6)
    r.check("ech", m)
    r.sync(cup(m, 7, 4) + E + "[2P")
    # the rest of the line moves left by two cells
    seg = m.canvas.crop((6 * CW, 7 * CH, COLS * CW, 8 * CH))
    m.canvas.paste(seg, (4 * CW, 7 * CH))
    m.clear(7, COLS - 2, COLS - 1)
    r.check("dch", m)
    # a second picture over part of the first
    place(r, m, picture(3, 48, 40), 7, 4)
    r.check("overwrite", m)
    # insert characters push tiles right
    r.sync(cup(m, 3, 2) + E + "[2@")
    seg = m.canvas.crop((2 * CW, 3 * CH, (COLS - 2) * CW, 4 * CH))
    m.canvas.paste(seg, (4 * CW, 3 * CH))
    m.clear(3, 2, 3)
    r.check("ich", m)
    # ED 2
    r.sync(E + "[2J")
    m.clear_screen()
    r.check("ed2", m)
    place(r, m, picture(4), 5, 5)
    # ED 1 clears from the start through the cursor
    r.sync(cup(m, 5, 0) + E + "[1J")
    m.clear(m.top, 0, COLS - 1, 4)
    m.clear(5, 0, 0)
    r.check("ed1", m)
    place(r, m, picture(4), 5, 9)
    r.sync(E + "[3J")
    r.check("ed3", m)
    # a hard reset
    place(r, m, picture(6), 3, 14)
    r.sync(E + "c" + E + "[?25l")
    m.clear_screen()
    r.check("ris", m)
    d = r.debug()
    check(d and d["sixel"] == 0 and d["pixels"] == 0,
          "erase: store not empty after reset: %r" % d)


def sc_scrollback_clear(r):
    """3b: ED 3 with a picture in the scrollback."""
    m = Model()
    r.sync(START)
    place(r, m, picture(1), 0, 0)
    r.feed(at_bottom(m))
    r.sync(lf(20))
    m.scroll(20)
    r.check("scrolled", m)
    r.scroll_back(m, 20)
    r.check("in scrollback", m)
    r.scroll_bottom(m, 20)
    r.sync(E + "[3J")
    m.clear_scrollback()
    r.check("ed3", m)
    r.scroll_back(m, 5)
    r.check("nothing above", m)
    d = r.debug()
    check(d and d["sixel"] == 0, "scrollback: slot not freed %r" % d)


def sc_alt(r):
    """4: the alternate screen."""
    main = Model()
    alt = Model()
    r.sync(START)
    place(r, main, picture(1), 0, 0)
    r.check("main", main)
    r.sync(E + "[?1049h" + E + "[?25l")
    place(r, alt, picture(2), 10, 10)
    r.check("alt", alt)
    d = r.debug()
    check(d and d["sixel"] == 2, "alt: debug %r" % d)
    # a second picture on top, scrolling the alternate screen
    r.feed(at_bottom(alt))
    r.sync(lf(3))
    alt.scroll(3)
    r.check("alt scrolled", alt)
    r.sync(E + "[?1049l")
    r.check("main again", main)
    d = r.debug()
    check(d and d["sixel"] == 1 and d["pixels"] == 64 * 48,
          "alt: slot of the alternate screen not freed: %r" % d)
    # back in: the alternate screen is empty; a picture on it is dropped
    # again when it is left, ?47 does not keep it either
    alt2 = Model()
    r.sync(E + "[?1049h" + E + "[?25l")
    r.check("alt empty", alt2)
    place(r, alt2, picture(3), 2, 2)
    r.check("alt again", alt2)
    r.sync(E + "[?47l")
    r.check("47 main", main)
    r.sync(E + "[?47h")
    r.check("47 alt", Model())
    r.sync(E + "[?47l")
    d = r.debug()
    check(d and d["sixel"] == 1, "alt: slots after leaving %r" % d)


def sc_resize(r):
    """5: narrower and back; the hidden columns return."""
    m = Model()
    r.sync(START)
    place(r, m, picture(1), 25, 4)	# columns 25..35
    place(r, m, picture(2), 0, 12)
    r.check("full", m)
    r.c.cmd("configure 180 320")
    r.sync()
    check(r.snap().size == (180, 320), "resize: window %s" % (r.snap().size,))
    r.check("narrow", m, cols=30)
    r.c.cmd("configure 240 320")
    r.sync()
    r.check("wide again", m)
    # a picture placed while narrow is clipped by the window, and its
    # hidden columns do not come back as a picture of another size
    r.c.cmd("configure 180 320")
    r.sync()
    place(r, m, picture(3), 28, 20, avail=2)
    r.check("placed narrow", m, cols=30)
    r.c.cmd("configure 240 320")
    r.sync()
    r.check("wide after clip", m)


def sc_scale(r):
    """5b: a bigger cell scales the pictures by whole multiples."""
    cw, ch = r.w // COLS, r.h // ROWS
    m = Model(cw=cw, ch=ch)
    r.sync(START)
    place(r, m, picture(1), 3, 2)
    r.check("scale 1", m)
    r.c.cmd("scale 240")
    r.sync()
    # the next pixel font: 10 x 20 cells for 7 x 13
    snap = r.snap()
    cw2, ch2 = snap.width // COLS, snap.height // ROWS
    check(snap.size == (COLS * cw2, ROWS * ch2) and (cw2, ch2) != (cw, ch),
          "scale: window %s" % (snap.size,))
    m2 = Model(cw=cw2, ch=ch2)
    pic1 = ref(picture(1), (COLS - 3) * cw)
    m2.pic_nn(pic1, 3, 2, cw, ch)
    r.check("scaled", m2)
    # a picture placed at the bigger size, then back to the small cell
    pic2 = ref(picture(2, 50, 30), (COLS - 20) * cw2)
    place(r, m2, picture(2, 50, 30), 20, 20)
    r.check("scale new", m2)
    r.c.cmd("scale 120")
    r.sync()
    m.pic_nn(pic2, 20, 20, cw2, ch2)
    r.check("scale back", m)
    # zoom keys: one pixel more in height
    r.keys(5, "i")
    check(r.snap().height != r.h or r.snap().width != r.w, "zoom changed nothing")
    m3 = Model(cw=r.snap().width // COLS, ch=r.snap().height // ROWS)
    m3.pic_nn(pic1, 3, 2, cw, ch)
    m3.pic_nn(pic2, 20, 20, cw2, ch2)
    r.check("zoom in", m3)


def sc_cursor(r):
    """6: the cursor and a selection invert what is composed."""
    m = Model()
    r.sync(E + "[2J" + E + "[H")
    place(r, m, picture(1), 0, 0)
    r.sync(cup(m, 2, 3))	# the cursor over a tile, visible
    m.invert(2, 3)
    r.check("cursor", m)
    r.sync(cup(m, 20, 20))
    m.invert(2, 3)
    m.invert(20, 20)
    r.check("cursor moved", m)
    # a drag over the picture
    r.sync(E + "[?25l")
    m.invert(20, 20)
    x0, y0 = 1 * CW + 3, 1 * CH + 5
    x1, y1 = 7 * CW + 3, 1 * CH + 5
    r.c.ptr("enter", x0, y0)
    r.c.ptr("motion", x0, y0)
    r.c.ptr("button", 1)
    r.sync()
    for x in range(x0 + CW, x1 + 1, CW):
        r.c.ptr("motion", x, y1)
        r.sync()
    r.c.ptr("button", 0)
    r.sync()
    img = r.snap().convert("RGB")
    exp = m.view()
    inv = 0
    odd = []
    for c in range(0, 12):
        box = (c * CW, 1 * CH, (c + 1) * CW, 2 * CH)
        a = img.crop(box)
        e = exp.crop(box)
        if a.tobytes() == e.tobytes():
            continue
        if a.tobytes() == ImageChops.invert(e).tobytes():
            inv += 1
        else:
            odd.append(c)
    check(not odd, "cursor: selected cells %s are neither plain nor "
          "inverted" % odd)
    check(inv >= 2, "cursor: %d inverted cells in the selection" % inv)
    r.snaps["selection"] = img.tobytes()
    # the rows around the selection are untouched
    check(img.crop((0, 0, 12 * CW, CH)).tobytes() ==
          exp.crop((0, 0, 12 * CW, CH)).tobytes(), "cursor: row above")
    check(img.crop((0, 2 * CH, 12 * CW, 5 * CH)).tobytes() ==
          exp.crop((0, 2 * CH, 12 * CW, 5 * CH)).tobytes(), "cursor: rows below")


def sc_sgr(r):
    """6b: the SGR attributes in force when a picture arrives do not
    change it; inverse would otherwise invert it for good."""
    m = Model()
    r.sync(START)
    r.feed(E + "[7;4;5m")
    place(r, m, picture(1), 0, 0)
    r.feed(E + "[0m")
    r.check("sgr", m)


def sc_wide(r):
    """7: a wide character over tiles and at the picture's edges."""
    m = Model()
    r.sync(START)
    place(r, m, picture(1, 66, 50), 5, 2)		# columns 5..15, rows 2..6
    r.check("placed", m)
    # inside
    r.sync(cup(m, 3, 8) + "世")
    m.text(3, 8, 9)
    # over the first column: the wide cell is columns 4 and 5
    r.sync(cup(m, 4, 4) + "世")
    m.text(4, 4, 5)
    # over the last column: columns 15 and 16
    r.sync(cup(m, 5, 15) + "世")
    m.text(5, 15, 16)
    r.check("wide", m)
    # a picture over the right half of a wide character: the left half is
    # blanked, not left as half a glyph
    r.sync(cup(m, 9, 20) + "世世世")
    m.text(9, 20, 25)
    place(r, m, picture(2, 30, 20), 21, 9)
    m.clear(9, 20, 20)
    r.check("picture over wide", m)
    # over the left half: the right half is blanked
    r.sync(cup(m, 14, 30) + "世")
    place(r, m, picture(4, 30, 10), 26, 14)
    m.clear(14, 31, 31)
    r.check("picture ends on wide", m)
    d = r.debug()
    check(d and d["sixel"] == 3, "wide: debug %r" % d)


def sc_right(r):
    """8: ?8452 puts the cursor right of the picture."""
    m = Model()
    r.sync(START)
    r.feed(E + "[?8452h")
    place(r, m, picture(1), 0, 0)
    r.check("right", m)
    r.feed(E + "[6n")
    r.sync()
    check(r.replies().endswith(b"\x1b[5;12R"), "right: cursor %r" %
          r.replies())
    # past the right margin the cursor wraps to the next line
    place(r, m, picture(2, 300, 12), 36, 10)
    r.feed(E + "[6n")
    r.sync()
    check(r.replies().endswith(b"\x1b[13;1R"), "right: wrap %r" %
          r.replies())
    r.feed(E + "[?8452l")
    place(r, m, picture(3), 0, 20)
    r.feed(E + "[6n")
    r.sync()
    check(r.replies().endswith(b"\x1b[25;1R"), "right: reset %r" %
          r.replies())
    r.check("right again", m)


def sc_memory(r):
    """9: the pixel cap: the oldest picture that is not in view goes."""
    m = Model()
    r.sync(START)
    # 9 x 5 cells at columns 30 to 38: a decoder canvas of 60 x 48 pixels
    # in the 10 columns right of the cursor, 54 x 48 stored
    streams = [picture(10 + i, 54, 48) for i in range(8)]
    for i in range(6):
        place(r, m, streams[i], 30, 5 * i)
    r.check("six", m)
    d = r.debug()
    check(d and d["sixel"] == 6 and d["pixels"] == 6 * 2592,
          "memory: six pictures %r" % d)
    # all six are in view (rows 0..29): a seventh does not fit and is
    # dropped; the cursor stays where it was
    r.sync(cup(m, 31, 30))
    before = len(r.replies())
    r.feed(streams[6])
    r.feed(E + "[6n")
    r.sync()
    check(r.replies()[before:] == b"\x1b[32;31R", "memory: cursor after a "
          "refused picture %r" % r.replies()[before:])
    d = r.debug()
    check(d and d["sixel"] == 6, "memory: refused picture stored %r" % d)
    r.check("refused", m)
    # scroll 11 lines: the first two pictures are in the scrollback only
    r.feed(at_bottom(m))
    r.sync(lf(11))
    m.scroll(11)
    place(r, m, streams[6], 30, 42)
    d = r.debug()
    check(d and d["sixel"] == 6 and d["pixels"] == 6 * 2592,
          "memory: after eviction %r" % d)
    r.check("seventh", m)
    # the first (oldest) picture is gone, the second is not
    m.clear(0, 30, 38, 4)
    bottom = m.top
    r.scroll_top(m)
    r.check("scrollback after eviction", m)
    # a picture larger than the cap is dropped, the cursor stays
    r.scroll_bottom(m, bottom)
    r.feed(E + "[%d;31H" % ROWS)
    before = len(r.replies())
    r.feed(picture(30, 54, 140))
    r.feed(E + "[6n")
    r.sync()
    check(r.replies()[before:] == b"\x1b[32;31R", "memory: cursor after a "
          "picture over the cap %r" % r.replies()[before:])


def sc_truncated(r):
    """12: a string cut short by an escape sequence."""
    m = Model()
    r.sync(START)
    s = picture(1, st=False)
    r.sync(s + (E + "[0m").encode() + b"ok")
    m.text(0, 0, 1)
    r.check("cut", m)
    d = r.debug()
    check(d is None or d["sixel"] == 0, "truncated: picture stored %r" % d)
    # an ESC P q inside a string: the first is dropped, the second shown
    s2 = picture(2)
    r.feed(cup(m, 3, 0))
    r.sync(s[:200] + s2)
    m.pic(ref(s2, COLS * CW), 0, 3)
    r.check("restart", m)
    # CAN and SUB cut it too
    r.feed(cup(m, 12, 0))
    r.sync(s[:300] + b"\x18" + b"x")
    m.text(12, 0, 0)
    r.feed(cup(m, 13, 0))
    r.sync(s[:300] + b"\x1a")
    m.text(13, 0, 0)
    r.check("can sub", m)


def sc_replies(r):
    """10: the answers to the queries of picture programs."""
    ver = subprocess.run([HAVOC, "-v"], capture_output=True,
                         text=True).stdout.split()[1]
    r.sync(START)
    r.feed(E + "[c" + E + "[?1;1;0S" + E + "[?1;3;0S" + E + "[?2;1;0S"
           + E + "[?3;1;0S" + E + "[14t" + E + "[16t" + E + "[18t" + E + "[>q"
           + E + "[5n")
    r.sync()
    want = (b"\x1b[?62;1;4;6;9;15c" b"\x1b[?1;0;256S" b"\x1b[?1;3;256S"
            b"\x1b[?2;0;240;320S" b"\x1b[?3;1;0S" b"\x1b[4;320;240t"
            b"\x1b[6;10;6t" b"\x1b[8;32;40t" +
            b"\x1bP>|havoc(" + ver.encode() + b")\x1b\\" + b"\x1b[0n")
    check(r.replies() == want, "replies: %r instead of %r" %
          (r.replies(), want))
    # after a change of the cell size the answers follow
    r.c.cmd("configure 180 320")
    r.sync()
    n = len(r.replies())
    r.sync(E + "[14t" + E + "[18t")
    check(r.replies()[n:] == b"\x1b[4;320;180t\x1b[8;32;30t",
          "replies: after resize %r" % r.replies()[n:])
    r.snaps["done"] = b""


def sc_replies_off(r):
    """10b: with sixel=no nothing is announced and `CSI ? S` is old."""
    r.sync(START)
    r.feed(E + "[c" + E + "[14t" + E + "[16t" + E + "[18t" + E + "[>q" + E + "[5n")
    r.sync()
    check(r.replies() == b"\x1b[?62;1;6;9;15c\x1b[0n",
          "replies off: %r" % r.replies())
    # a picture is ignored up to its end and the text after it shows
    m = Model()
    r.sync(picture(1) + b"ok")
    m.text(0, 0, 1)
    r.check("ignored", m)
    r.snaps["done"] = b""


def sc_ptysize(r):
    """11: the pty size in pixels."""
    path = r.size_path
    lines = lambda: [tuple(int(v) for v in l.split())
                     for l in open(path)]  # noqa: E731
    cw, ch = r.w // COLS, r.h // ROWS
    r.sync()
    check(lines()[-1] == (32, 40, 40 * cw, 32 * ch), "ptysize: start %r" %
          lines())
    r.c.cmd("configure %d %d" % (30 * cw, 32 * ch))
    r.sync()
    check(lines()[-1] == (32, 30, 30 * cw, 32 * ch), "ptysize: narrow %r" %
          lines())
    r.c.cmd("configure %d %d" % (40 * cw, 20 * ch))
    r.sync()
    check(lines()[-1] == (20, 40, 40 * cw, 20 * ch), "ptysize: lower %r" %
          lines())
    # a window of one row: zoom in changes the cell from 7 x 13 to 7 x 14
    # and neither the columns nor the rows
    r.c.cmd("configure %d %d" % (40 * cw, 20))
    r.sync()
    check(lines()[-1] == (1, 40, 40 * cw, ch), "ptysize: one row %r" %
          lines())
    n = len(lines())
    r.keys(5, "i")
    check(len(lines()) == n + 1 and lines()[-1] == (1, 40, 40 * cw, ch + 1),
          "ptysize: zoom %r" % lines())


def sc_ptysize_off(r):
    """11b: without sixel the pixel fields stay 0."""
    path = r.size_path
    lines = lambda: [tuple(int(v) for v in l.split())
                     for l in open(path)]  # noqa: E731
    cw, ch = r.w // COLS, r.h // ROWS
    r.sync()
    check(lines()[-1] == (32, 40, 0, 0), "ptysize off: start %r" % lines())
    r.c.cmd("configure %d %d" % (30 * cw, 32 * ch))
    r.sync()
    check(lines()[-1] == (32, 30, 0, 0), "ptysize off: narrow %r" % lines())
    r.c.cmd("configure %d %d" % (40 * cw, 20))
    r.sync()
    n = len(lines())
    r.keys(5, "i")
    check(len(lines()) == n and lines()[-1] == (1, 40, 0, 0),
          "ptysize off: zoom %r" % lines())


def sc_damage(r):
    """2c: the damage of changes below a picture stays below it."""
    m = Model()
    r.sync(START)
    place(r, m, picture(1), 0, 0)		# rows 0..4
    r.sync(cup(m, 20, 0) + "some text")
    got = r.sync(b"\nmore text\n")
    n = 0
    for c in got:
        for (x, y, w, h) in c.rects:
            n += 1
            check(y >= 15 * CH or c.full,
                  "damage: rectangle %s reaches the picture" % ((x, y, w, h),))
    check(n > 0, "damage: no commit")
    # a picture appearing damages its own rows
    r.feed(cup(m, 10, 10))
    got = r.sync(picture(2))
    check(got, "damage: no commit for a picture")
    # text over the picture damages the picture's row only
    got = r.sync(cup(m, 2, 0) + "x")
    for c in got:
        for (x, y, w, h) in c.rects:
            check(c.full or y + h <= 4 * CH, "damage: text over a tile "
                  "reaches %s" % ((x, y, w, h),))
    r.snaps["done"] = r.snap().tobytes()


def sc_hostile(r):
    """Hostile streams: nothing crashes, the store stays within its cap."""
    rnd = random.Random(7)
    r.sync(START)
    streams = [
        b"\x1bPq" + bytes(rnd.choice(b'#!$-?~0123456789;"@ABCDEF') for _ in range(60000)) + b"\x1b\\",
        b"\x1bPq!4000000000~\x1b\\",
        b'\x1bPq"1;1;60000;60000#0;2;100;0;0~\x1b\\',
        b'\x1bPq"60000;60000;60000;60000!100000~~\x1b\\',
        b"\x1bPq#300;2;1;2;3#44~-#1;1;999;999;999~$-~\x1b\\",
        b"\x1bPq#0;2;100;100;100" + b"!65535~" * 200 + b"\x1b\\",
        b"\x1bP9;1;9;9;9;9q~~~~~\x1b\\",
        b"\x1bP>q~~\x1b\\", b"\x1bP$q~~\x1b\\", b"\x1bP1;2;3;4;5;6;7;8;9;10;11q~\x1b\\",
        b"\x1bPq" + b"~" * 50 + b"\x1b" + b"\x1bPq~~\x1b\\" + b"\x1b\\",
        b"\x1bPq\xc3\xa9~~\xe2\x82\xac~\x9c",
        b"\x1bPq~~\xc2\x9cabc",
    ]
    for i, s in enumerate(streams):
        r.feed(s)
        r.sync(b"\x1b[0m\x1bc\x1b[?25l")
        check(r.alive(), "hostile: havoc stopped after stream %d" % i)
    # slot exhaustion: more pictures than slots, each one cell
    flood = b"".join(b"\x1b[%d;%dH\x1bPq~\x1b\\" % (1 + i % 32, 1 + i // 32 % 40)
                     for i in range(1200))
    r.feed(flood)
    r.sync()
    d = r.debug()
    check(d and d["sixel"] <= 255, "hostile: slots %r" % d)
    r.sync(b"\x1b[2J")
    # pictures of one cell each, scrolled out and in with the cap in force
    r.feed(b"".join(b"\x1bPq~~~~~~\x1b\\\n" for _ in range(400)))
    r.sync()
    # a tall valid picture scrolling through the scrollback limit
    tall = sixel_enc.encode(sixel_enc.pattern(200, 3000, 4))
    r.feed(tall)
    r.sync()
    d = r.debug()
    check(d and d["pixels"] <= d["cap"], "hostile: over the cap %r" % d)
    # random bytes
    for i in range(30):
        n = rnd.randrange(1, 4000)
        data = bytes(rnd.randrange(256) for _ in range(n))
        r.feed(b"\x1bPq" + data if i % 2 else data)
    r.sync(b"\x1b\\\x1b[0m")
    check(r.alive(), "hostile: havoc stopped after random bytes")
    d = r.debug()
    check(d is None or d["pixels"] <= d["cap"], "hostile: final %r" % d)
    r.sync(b"\x1bc\x1b[?25l")
    r.snaps["done"] = r.snap().tobytes()
    d = r.debug()
    check(d and d["sixel"] == 0 and d["pixels"] == 0,
          "hostile: the store holds pictures after a hard reset %r" % d)


def sc_real(r):
    """Realistic streams: img2sixel and the captured vectors, shown
    pixel for pixel like the reference decodes them."""
    vec = os.path.join(HERE, "vectors", "sixel")
    names = ["img2sixel-grad.bin", "chafa-sixel-grad.bin", "timg-sixel.bin",
             "img2sixel-alpha.bin", "chafa-sixel-alpha.bin",
             "mpv-sixel.bin"]
    for n in names:
        data = open(os.path.join(vec, n), "rb").read()
        m0 = re.search(rb"\x1bP[0-9;]*q.*?\x1b\\", data, re.S)
        check(m0 is not None, "real: no sixel string in %s" % n)
        if not m0:
            continue
        s = m0.group(0)
        # a string cut by a second ESC P q (timg): the last one is drawn
        last = b"\x1bP" + s.split(b"\x1bP")[-1]
        img = ref(last, COLS * CW)
        check(img is not None, "real: nothing drawn for %s" % n)
        if img is None:
            continue
        r.sync(START)
        m = Model()
        r.feed(cup(m, 0, 0))
        r.sync(s)
        m.pic(img, 0, 0)
        r.check(n, m)


def sc_opacity(r):
    """ARGB: picture pixels are opaque, unset ones show the translucent
    background (premultiplied)."""
    m = Model()
    r.sync(START)
    s = picture(1)
    place(r, m, s, 0, 0)
    exp = ref(s, COLS * CW)
    img = r.snap()
    bad = []
    for y in range(5 * CH):
        for x in range(11 * CW):
            want = (0, 0, 0, 128)
            if x < exp.width and y < exp.height and exp.getpixel((x, y))[3]:
                want = exp.getpixel((x, y))[:3] + (255,)
            if img.getpixel((x, y)) != want:
                bad.append((x, y, img.getpixel((x, y)), want))
    check(not bad, "opacity: %d pixels differ, first %r" % (len(bad), bad[:3]))
    r.snaps["done"] = img.tobytes()
    # the cursor over a tile: composed pixels a - c (premultiplied)
    r.sync(E + "[?25h" + cup(m, 1, 1))
    img2 = r.snap()
    for y in range(CH, 2 * CH):
        for x in range(CW, 2 * CW):
            a, b = img.getpixel((x, y)), img2.getpixel((x, y))
            want = (a[3] - a[0], a[3] - a[1], a[3] - a[2], a[3])
            if b != want:
                bad.append((x, y, b, want))
    check(not bad, "opacity: inverted tile differs: %r" % (bad[:3],))


def sc_img2sixel(r):
    """A picture through img2sixel, when it is installed."""
    from shutil import which
    if not which("img2sixel"):
        print("note: img2sixel is not installed, skipped")
        return
    png = os.path.join(r.dir, "grad.png")
    im = Image.new("RGB", (64, 48))
    for y in range(48):
        for x in range(64):
            im.putpixel((x, y), (x * 4, y * 5, 255 - x * 4))
    im.save(png)
    s = subprocess.run(["img2sixel", png], capture_output=True,
                       check=True).stdout
    m = Model()
    r.sync(START)
    place(r, m, s, 4, 3)
    r.check("img2sixel", m)
    r.feed(E + "[6n")
    r.sync()
    check(r.replies().endswith(b"\x1b[8;5R"), "img2sixel: cursor %r" %
          r.replies())


# ------------------------------------------------------------------- main

SCENARIOS = {}


def main():
    global COMP, HAVOC, SELECTED
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    COMP, HAVOC = (os.path.abspath(a) for a in sys.argv[1:3])
    SELECTED = set(sys.argv[3:])

    run_scenario("basic", sc_basic)
    run_scenario("scroll", sc_scroll)
    run_scenario("region", sc_region)
    run_scenario("erase", sc_erase)
    run_scenario("scrollback_clear", sc_scrollback_clear)
    run_scenario("alt", sc_alt)
    run_scenario("resize", sc_resize)
    run_scenario("scale", sc_scale, fractional=True, config=ZOOM_CONFIG)
    run_scenario("cursor", sc_cursor)
    run_scenario("sgr", sc_sgr)
    run_scenario("wide", sc_wide)
    run_scenario("right", sc_right)
    run_scenario("memory", sc_memory, extra="max_pixels=16000\n")
    run_scenario("truncated", sc_truncated)
    run_scenario("replies", sc_replies, runs=(False,))
    run_scenario("replies_off", sc_replies_off, extra="sixel=no\n",
                 runs=(False,))
    run_scenario("ptysize", sc_ptysize, ptysize=True, fractional=True,
                 config=ZOOM_CONFIG, runs=(False,))
    run_scenario("ptysize_off", sc_ptysize_off, extra="sixel=no\n",
                 ptysize=True, fractional=True, config=ZOOM_CONFIG,
                 runs=(False,))
    run_scenario("damage", sc_damage)
    run_scenario("hostile", sc_hostile)
    run_scenario("real", sc_real)
    run_scenario("opacity", sc_opacity, extra="[window]\nopacity=128\n",
                 runs=(False,))
    run_scenario("img2sixel", sc_img2sixel)
    print("%s: %d checks, %d failed" % ("PASSED" if not failures else "FAILED",
                                        checks, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
