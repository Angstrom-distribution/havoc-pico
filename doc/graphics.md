# Pictures in havoc: sixel and a kitty graphics subset

havoc can show pictures sent as sixel (`DCS q`) and with a subset of the
kitty graphics protocol (`APC G`). Both are off unless the build and the
configuration turn them on; with either off, havoc behaves as before.

## Building

Three make variables, each 0 by default:

| Variable | Adds | Needs |
|---|---|---|
| `HAVOC_SIXEL=1` | sixel | nothing more |
| `HAVOC_KITTY=1` | kitty graphics, raw RGB and RGBA (`f=24`, `f=32`) | nothing more |
| `HAVOC_PNG=1` | PNG (`f=100`) and zlib (`o=z`) for kitty | `HAVOC_KITTY=1`, libpng and zlib |

```
make HAVOC_SIXEL=1 HAVOC_KITTY=1 HAVOC_PNG=1
```

With all three at 0 none of the picture code is built and the binary has no
picture symbols (`tests/run-gfx.sh` checks this). libtsm keeps its new
string and event interfaces, unused; the tests check that every captured
stream gives the same screen as before. `sixel=yes` or `kitty=yes` in
such a build prints a warning and is ignored.

## Configuration

The `[graphics]` section of `havoc.cfg`:

| Key | Values | Default | Meaning |
|---|---|---|---|
| `sixel` | `yes`, `no` | `no` | accept sixel; DA1 announces it (`4`) |
| `kitty` | `yes`, `no` | `no` | accept kitty graphics commands |
| `kitty shm` | `yes`, `no` | `no` | accept kitty `t=s` (shared memory); needs `kitty=yes` |
| `memory` | 0 to 16777216 | 524288 | pixels all pictures may hold together |

A configuration that turns both protocols on:

```
[graphics]
# pictures sent as sixel (DCS q), announced in the device attributes
sixel=yes

# pictures sent with the kitty graphics protocol (APC G)
kitty=yes

# kitty pictures from POSIX shared memory of the same user (t=s);
# useful for local video players, refused over ssh
#kitty shm=yes

# pixels all pictures may hold together (about 1 to 3 bytes each)
#memory=524288
```

## Sixel

- Raster attributes, `!` repeats, `$` and `-`, registers 0 to 255 (higher
  numbers wrap), RGB (`Pu=2`) and HLS (`Pu=1`) colours. Registers 0 to 15
  start as the VT340 colours, 16 to 255 black; each picture has its own
  palette.
- Vertical aspect from P1 by the DEC table, or `Pan;Pad` of the raster
  attributes (clamped to 1 to 5). `P2=1` leaves unset pixels transparent.
- `CSI ? 8452 h` puts the cursor right of the picture instead of below it.
  Mode 80 is ignored: sixel scrolling is always on.
- A sixel picture is a set of tiles in the terminal cells. It scrolls, goes
  into the scrollback and is erased or overwritten like text. Text written
  over a tile replaces it.

## Kitty graphics subset

| Key | Supported |
|---|---|
| `a` | `t`, `T`, `p`, `d`, `q`; animation (`f`, `a`, `c`) is `EINVAL` |
| `t` | `d` (direct); `s` (shared memory) with `kitty shm=yes`; `f` and `t` are always `EBADF` |
| `f` | `24`, `32`; `100` with `HAVOC_PNG=1` (not interlaced) |
| `o` | `z` with `HAVOC_PNG=1` |
| `i`, `p` | image and placement ids; `I` is `EINVAL` |
| `x,y,w,h` | source rectangle |
| `X,Y` | offset inside the first cell |
| `c,r` | box in cells; the picture is scaled to it |
| `C` | `1`: the cursor does not move |
| `z` | z-index: below text, below cell backgrounds (`z < -1073741824`), or above text |
| `m` | chunked transfers (with `t=d`) |
| `q` | `0`, `1`, `2` |
| `U`, `P,Q,H,V` | `EINVAL` (no Unicode or relative placements) |

Deletion `a=d` takes `d=a`, `i`, `c`, `p`, `x`, `y`, `z` and the uppercase
forms, which also free the images left without placements. A delete that
works sends no reply.

Kitty pictures are not cells: each screen (main and alternate) has its own
list of placements. A placement stays when text is written or erased under
it. It follows scrolling, the scrollback and resizing; `ED 2` deletes the
placements on the screen, `ED 3` those in the scrollback, a hard reset all
images. After `a=T` or `a=p` without `C=1` the cursor ends on the last row
of the box, right of it, or at the start of the next line when that is past
the right edge.

When an anonymous picture is placed on the same cells with the same `z` as
another anonymous one, the old placement is deleted. A video player that
sends every frame as a new picture then holds one frame, not all of them.
This rule is havoc's own.

