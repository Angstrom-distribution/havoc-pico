#!/usr/bin/env python3
"""Window tests for pictures in a C8 window (HAVOC_SIXEL=1 HAVOC_KITTY=1
HAVOC_PNG=1, [window] c8=yes, the fake compositor built with -DWITH_C8).

    c8pics.py FAKE_COMPOSITOR_C8 HAVOC [scenario ...]

The window is read through the palette the compositor holds. The expected
window is the XRGB model of graphics.py and kitty.py with every picture
pixel quantised by a reference quantiser written here: the fixed 6x6x6 cube,
a 4x4 ordered dither whose phase counts from the picture's own corner, with
divides and comparisons instead of tables. Text and backgrounds are cube
colours, which the quantiser leaves alone, so a pixel only needs the right
phase when it comes from a picture. Pictures with alpha are blended in
XRGB first and the blended colour is quantised with the top picture's
phase. The palette slots must stay as the text asked for them. Every
scenario runs twice, incrementally and with HAVOC_FULL_REDRAW; the windows
must be equal after every step (the check of the damage).
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "tools"))

from PIL import Image, ImageChops  # noqa: E402

import graphics as G  # noqa: E402
import kitty as K  # noqa: E402
from graphics import E, COLS, ROWS, CW, CH, BG, check, START, cup, lf, at_bottom  # noqa: E402
from kitty import Screen, put, mkimg, apc, raw, cup_s, KCFG  # noqa: E402

FORMAT_C8 = 0x20203843
C8_CONFIG = G.CONFIG.replace("[graphics]", "[window]\nc8=yes\n[graphics]")
C8_ARGS = ("--c8", "exclusive")
SLOT_INDICES = list(range(16)) + list(range(232, 256))

# ----------------------------------------------------- the reference

LEVEL = (0, 95, 135, 175, 215, 255)
BAYER = ((0, 8, 2, 10), (12, 4, 14, 6), (3, 11, 1, 9), (15, 7, 13, 5))


def channel(v, th):
    """Level index of v: the lower level, or the upper one when v is more
    than th sixteenths of the way to it."""
    lo = max(i for i in range(6) if LEVEL[i] <= v)
    if lo == 5:
        return 5
    f = (v - LEVEL[lo]) * 16 // (LEVEL[lo + 1] - LEVEL[lo])
    return lo + (1 if f > th else 0)


def quantise(rgb, px, py):
    """The cube colour of rgb at picture pixel (px, py)."""
    th = BAYER[py & 3][px & 3]
    return tuple(LEVEL[channel(v, th)] for v in rgb)


def owners(rects, w, h, dy=0):
    """For each window pixel the corner (x, y) of the picture drawn last on
    it, or None. rects: (cx, cy, x0, y0, x1, y1, alpha) in drawing order,
    the picture's corner, the area it covers and its alpha channel from
    the corner on (None: opaque); y counts from dy above the window."""
    own = [None] * (w * h)
    for (cx, cy, x0, y0, x1, y1, al) in rects:
        data = al.tobytes() if al else None
        aw = al.width if al else 0
        for y in range(max(y0 - dy, 0), min(y1 - dy, h)):
            for x in range(max(x0, 0), min(x1, w)):
                if data is not None:
                    sx, sy = x - cx, y + dy - cy
                    if not (0 <= sx < aw and 0 <= sy < al.height) or \
                            not data[sy * aw + sx]:
                        continue
                own[y * w + x] = (cx, cy - dy)
    return own


def expect(xrgb, rects, dy=0, exact=()):
    """xrgb (the model's window) as a C8 window shows it. exact: colours
    that text has in slots of their own; they are shown as they are."""
    w, h = xrgb.size
    own = owners(rects, w, h, dy)
    src = xrgb.convert("RGB")
    out = Image.new("RGB", src.size)
    sp, op = src.load(), out.load()
    for y in range(h):
        for x in range(w):
            o = own[y * w + x]
            if sp[x, y] in exact:
                op[x, y] = sp[x, y]
            elif o:
                op[x, y] = quantise(sp[x, y], x - o[0], y - o[1])
            else:
                op[x, y] = quantise(sp[x, y], 0, 0)
    return out


def rect_img(cx, cy, img):
    return (cx, cy, cx, cy, cx + img.width, cy + img.height,
            img.getchannel("A"))


def kitty_rects(scr, cw=CW, ch=CH):
    """The areas of the placements in view, in drawing order."""
    out = []
    view = 0 if scr.alt else scr.view
    for p in sorted(scr.pl, key=lambda p: (p.z, p.seq)):
        cx = p.col * cw + p.dx * cw // p.cw0
        cy = (p.row + view) * ch + p.dy * ch // p.ch0
        y0 = max(cy, (p.row + view + p.skip) * ch)
        scaled = (p.dw, p.dh) != (p.sw, p.sh)
        al = None
        if not scaled:
            al = p.img.crop((p.sx, p.sy, p.sx + p.sw, p.sy + p.sh)
                            ).getchannel("A")
        out.append((cx, cy, cx, y0, cx + p.dw * cw // p.cw0,
                    cy + p.dh * ch // p.ch0, al))
    return out


# ------------------------------------------------------------- helpers

def slots(r):
    """The 40 slot entries of the palette in force, as {index: 'rrggbb'}."""
    line = r.c.cmd("palette " + " ".join(str(i) for i in SLOT_INDICES))
    return dict(w.split("=") for w in line.split()[1:])


def slot_colours(r):
    """The colours the text holds slots for (slots start black)."""
    return {tuple(int(v[i:i + 2], 16) for i in (0, 2, 4))
            for v in slots(r).values() if v != "000000"}


def palsets(r):
    return int(r.c.stats()["palsets"])


def is_c8(r):
    check(r.c.stats()["format"] == str(FORMAT_C8),
          "%s: window is not C8" % r.name)


def compare(r, label, act, exp, hole=()):
    """act (the window) must equal exp, outside the cells in hole."""
    r.snaps[label] = act.tobytes()
    if r.full:
        return
    if act.size != exp.size:
        check(False, "%s: %s: window %s, model %s" % (r.name, label,
                                                     act.size, exp.size))
        return
    diff = ImageChops.difference(act, exp)
    for (row, col) in hole:
        diff.paste(BG, (col * CW, row * CH, (col + 1) * CW, (row + 1) * CH))
    bb = diff.getbbox()
    if bb and os.environ.get("C8_KEEP"):
        act.save("/tmp/c8-%s-%s-act.png" % (r.name, label.replace(" ", "_")))
        exp.save("/tmp/c8-%s-%s-exp.png" % (r.name, label.replace(" ", "_")))
    check(bb is None, "%s: %s: window differs from the reference at %s "
          "(cell %s)" % (r.name, label, bb,
                         bb and (bb[0] // CW, bb[1] // CH)))


def invert_cell(img, row, col):
    box = (col * CW, row * CH, (col + 1) * CW, (row + 1) * CH)
    img.paste(ImageChops.invert(img.crop(box)), box)


def spread(img):
    """Distinct colours of the window: the check that a picture is dithered
    and not flattened."""
    return len(set(img.getdata()))


# ------------------------------------------------------------- sixel

def sc_sixel(r):
    """1, 2: sixel pictures, scrolling and erasing; the pattern moves with
    the picture."""
    is_c8(r)
    m = G.Model()
    rects = []
    r.sync(START)
    sl0, ps0 = slots(r), palsets(r)

    def put6(stream, col, row):
        img = G.ref(stream, (COLS - col) * CW)
        G.place(r, m, stream, col, row)
        rects.append(rect_img(col * CW, row * CH, img))

    def step(label, hole=()):
        act = r.snap().convert("RGB")
        compare(r, label, act, expect(m.view(), rects, m.top * CH), hole)
        return act

    put6(G.picture(1), 0, 0)
    before = step("picture")
    put6(G.picture(2, 300, 20), 30, 8)
    step("clipped")
    p0 = G.sixel_enc.encode(G.sixel_enc.pattern(30, 12, 3), p1=0, p2=0,
                            raster=False)
    put6(p0, 12, 12)
    step("aspect")
    check(slots(r) == sl0 and palsets(r) == ps0,
          "sixel: a picture changed the palette")
    if not r.full:
        check(spread(before) > 8, "sixel: the window has %d colours" %
              spread(before))

    # scrolled by one row: the same pixels, one row up
    r.feed(at_bottom(m))
    r.sync(lf(1))
    m.scroll(1)
    after = step("lf1")
    check(before.crop((0, CH, 11 * CW, 5 * CH)).tobytes() ==
          after.crop((0, 0, 11 * CW, 4 * CH)).tobytes() or r.full,
          "sixel: the pattern changed when the picture scrolled")
    r.sync(lf(5))
    m.scroll(5)
    step("lf5")
    bottom = m.top
    r.scroll_back(m, 9)
    step("scrollback")
    r.scroll_bottom(m, bottom)
    step("bottom")

    # erasing a row; text over a tile (the cell is text then)
    r.sync(cup(m, m.top + 3, 0) + E + "[2K")
    m.clear(m.top + 3, 0, COLS - 1)
    step("erase")
    r.sync(cup(m, m.top + 0, 40 // 2) + "x")
    m.text(m.top + 0, 20, 20)
    step("text", hole=[(0, 20)])
    check(slots(r) == sl0 and palsets(r) == ps0,
          "sixel: the palette changed")


def sc_sixel_cursor(r):
    """3: the cursor inverts a tile and the dither follows the inverted
    pixels."""
    is_c8(r)
    m = G.Model()
    r.sync(E + "[2J" + E + "[H" + E + "[?25l")
    stream = G.picture(1)
    img = G.ref(stream, COLS * CW)
    G.place(r, m, stream, 0, 0)
    rects = [rect_img(0, 0, img)]
    r.sync(cup(m, 2, 3) + E + "[?25h")
    exp = m.view()
    invert_cell(exp, 2, 3)
    compare(r, "cursor", r.snap().convert("RGB"), expect(exp, rects))
    r.sync(cup(m, 20, 20))
    exp = m.view()
    invert_cell(exp, 20, 20)
    compare(r, "cursor moved", r.snap().convert("RGB"), expect(exp, rects))


def sc_slots(r):
    """4: text colours keep their slots, and a picture beside them takes
    none."""
    is_c8(r)
    m = G.Model()
    r.sync(START)
    r.sync(cup(m, 20, 0) + E + "[38;2;12;34;56mslot text" + E + "[0m")
    sl1, ps1 = slots(r), palsets(r)
    check(sl1["0"] == "0c2238", "slots: the text colour has no slot %r" %
          sl1["0"])
    txt = r.snap().convert("RGB").crop((0, 20 * CH, 10 * CW, 21 * CH))
    stream = G.picture(7)
    img = G.ref(stream, COLS * CW)
    G.place(r, m, stream, 0, 0)
    rects = [rect_img(0, 0, img)]
    act = r.snap().convert("RGB")
    check(slots(r) == sl1, "slots: a picture changed the slots %r" % slots(r))
    check(palsets(r) == ps1, "slots: a picture sent a palette")
    check(act.crop((0, 20 * CH, 10 * CW, 21 * CH)).tobytes() == txt.tobytes(),
          "slots: the text changed")
    # all the colours of the picture are cube entries
    cube = {(a, b, c) for a in LEVEL for b in LEVEL for c in LEVEL}
    top = act.crop((0, 0, 11 * CW, 5 * CH))
    check(set(top.getdata()) <= cube, "slots: a colour outside the cube")
    # the text's slot colour is exact
    check((12, 34, 56) in set(act.crop((0, 20 * CH, 10 * CW, 21 * CH)).getdata()),
          "slots: the text colour is not in the window")


# ------------------------------------------------------------- kitty

def ramp(w, h):
    """A grey ramp over the top half and a colour ramp below: every level
    of every channel, with all fractions."""
    im = Image.new("RGBA", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            v = x * 255 // (w - 1)
            px[x, y] = (v, v, v, 255) if y < h // 2 else \
                (v, 255 - v, (v * 3) & 255, 255)
    return im


def kstep(r, scr, label, text=None, coloured=(), cursor=None, hole=()):
    act = r.snap().convert("RGB")
    xrgb = scr.render(text=text, coloured=coloured)
    if cursor:
        invert_cell(xrgb, *cursor)
    compare(r, label, act, expect(xrgb, kitty_rects(scr),
                                  exact=slot_colours(r)), hole)
    return act


def sc_kitty(r):
    """5: kitty placements: ramps, z order, alpha, offsets, scaling."""
    is_c8(r)
    scr = Screen()
    r.sync(START)
    sl0, ps0 = slots(r), palsets(r)
    put(r, scr, ramp(120, 40), 2, 1, i=1, C=1)
    before = kstep(r, scr, "ramp")
    if not r.full:
        check(spread(before) > 20, "kitty: the window has %d colours" %
              spread(before))
    # two overlapping pictures: the higher z on top
    put(r, scr, mkimg(60, 40, 1), 2, 8, z=3, i=2, C=1)
    put(r, scr, mkimg(60, 40, 2), 6, 9, z=1, i=3, C=1)
    kstep(r, scr, "z order")
    put(r, scr, mkimg(30, 30, 3), 8, 10, z=1, i=4, C=1)
    kstep(r, scr, "equal z")
    # a pixel offset into the cell and a scaled placement
    put(r, scr, mkimg(40, 30, 4), 20, 1, i=5, C=1, X=3, Y=4)
    kstep(r, scr, "offset")
    put(r, scr, mkimg(20, 20, 5), 24, 12, i=6, C=1, c=12, r=3)
    kstep(r, scr, "scaled")
    check(slots(r) == sl0 and palsets(r) == ps0,
          "kitty: a picture changed the palette")
    # a placement with alpha over another picture
    put(r, scr, mkimg(60, 30, 6, alpha=True), 14, 8, z=2, i=7, C=1)
    kstep(r, scr, "alpha")

    # scrolled by one row: the same pixels, one row up
    r.feed(E + "[%d;1H" % ROWS)
    r.sync(lf(1))
    scr.lf(1)
    after = kstep(r, scr, "lf1")
    check(r.full or before.crop((2 * CW, 2 * CH, 20 * CW, 5 * CH)).tobytes() ==
          after.crop((2 * CW, CH, 20 * CW, 4 * CH)).tobytes(),
          "kitty: the pattern changed when the picture scrolled")
    r.sync(lf(5))
    scr.lf(5)
    kstep(r, scr, "lf5")
    for _ in range(7):
        r.c.cmd("mods 5")
        r.c.keys("k")
        r.c.cmd("mods 0")
    r.sync()
    scr.view = min(scr.view + 7, scr.sb)
    kstep(r, scr, "scrollback")
    r.c.cmd("mods 5")
    r.c.cmd("key 107")
    r.c.cmd("mods 0")
    r.sync()
    scr.view = 0
    kstep(r, scr, "bottom")
    # deleting one
    r.sync(apc("a=d,d=I,i=2"))
    scr.pl[:] = [p for p in scr.pl if p.iid != 2]
    kstep(r, scr, "deleted")


def sc_ktext(r):
    """6: text between layers, with its colour in a slot."""
    is_c8(r)
    scr = Screen()
    r.sync(START)
    r.sync(cup_s(5, 2) + "THE QUICK BROWN FOX" + cup_s(6, 2) +
           E + "[38;2;12;34;56mover the lazy dog" + E + "[0m")
    txt = r.snap()
    sl1 = slots(r)
    check(sl1["0"] == "0c2238", "ktext: the text colour has no slot")
    put(r, scr, mkimg(120, 50, 4), 1, 3, z=-1, i=11, C=1)
    put(r, scr, mkimg(60, 30, 5, alpha=True), 12, 4, z=2, i=12, C=1)
    kstep(r, scr, "text between layers", text=txt)
    check(slots(r) == sl1, "ktext: the slots changed")
    # a coloured background hides z below -2^30
    r.sync(E + "[2J")
    for row in (14, 15, 16):
        r.feed(cup_s(row, 4) + E + "[44m" + " " * 6 + E + "[0m")
    r.sync()
    coloured = {(row, c) for row in (14, 15, 16) for c in range(4, 10)}
    text0 = r.snap()
    scr.pl[:] = []
    put(r, scr, mkimg(100, 60, 6), 2, 13, z=K.UNDER_BG - 1, i=21, C=1)
    kstep(r, scr, "under background", text=text0, coloured=coloured)


def sc_kcursor(r):
    """7: the cursor over a placement inverts it, then dithers."""
    is_c8(r)
    scr = Screen()
    r.sync(E + "[2J" + E + "[H" + E + "[?25l")
    put(r, scr, ramp(64, 48), 0, 0, i=1, C=1, z=-1)
    r.sync(E + "[?25h" + cup_s(2, 3))
    kstep(r, scr, "cursor", cursor=(2, 3))
    r.sync(cup_s(20, 20))
    kstep(r, scr, "cursor moved", cursor=(20, 20))
    r.sync(cup_s(2, 3) + "abc")
    r.sync(cup_s(0, 0))
    r.sync(E + "[?25l")
    # text written over a placement of z < 0 keeps the picture around it
    txt = r.snap()
    scr2 = Screen()
    scr2.pl[:] = []
    put(r, scr2, ramp(64, 48), 0, 0, i=1, C=1, z=-1)
    kstep(r, scr2, "text over", text=txt)


def sc_kresize_damage(r):
    """8: erase and overwrite parts of placements."""
    is_c8(r)
    scr = Screen()
    r.sync(START)
    put(r, scr, ramp(120, 60), 2, 2, i=1, C=1)
    kstep(r, scr, "placed")
    r.sync(cup_s(4, 0) + E + "[2K")
    kstep(r, scr, "line erased")
    r.sync(E + "[2J")
    scr.erase_screen()
    kstep(r, scr, "screen erased")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    G.COMP, G.HAVOC = (os.path.abspath(a) for a in sys.argv[1:3])
    G.SELECTED = set(sys.argv[3:])
    G.Run = K.KRun
    S = lambda name, fn, **kw: G.run_scenario(  # noqa: E731
        name, fn, extra=KCFG, config=C8_CONFIG, args=C8_ARGS, **kw)
    S("c8_sixel", sc_sixel)
    S("c8_sixel_cursor", sc_sixel_cursor)
    S("c8_slots", sc_slots)
    S("c8_kitty", sc_kitty)
    S("c8_ktext", sc_ktext)
    S("c8_kcursor", sc_kcursor)
    S("c8_damage", sc_kresize_damage)
    print("%s: %d checks, %d failed" % ("PASSED" if not G.failures else
                                        "FAILED", G.checks, len(G.failures)))
    return 1 if G.failures else 0


if __name__ == "__main__":
    sys.exit(main())
