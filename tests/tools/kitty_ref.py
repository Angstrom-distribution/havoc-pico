#!/usr/bin/env python3
"""Independent reference for the kitty graphics receive side.

  kitty_ref.py decode OUT VECTOR...   [--png 0|1]
      parse the captured streams: one OUT/<name>.txt per vector with a line
      per finished command, and OUT/<name>.<n>.rgb565 / .alpha for the n-th
      picture (little endian RGB565, one alpha byte per pixel)
  kitty_ref.py pngs OUT
      write the PNG test set: OUT/<name>.png, .rgb565, .alpha and OUT/list.txt
      ("name w h status", status ok, einval or enodata)
  kitty_ref.py raw OUT
      write raw and zlib streams as APC strings with their expected pictures

Needs only the Python standard library."""
import base64, struct, sys, zlib


def rgb565(r, g, b):
    return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3


def pixel(x, y):
    """Deterministic test pixel (r, g, b, a); some alpha 0 and 255."""
    a = (x * y * 5) & 255
    if (x + y) % 5 == 0:
        a = 255
    if (x + 2 * y) % 7 == 0:
        a = 0
    return ((x * 7 + y * 3) & 255, (x * 5 + y * 11) & 255,
            (x * 13 + y * 17) & 255, a)


def pack_expected(pix, alpha):
    """pix: list of (r, g, b, a). Returns (rgb565 bytes, alpha bytes|None)."""
    rgb = b"".join(struct.pack("<H", rgb565(p[0], p[1], p[2])) for p in pix)
    al = bytes(p[3] for p in pix)
    return rgb, (al if alpha and any(p[3] != 255 for p in pix) else None)


# --- parsing the captured streams ---------------------------------------

def apcs(data):
    """Yield the bodies of ESC _ ... ESC \\ strings (7-bit introducers)."""
    pos = 0
    while True:
        i = data.find(b"\x1b_", pos)
        if i < 0:
            return
        j = data.find(b"\x1b\\", i + 2)
        if j < 0:
            return
        yield data[i + 2:j]
        pos = j + 2


def parse(body):
    """(keys dict, payload bytes) of a G string, or None."""
    if not body.startswith(b"G"):
        return None
    ctrl, _, payload = body[1:].partition(b";")
    keys = {}
    if ctrl:
        for kv in ctrl.split(b","):
            k, _, v = kv.partition(b"=")
            keys[k.decode()] = v.decode()
    return keys, payload


def num(keys, k, d=0):
    return int(keys.get(k, d))


def to_pixels(raw, w, h, bpp):
    pix = []
    for n in range(w * h):
        c = raw[n * bpp:n * bpp + bpp]
        pix.append((c[0], c[1], c[2], c[3] if bpp == 4 else 255))
    return pix


