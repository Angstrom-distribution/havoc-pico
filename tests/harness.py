"""Drive havoc through tests/fake-compositor.c --driver.

Shared by damage.py (the tests) and measure.py (damaged bytes per scenario).
Needs python3 only.
"""
import os
import shutil
import subprocess
import tempfile

# evdev codes of the keys the compositor's default keymap types as these
KEYS = {c: k for k, c in enumerate("1234567890", 2)}
KEYS.update({c: k for k, c in enumerate("qwertyuiop", 16)})
KEYS.update({c: k for k, c in enumerate("asdfghjkl", 30)})
KEYS.update({c: k for k, c in enumerate("zxcvbnm", 44)})
KEYS[" "] = 57


class Commit:
    """One commit of the window: buffer size, format, damage rectangles."""

    def __init__(self, line):
        f = line.split()
        self.w, self.h, self.format, self.n = (int(v) for v in f[1:5])
        self.union_px, self.sum_px, self.full = (int(v) for v in f[5:8])
        self.rects = [tuple(int(v) for v in r.split(",")) for r in f[8:]]

    @property
    def bytes(self):
        # C8 buffers hold one byte per pixel
        return self.union_px * (1 if self.format == 0x20203843 else 4)


class Comp:
    """A fake compositor with havoc running in it."""

    def __init__(self, comp, havoc, config="", release="next",
                 fractional=False, full_redraw=False, quiet=120, args=(),
                 child=None, cols=80, rows=24, env=None, stderr=None):
        self.dir = tempfile.mkdtemp(prefix="havoc-test-")
        env = dict(os.environ, XDG_RUNTIME_DIR=self.dir,
                   ASAN_OPTIONS="detect_leaks=0", **(env or {}))
        env.pop("HAVOC_FULL_REDRAW", None)
        if full_redraw:
            env["HAVOC_FULL_REDRAW"] = "1"
        cfg = ""
        if config:
            cfg = os.path.join(self.dir, "havoc.cfg")
            with open(cfg, "w") as f:
                f.write(config)
        cmd = [comp, "--driver", "--release", release, "--quiet", str(quiet)]
        if fractional:
            cmd.append("--fractional")
        if child:
            cmd += ["--child", child]
        cmd += list(args)
        self.p = subprocess.Popen(cmd + [havoc, cfg], env=env,
                                  stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=stderr,
                                  text=True)
        self.first = []
        for _ in range(40):
            self.first += self.wait()
            if self.first:
                break
        if not self.first:
            raise RuntimeError("havoc never drew")
        self.w, self.h = self.first[0].w, self.first[0].h
        self.cw, self.ch = self.w // cols, self.h // rows
        self.cmd("focus")
        self.wait()

    def cmd(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        if line.split()[0] in ("wait", "stats"):
            return
        return self.p.stdout.readline().strip()

    def wait(self):
        """Wait for quiet, return the commits made since the last wait."""
        self.p.stdin.write("wait\n")
        self.p.stdin.flush()
        out = []
        while True:
            l = self.p.stdout.readline()
            if not l or l.strip() == "END":
                return out
            out.append(Commit(l))

    def feed(self, data):
        if isinstance(data, str):
            data = data.encode()
        for i in range(0, len(data), 3000):
            self.cmd("feed " + data[i:i + 3000].hex())

    def keys(self, text):
        for c in text:
            self.cmd("key %d" % KEYS[c])

    def ptr(self, *args):
        self.cmd("ptr " + " ".join(str(a) for a in args))

    def cell_px(self, x, y):
        """Surface coordinates of the middle of a cell (no margin)."""
        return x * self.cw + self.cw // 2, y * self.ch + self.ch // 2

    def stats(self):
        self.p.stdin.write("stats\n")
        self.p.stdin.flush()
        s = {}
        for w in self.p.stdout.readline().split()[1:]:
            k, v = w.split("=")
            s[k] = v
        self.p.stdout.readline()
        return s

    def snap(self, path):
        r = self.cmd("snap " + path).split()
        return int(r[1]), int(r[2])

    def close(self):
        try:
            self.p.stdin.write("quit\n")
            self.p.stdin.flush()
        except OSError:
            pass
        try:
            self.p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.p.kill()
        shutil.rmtree(self.dir, ignore_errors=True)
        return self.p.returncode

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


def damaged(commits):
    return sum(c.bytes for c in commits)
