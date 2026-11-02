#!/usr/bin/env python3
"""The terminal's program for the window size tests: writes the pty size
(rows columns xpixel ypixel) to the file in argv[1] at start and at every
SIGWINCH."""
import fcntl
import signal
import struct
import sys
import termios

out = sys.argv[1]


def record(*args):
    size = struct.unpack("HHHH", fcntl.ioctl(0, termios.TIOCGWINSZ, bytes(8)))
    with open(out, "a") as f:
        f.write("%d %d %d %d\n" % size)


signal.signal(signal.SIGWINCH, record)
record()
while True:
    signal.pause()