def png_pixels(data):
    """Decode a PNG of this module's own writer (non-interlaced, 8 or 16
    bits, any colour type, 1 to 8 bit palette or grey): list of RGBA."""
    pos = 8
    plte = trns = None
    idat = b""
    while pos < len(data):
        ln, = struct.unpack(">I", data[pos:pos + 4])
        t = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        if t == b"IHDR":
            w, h, bd, ct, _, _, il = struct.unpack(">IIBBBBB", body)
        elif t == b"PLTE":
            plte = body
        elif t == b"tRNS":
            trns = body
        elif t == b"IDAT":
            idat += body
        pos += 12 + ln
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ct]
    bits = ch * bd
    stride = (w * bits + 7) // 8
    raw = zlib.decompress(idat)
    bpp = max(1, bits // 8)
    rows = []
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        rows.append(line)
        prev = line
    out = []
    for y in range(h):
        line = rows[y]

        def sample(i):
            if bd == 16:
                return line[2 * i], (line[2 * i] << 8 | line[2 * i + 1])
            if bd == 8:
                return line[i], line[i]
            bit = i * bd
            v = line[bit // 8] >> (8 - bd - bit % 8) & ((1 << bd) - 1)
            return v * 255 // ((1 << bd) - 1), v
        for x in range(w):
            if ct == 3:
                _, idx = sample(x)
                r, g, b = plte[3 * idx:3 * idx + 3]
                a = trns[idx] if trns and idx < len(trns) else 255
            elif ct == 0:
                v, full = sample(x)
                r = g = b = v
                a = 255
                if trns:
                    t, = struct.unpack(">H", trns[:2])
                    a = 0 if full == t else 255
            elif ct == 4:
                r = g = b = sample(2 * x)[0]
                a = sample(2 * x + 1)[0]
            else:
                s = [sample(ch * x + i)[0] for i in range(ch)]
                r, g, b = s[0], s[1], s[2]
                a = s[3] if ct == 6 else 255
                if ct == 2 and trns:
                    full = [sample(3 * x + i)[1] for i in range(3)]
                    if full == list(struct.unpack(">HHH", trns[:6])):
                        a = 0
            out.append((r, g, b, a))
    return w, h, out


def decode(out, vectors, png):
    import os
    os.makedirs(out, exist_ok=True)
    for v in vectors:
        name = os.path.basename(v)
        if name.endswith(".bin"):
            name = name[:-4]
        data = open(v, "rb").read()
        lines = []
        count = [0]
        pending = None      # (keys of the first chunk, [payloads])

        def emit(keys, pl):
            lines.append(finish(out, name, keys, pl, png, count))

        for body in apcs(data):
            g = parse(body)
            if g is None:
                continue
            keys, pl = g
            if pending is not None:
                rest = set(keys) - {"m", "q"}
                if all(keys[k] == pending[0].get(k) for k in rest):
                    pending[1].append(pl)
                    if not num(keys, "m"):
                        emit(pending[0], pending[1])
                        pending = None
                    continue
                lines.append("a=%s i=%d err=ENODATA" %
                             (pending[0].get("a", "t"), num(pending[0], "i")))
                pending = None
            if (keys.get("a", "t") in "tTq" and keys.get("t", "d") == "d" and
                    num(keys, "m")):
                pending = (keys, [pl])
                continue
            emit(keys, [pl])
        with open("%s/%s.txt" % (out, name), "w") as f:
            f.write("".join(l + "\n" for l in lines))


def unb64(chunks):
    """Each chunk is decoded on its own: clients pad every chunk."""
    return b"".join(base64.b64decode(c + b"=" * (-len(c) % 4)) for c in chunks)


def finish(out, name, keys, pl, png, count):
    a = keys.get("a", "t")
    head = "a=%s i=%d" % (a, num(keys, "i"))
    if a not in "tTq":
        return head
    f = num(keys, "f", 32)
    if f == 100 and not png:
        return head + " err=EINVAL"
    if keys.get("t", "d") != "d":
        return head + " err=EBADF"
    if f == 100:
        w, h, pix = png_pixels(unb64(pl))
    else:
        raw = unb64(pl)
        w, h = num(keys, "s"), num(keys, "v")
        bpp = 4 if f == 32 else 3
        if len(raw) != w * h * bpp:
            return head + " err=ENODATA"
        pix = to_pixels(raw, w, h, bpp)
    if a == "q":
        return head
    rgb, al = pack_expected(pix, True)
    n = count[0]
    count[0] += 1
    open("%s/%s.%d.rgb565" % (out, name, n), "wb").write(rgb)
    if al:
        open("%s/%s.%d.alpha" % (out, name, n), "wb").write(al)
    return head + " w=%d h=%d alpha=%d" % (w, h, 1 if al else 0)


# --- generated PNGs and raw streams ---------------------------------------

def chunk(t, body):
    return (struct.pack(">I", len(body)) + t + body +
            struct.pack(">I", zlib.crc32(t + body) & 0xffffffff))


def write_png(w, h, ct, bd, rows, plte=None, trns=None, interlace=0,
              bad_crc=False, truncate=0, trailing=b""):
    """rows: list of bytes, each a scanline without filter byte."""
    raw = b"".join(b"\0" + r for r in rows)
    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, bd, ct, 0, 0, interlace))
    if plte:
        out += chunk(b"PLTE", plte)
    if trns:
        out += chunk(b"tRNS", trns)
    # split IDAT in two so the reader sees a chunk boundary
    z = zlib.compress(raw)
    half = len(z) // 2
    out += chunk(b"IDAT", z[:half]) + chunk(b"IDAT", z[half:])
    iend = chunk(b"IEND", b"")
    if bad_crc:
        iend = iend[:-1] + bytes([iend[-1] ^ 1])
        # a bad CRC in IDAT is what libpng checks first
        i = out.rindex(b"IDAT") - 4
        ln, = struct.unpack(">I", out[i:i + 4])
        j = i + 8 + ln
        out = out[:j] + bytes([out[j] ^ 1]) + out[j + 1:]
    out += iend
    if truncate:
        out = out[:-truncate]
    return out + trailing


