#!/usr/bin/env python3
"""Write one fuzzing seed per DCS q string found in the given files:
sixel_cmp-style header (P1, P2 and the piece size) and the body."""
import re
import sys

out = sys.argv[1]
n = 0
for path in sys.argv[2:]:
    data = open(path, "rb").read()
    for m in re.finditer(rb"\x1bP([0-9;]*)q(.*?)\x1b\\", data, re.S):
        p = [int(x) if x else 0 for x in m.group(1).split(b";")] if m.group(1) else []
        p1 = (p[0] if p else 0) & 15
        p2 = (p[1] if len(p) > 1 else 0) & 3
        body = m.group(2)[:65000]
        open(f"{out}/seed{n:03d}", "wb").write(bytes((p1, 63 << 2 | p2)) + body)
        n += 1
print(f"{n} seeds", file=sys.stderr)