Replies are `ESC _ G i=<id>[,p=<pid>];OK ESC \` or
`ESC _ G i=<id>;<CODE>:<text> ESC \` with a fixed text per code:
`EINVAL`, `EBADF`, `ENOMEM`, `ENODATA`, `ENOENT`. There is no reply
without `i`, and `q` limits them as in kitty.

## What havoc tells programs

Only with `sixel` or `kitty` on:

- DA1 `CSI ? 62;1;4;6;9;15 c` (with `sixel=yes`; `4` is sixel).
- XTSMGRAPHICS `CSI ? Pi;Pa;Pv S` (with `sixel=yes`): 256 registers and
  the text area in pixels; settings are refused.
- `CSI 14 t`, `CSI 16 t`, `CSI 18 t`: text area in pixels, cell size,
  size in cells.
- XTVERSION `CSI > q`: `DCS > | havoc(<version>) ST`. havoc does not
  pretend to be another terminal, so some programs need the protocol named:
  `chafa -f sixels`, `timg -p sixel`, `img2sixel`, `mpv --vo=sixel` or
  `--vo=kitty`, `kitten icat`.
- The pty size carries the pixel size (`ws_xpixel`, `ws_ypixel`), also
  updated when the zoom changes the cell size.
- `ED 3` clears the scrollback.
- The pty is read 4 KiB at a time instead of 256 B.

`TERM` stays `xterm-256color`.

## Memory and limits

One store holds all pictures, capped by `[graphics] memory` in pixels:

| Kind | Bytes per pixel |
|---|---|
| sixel (register index, plus a 1-bit mask when some pixel is unset) | 1 to 1.125 |
| kitty, opaque (RGB565) | 2 |
| kitty with alpha (RGB565 and 8-bit alpha) | 3 |

The default of 524288 pixels is about 0.5 to 1.5 MiB, sized for handhelds
with a QVGA-class panel and about 64 MiB of RAM. Sixel builds also add 4
bytes to every cell (screen and scrollback) for the tile.

When a new picture does not fit, havoc frees, in this order: sixel pictures
no cell refers to; kitty images with an id and no placement, oldest first;
pictures with nothing in view (scrollback outside the view, or the other
screen), oldest first. A picture with anything in view is never evicted:
the new one is refused instead (sixel: dropped, kitty: `ENOMEM`).

Other limits: pictures at most 4096 pixels on a side; 255 sixel pictures;
1024 kitty images; 256 placements per screen; 512 bytes of kitty keys per
command; numbers saturate (sixel at 65535, kitty keys at 2^32 - 1).

## Security

Every byte from the pty may come from a remote program. Decoders check
their limits while decoding, base64 and zlib decode into buffers sized in
advance, and replies carry only numbers and fixed text.

`t=f` and `t=t` (files named by the program) are always refused: a remote
program would make havoc open local paths, block on a FIFO or learn which
files exist.

`t=s`, only with `kitty shm=yes`:

- the name is at most 255 bytes of `A-Z a-z 0-9 . _ -`; no `/`, no `.` or
  `..`; havoc adds the leading `/`;
- it is opened read-only with `O_NOFOLLOW` and `O_NONBLOCK`, and must be a
  regular file of havoc's own user, at least as large as the picture needs
  and at most 4 bytes per pixel of the cap;
- only then it is unlinked, mapped, read once and unmapped;
- every failure gives the same reply, `EBADF:shared memory not usable`.

Over ssh the name does not exist on the local machine, so `t=s` fails and
programs that probe for it fall back to `t=d`.

## C8 windows

In a C8 window (8-bit palette output) pictures are dithered to the fixed
6x6x6 colour cube that the palette always holds (indices 16 to 231), with a
4x4 ordered dither anchored to the picture, so the pattern does not move
when the picture scrolls. Text colours are not changed.

## Testing

The tests run on Linux with gcc, clang with libFuzzer, python3 with PIL,
libpng, zlib, and the development files of libwayland and libxkbcommon.
All C tests are built with AddressSanitizer and UBSan.

```
sh tests/run.sh
```

runs, in order:

| Script | Tests |
|---|---|
| `run-tsm.sh` | libtsm strings, replies, tiles and events; same screen as pico for every vector; fuzzer |
| `run-sixel.sh` | sixel decoder against a reference decoder (`tools/sixel_ref.py`), failed allocations; fuzzer |
| `run-kitty.sh` | base64, command parser, replies, `t=s` rules, PNG; fuzzer |
| `run-kplace.sh` | placement lists, alone and on a libtsm screen |
| `run-c8.sh` | the C8 dither |
| `run-gfx.sh` | builds with every variable off, sixel only, kitty only and all three; a store fuzzer; window tests in a fake compositor (`graphics.py`, `kitty.py`, and `c8pics.py` for C8) |

Environment:

- `FUZZ_SECONDS`: time of each fuzzer run, default 60, 0 skips.
- `PICO_TSM` or `PICO_REF`: libtsm before these changes for the same-screen
  test (default `tsm/` of `origin/pico`).
- `PICOWL_PALETTE_XML`: the C8 palette protocol; without it the C8 window
  tests are skipped.
- `HAVOC_GFX_DEBUG`: havoc prints the numbers of pictures and placements
  after each change.
- `HAVOC_FULL_REDRAW`: havoc redraws every cell; the window tests compare
  against it.

Test pictures are made by the tests; the captured streams in
`tests/vectors` come from generated images (`tests/tools/mkimg.py`,
`mkphoto.py`) and can be made again with `regen.sh` there.