def pack_row(values, bd):
    """values: samples (ints) for one row, packed at bd bits."""
    if bd == 8:
        return bytes(values)
    if bd == 16:
        return b"".join(struct.pack(">H", v) for v in values)
    bits = "".join(format(v, "0%db" % bd) for v in values)
    bits += "0" * (-len(bits) % 8)
    return bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))


def gen_pngs(out):
    import os
    os.makedirs(out, exist_ok=True)
    lst = []

    def add(name, png, w, h, pix, status="ok", alpha=True):
        open("%s/%s.png" % (out, name), "wb").write(png)
        if status == "ok":
            rgb, al = pack_expected(pix, alpha)
            open("%s/%s.rgb565" % (out, name), "wb").write(rgb)
            if al:
                open("%s/%s.alpha" % (out, name), "wb").write(al)
        lst.append("%s %d %d %s %d" % (name, w, h, status, 1 if status == "ok" and al else 0))

    def mk(name, w, h, ct, bd, alpha, **kw):
        rows, pix = [], []
        sh = 8 if bd == 16 else 0
        for y in range(h):
            vals = []
            for x in range(w):
                r, g, b, a = pixel(x, y)
                if ct == 2:
                    vals += [r, g, b]
                    pix.append((r, g, b, 255))
                elif ct == 6:
                    vals += [r, g, b, a]
                    pix.append((r, g, b, a))
                elif ct == 0:
                    vals.append(r)
                    pix.append((r, r, r, 255))
                elif ct == 4:
                    vals += [r, a]
                    pix.append((r, r, r, a))
            if bd == 16:
                vals = [v << 8 | v for v in vals]
            rows.append(pack_row(vals, bd))
        return write_png(w, h, ct, bd, rows, **kw), pix

    for name, ct, bd, alpha in [("rgb", 2, 8, False), ("rgba", 6, 8, True),
                                ("grey", 0, 8, False), ("greya", 4, 8, True),
                                ("rgb16", 2, 16, False), ("rgba16", 6, 16, True)]:
        png, pix = mk(name, 37, 23, ct, bd, alpha)
        if bd == 16:
            pix = [(p[0], p[1], p[2], p[3]) for p in pix]
        add(name, png, 37, 23, pix, alpha=alpha)

    # palette, 4 bits, with and without tRNS
    for name, with_trns in (("pal", False), ("paltrns", True)):
        w, h = 19, 11
        pal = [(i * 16 & 255, i * 7 + 20 & 255, 255 - i * 9 & 255) for i in range(16)]
        plte = b"".join(bytes(c) for c in pal)
        trns = bytes([255 - i * 17 for i in range(10)]) if with_trns else None
        rows, pix = [], []
        for y in range(h):
            idx = [(x + y) % 16 for x in range(w)]
            rows.append(pack_row(idx, 4))
            for i in idx:
                a = trns[i] if trns and i < len(trns) else 255
                pix.append(pal[i] + (a,))
        add(name, write_png(w, h, 3, 4, rows, plte=plte, trns=trns), w, h, pix,
            alpha=with_trns)
    # 2-bit grey
    w, h = 9, 4
    rows = [pack_row([(x + y) % 4 for x in range(w)], 2) for y in range(h)]
    pix = [((x + y) % 4 * 85,) * 3 + (255,) for y in range(h) for x in range(w)]
    add("grey2", write_png(w, h, 0, 2, rows), w, h, pix, alpha=False)
    # 1x1 and 4096x1
    add("one", write_png(1, 1, 2, 8, [bytes([200, 100, 50])]), 1, 1,
        [(200, 100, 50, 255)], alpha=False)
    rows = [bytes(v for x in range(4096) for v in pixel(x, 0)[:3])]
    add("wide", write_png(4096, 1, 2, 8, rows), 4096, 1,
        [pixel(x, 0)[:3] + (255,) for x in range(4096)], alpha=False)
    # trailing garbage after IEND
    png, pix = mk("garbage", 8, 8, 2, 8, False, trailing=b"garbage after the end")
    add("garbage", png, 8, 8, pix, alpha=False)
    # errors
    png, _ = mk("interlaced", 8, 8, 2, 8, False, interlace=1)
    add("interlaced", png, 8, 8, None, "einval")
    add("toowide", write_png(5000, 10, 2, 8, [bytes(15000)] * 10), 5000, 10, None, "einval")
    png, _ = mk("badcrc", 8, 8, 2, 8, False, bad_crc=True)
    add("badcrc", png, 8, 8, None, "einval")
    png, _ = mk("short", 40, 40, 2, 8, False, truncate=30)
    add("short", png, 40, 40, None, "enodata")
    # a complete file whose image data holds 3 of its 8 rows
    rows = [bytes(24)] * 3
    add("fewrows", write_png(8, 8, 2, 8, rows), 8, 8, None, "enodata")
    add("notpng", b"this is not a png file at all", 0, 0, None, "einval")
    open("%s/list.txt" % out, "w").write("\n".join(lst) + "\n")


