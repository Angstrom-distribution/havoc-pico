# havoc

A minimal terminal emulator for Wayland on Linux.

## Install

You will need the libwayland-client and libxkbcommon development files
as well as wayland-protocols and the wayland-scanner(comes with the
`wayland-devel` package on many distros.)

Then run make:
```
make CFLAGS="-O2 -DNDEBUG"
make install
```

## Configure

havoc will search for a file called `havoc.cfg` in `$XDG_CONFIG_HOME/havoc/` first,
then in `$HOME/.config/havoc/` and last in the current working directory.

See the example `havoc.cfg` for available options.

Without a program on the command line and without `program=` in `[child]`, havoc starts
your login shell: `$SHELL` if it is executable, else the shell in your passwd entry, else
`/bin/sh`, run as `-name` so that it reads the profile, in `$HOME` (the passwd home if
`HOME` is unset). `HOME`, `SHELL`, `USER` and `LOGNAME` are set from the passwd entry where
unset. A program from the command line or the config runs as given, in the current directory.

## Pictures

havoc can show sixel pictures and a subset of the kitty graphics protocol.
Both are off by default: build with `HAVOC_SIXEL=1`, `HAVOC_KITTY=1` and
`HAVOC_PNG=1` and turn them on in the `[graphics]` section of `havoc.cfg`.
See [doc/graphics.md](doc/graphics.md).
