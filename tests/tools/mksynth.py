#!/usr/bin/env python3
"""Write synthetic sixel strings (generated here, no outside data) that the
captured vectors do not cover: every P1, raster aspect, wrapped registers,
HLS and RGB definitions, repeats, overprinting, P2, size hints, bytes outside
the alphabet, short and long colour commands, random strings.

    mksynth.py OUT.bin
"""
import random
import sys

random.seed(7)
ESC = b"\x1b"
strings = []


def add(body, params=b""):
    strings.append(ESC + b"P" + params + b"q" + body + ESC + b"\\")


def stripes(n=12, colours=(1, 2, 3)):
    s = b""
    for i in range(n):
        s += b"#%d" % colours[i % len(colours)] + bytes([0x3f + (i * 5) % 64]) * 3
    return s


for p1 in range(0, 11):
    add(b"#1;2;100;0;0#2;2;0;100;0#3;2;0;0;100" + stripes() + b"-" + stripes(), b"%d;1" % p1 if p1 % 2 else b"%d" % p1)
for pan, pad in ((1, 1), (2, 1), (3, 1), (5, 1), (9, 1), (7, 2), (1, 2), (10, 3)):
    add(b'"%d;%d;20;30#1;2;100;50;0' % (pan, pad) + stripes(8) + b"-" + stripes(8), b"0;1")
add(b'"0;1;20;30' + stripes(8))
add(b'"1;0;20;30' + stripes(8), b"2")
# raster attributes after data are ignored
add(stripes(6) + b'"1;1;50;50' + stripes(6))
# wrapped registers
add(b"#256;2;100;0;0~#300;2;0;100;0~#44~#511;2;0;0;100~#255~#0~")
add(b"#255;2;10;20;30~#259~#3~")
# default registers 0..15, black above
add(b"".join(b"#%d~" % i for i in (0, 1, 2, 7, 8, 15, 16, 100, 255)))
# RGB values: percent table, over 100, empty values
add(b"#1;2;0;0;0~#2;2;1;2;3~#3;2;33;66;99~#4;2;100;100;100~#5;2;150;200;999~#6;2;;;~")
# HLS
for h in range(0, 360, 30):
    add(b"".join(b"#%d;1;%d;%d;%d~" % (i, h, l, s) for i, (l, s) in enumerate(
        ((50, 100), (25, 100), (75, 100), (50, 50), (50, 0), (100, 50), (0, 50), (90, 20)))))
# unusual colour commands
add(b"#1;2;50~#2;2;50;50;50;50;50~#3;2;100;0;0;99~")
add(b"#1;3;1;2;3~#1;0;1;2;3~~")
add(b"#1;2;100;0;0~#;2;0;100;0~")
# repeats
add(b"#1;2;100;0;0!1~!2~!3~!0~!10@!10A")
add(b"!5#1;2;100;0;0~!~!9~")
add(b"#1;2;100;0;0!240~-!240N-!1000~")
add(b"#1;2;100;0;0!100000~$!5N")
add(b"#1;2;100;0;0!5000~-!5000F")
add(b"!99999999~", b"7")
# overprint
add(b"#1;2;100;0;0!20~$#2;2;0;100;0!10N$#3;2;0;0;100!5A", b"7")
# bands
add(b"#1;2;100;0;0~-----~-", b"7")
add(b"$-$-$-")
add(b"-~")
add(b"?")
add(b"?-?-?")
add(b"~~~$-~~~$-", b"2;1")
add(b"~~~-~~~-", b"2;0")
add(b"")
# P2 and size hints
for p2 in (0, 1, 2):
    for hint in (b"", b'"1;1;10;10', b'"1;1;100;100', b'"1;1;3;100', b'"1;1;100;3', b'"1;1;4096;4096'):
        add(hint + b"#1;2;100;0;0~~~!7N", b"7;%d" % p2)
        add(hint, b"7;%d" % p2)
# bytes outside the alphabet, controls, digits and ; in the ground state
add(b"#1;2;100;0;0~\x01~ ~\x7f~\xff~;;;123~\r\n~")
add(b"~" + bytes(range(0x20, 0x3f)) + b"~")
# a long colour command split by newlines is not a command
add(b"#1;2;100;\n0;0~")
# aborted strings
strings.append(ESC + b"Pq~~~" + ESC + b"[0m")
strings.append(ESC + b"P1;1q~~~" + ESC + b"P" + b"q~" + ESC + b"\\")


def rand_string():
    s = b""
    for _ in range(random.randrange(5, 120)):
        op = random.randrange(12)
        if op == 0:
            s += b"#%d;2;%d;%d;%d" % (random.randrange(300), *(random.randrange(101) for _ in range(3)))
        elif op == 1:
            s += b"#%d" % random.randrange(300)
        elif op == 2:
            s += b"$"
        elif op == 3:
            s += b"-"
        elif op == 4:
            s += b"!%d%c" % (random.randrange(60), 0x3f + random.randrange(64))
        elif op == 5 and not s.count(b"~"):
            s += b'"%d;%d;%d;%d' % (random.randrange(1, 8), random.randrange(1, 4),
                                    random.randrange(80), random.randrange(80))
        else:
            s += bytes(0x3f + random.randrange(64) for _ in range(random.randrange(1, 12)))
    return s


for i in range(60):
    add(rand_string(), b"%d;%d" % (random.randrange(10), random.randrange(3)))
open(sys.argv[1], "wb").write(b"text " + b" text ".join(strings))