def apc(keys, payload=b""):
    return b"\x1b_G" + keys.encode() + (b";" + payload if payload else b"") + b"\x1b\\"


def gen_raw(out):
    import os
    os.makedirs(out, exist_ok=True)
    w, h = 29, 17
    pix = [pixel(x, y) for y in range(h) for x in range(w)]
    for name, bpp in (("rgb", 3), ("rgba", 4)):
        raw = bytes(c for p in pix for c in p[:bpp])
        rgb, al = pack_expected(pix, bpp == 4)
        for tag, data, extra in (("", raw, ""), ("-z", zlib.compress(raw), ",o=z")):
            b = base64.b64encode(data)
            open("%s/%s%s.bin" % (out, name, tag), "wb").write(
                apc("a=t,f=%d,s=%d,v=%d,i=7%s" % (bpp * 8, w, h, extra), b))
        open("%s/%s.rgb565" % (out, name), "wb").write(rgb)
        if al:
            open("%s/%s.alpha" % (out, name), "wb").write(al)
    open("%s/size.txt" % out, "w").write("%d %d\n" % (w, h))


if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[1] == "decode":
        args = sys.argv[3:]
        png = 1
        if "--png" in args:
            i = args.index("--png")
            png = int(args[i + 1])
            del args[i:i + 2]
        decode(sys.argv[2], args, png)
    elif len(sys.argv) == 3 and sys.argv[1] == "pngs":
        gen_pngs(sys.argv[2])
    elif len(sys.argv) == 3 and sys.argv[1] == "raw":
        gen_raw(sys.argv[2])
    else:
        sys.exit(__doc__)
