#!/usr/bin/env python3
"""Run a program on a pty that pretends to be a terminal, record every byte it
writes, and answer the terminal queries it sends according to a profile.

    ptycap.py --profile havoc-now|sixel|kitty|both --out FILE [--cols C]
              [--rows R] [--cw W] [--ch H] [--no-pixels] [--term T]
              [--timeout S] -- CMD ARGS...

Profiles:
  havoc-now  replies what havoc answers today: DA1 without 4, DA2, DSR;
             nothing for CSI t, XTSMGRAPHICS, kitty query, XTVERSION
  sixel      havoc with the proposed sixel answers
  kitty      sixel plus the kitty graphics query answer
  silent     answers nothing at all

The queries seen are written to FILE.queries, one per line, in order.
"""
import argparse
import fcntl
import os
import pty
import re
import select
import struct
import sys
import termios
import time

p = argparse.ArgumentParser()
p.add_argument("--profile", default="havoc-now")
p.add_argument("--out", required=True)
p.add_argument("--cols", type=int, default=40)
p.add_argument("--rows", type=int, default=32)
p.add_argument("--cw", type=int, default=6)
p.add_argument("--ch", type=int, default=10)
p.add_argument("--no-pixels", action="store_true")
p.add_argument("--term", default="xterm-256color")
p.add_argument("--timeout", type=float, default=8.0)
p.add_argument("--stdin-file")
p.add_argument("--xtversion")
p.add_argument("--env", action="append", default=[])
p.add_argument("cmd", nargs=argparse.REMAINDER)
a = p.parse_args()
cmd = a.cmd[1:] if a.cmd and a.cmd[0] == "--" else a.cmd

W, H = a.cols * a.cw, a.rows * a.ch
xp, yp = (0, 0) if a.no_pixels else (W, H)

pid, fd = pty.fork()
if pid == 0:
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("KITTY", "TERM", "WEZTERM", "ITERM", "VTE",
                                "LC_TERMINAL", "COLORTERM", "WT_"))}
    env["TERM"] = a.term
    env["COLORTERM"] = "truecolor"
    for kv in a.env:
        k, v = kv.split("=", 1)
        env[k] = v
    os.execvpe(cmd[0], cmd, env)

fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", a.rows, a.cols, xp, yp))

prof = a.profile
sixel = prof in ("sixel", "kitty", "kitty-direct")
replies = []
queries = []


def answer(q, reply):
    queries.append(q)
    if reply is not None and prof != "silent":
        os.write(fd, reply.encode())


def kdirect(m):
    c = m.group(1)
    i = re.search(rb"i=(\d+)", c)
    i = i.group(1).decode() if i else "0"
    if re.search(rb"t=[tfs]", c):
        return "\x1b_Gi=%s;EINVAL:only direct transmission\x1b\\" % i
    return "\x1b_Gi=%s;OK\x1b\\" % i


# what havoc's tsm-vte.c answers today: DA1 '62;1;6;9;15', DA2 '>1;1;0', DSR
DA1 = "\x1b[?62;4;6;9;15;22c" if sixel else "\x1b[?62;1;6;9;15c"

pats = [
    (re.compile(rb"\x1b\[0?c"), lambda m: DA1),
    (re.compile(rb"\x1b\[>0?c"), lambda m: "\x1b[>1;1;0c"),
    (re.compile(rb"\x1b\[5n"), lambda m: "\x1b[0n"),
    (re.compile(rb"\x1b\[6n"), lambda m: "\x1b[1;1R"),
    (re.compile(rb"\x1b\[14t"), lambda m: f"\x1b[4;{H};{W}t" if sixel else None),
    (re.compile(rb"\x1b\[16t"), lambda m: f"\x1b[6;{a.ch};{a.cw}t" if sixel else None),
    (re.compile(rb"\x1b\[18t"), lambda m: f"\x1b[8;{a.rows};{a.cols}t" if sixel else None),
    (re.compile(rb"\x1b\[\?1;1;0S"), lambda m: "\x1b[?1;0;256S" if sixel else None),
    (re.compile(rb"\x1b\[\?1;4;0S"), lambda m: "\x1b[?1;0;256S" if sixel else None),
    (re.compile(rb"\x1b\[\?2;1;0S"), lambda m: f"\x1b[?2;0;{W};{H}S" if sixel else None),
    (re.compile(rb"\x1b\[\?2;4;0S"), lambda m: f"\x1b[?2;0;{W};{H}S" if sixel else None),
    (re.compile(rb"\x1b_G([^\x1b]*a=q[^\x1b;]*)(;[^\x1b]*)?\x1b\\"),
     lambda m: ("\x1b_Gi=%s;OK\x1b\\" % (re.search(rb"i=(\d+)", m.group(1)).group(1).decode()
                                          if re.search(rb"i=(\d+)", m.group(1)) else "0"))
     if prof == "kitty" else kdirect(m) if prof == "kitty-direct" else None),
    (re.compile(rb"\x1b\[>0?q"), lambda m: ("\x1bP>|%s\x1b\\" % a.xtversion) if a.xtversion else None),
    (re.compile(rb"\x1b\[\?u"), lambda m: None),
    (re.compile(rb"\x1b\]1[01];\?(\x07|\x1b\\)"), lambda m: None),
    (re.compile(rb"\x1bP\+q[0-9a-fA-F;]*\x1b\\"), lambda m: None),
    (re.compile(rb"\x1b\[\?[0-9;]*\$p"), lambda m: None),
    (re.compile(rb"\x1b\[\?996n"), lambda m: None),
]

out = bytearray()
scanned = 0
stdin_data = open(a.stdin_file, "rb").read() if a.stdin_file else None
if stdin_data:
    os.write(fd, stdin_data)
t0 = time.time()
last = t0
while True:
    r, _, _ = select.select([fd], [], [], 0.2)
    if r:
        try:
            d = os.read(fd, 65536)
        except OSError:
            break
        if not d:
            break
        out += d
        last = time.time()
        # look for complete queries in the new data (keep a small overlap)
        start = max(0, scanned - 64)
        found = {}
        for rx, fn in pats:
            for m in rx.finditer(out, start):
                if m.end() <= scanned:
                    continue
                found.setdefault((m.start(), m.end()), (m, fn))
        # answer in the order the queries arrived, as a terminal's parser does
        for key in sorted(found):
            m, fn = found[key]
            answer(repr(m.group(0))[2:-1][:120], fn(m))
        scanned = len(out)
    if time.time() - t0 > a.timeout:
        break
    try:
        wp, st = os.waitpid(pid, os.WNOHANG)
        if wp:
            # drain
            while True:
                r, _, _ = select.select([fd], [], [], 0.3)
                if not r:
                    break
                try:
                    d = os.read(fd, 65536)
                except OSError:
                    break
                if not d:
                    break
                out += d
            break
    except ChildProcessError:
        break

try:
    os.kill(pid, 9)
except OSError:
    pass
open(a.out, "wb").write(out)
with open(a.out + ".queries", "w") as f:
    for q in queries:
        f.write(q + "\n")
dt = time.time() - t0
print(f"{a.out}: {len(out)} bytes, {len(queries)} queries, {dt:.1f} s", file=sys.stderr)
