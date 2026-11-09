# Agent guide for havoc-pico

This is a fork of [havoc](https://github.com/ii8/havoc), a minimal Wayland terminal, for small handhelds. The default branch `pico` is upstream commit `73e3467` with patches on top: text-input-v3, per-cell damage, emoji from the wvkbd packs, bitmap fonts and C8 buffers. Read the README and `havoc.cfg` for what havoc does; `git log 73e3467..pico` has the reason for each patch.

## Git

Commit locally. Never run `git push`, rewrite history or tag a release without the owner's permission. Stage exact paths, one logical change per commit.

- Work on a branch and propose it as a pull request against `pico`. Pull requests are opened ready for review, never as drafts. Merging is the owner's decision.
- Each patch on `pico` is one commit that builds on its own. A change that upstream havoc could take stays in its own commit, with nothing handheld-specific in it, so it can be sent upstream unchanged.
- Decisions and evidence (test output, measurements, what ran where) go on the pull request.

## Commit messages

- Subject: `component: summary`, imperative, for example `tsm: stop OSC numbers from overflowing`.
- Body: why, in plain prose. A patch for `pico` ends its body with `Upstream-Status:` (`Pending` until it has been sent upstream).
- End with `Signed-off-by:` read from `git config user.name` and `user.email`, never made up. Add `Assisted-by: <tool>:<model>` when an AI assisted. Never a `Co-Authored-By:` or session trailer.

## Public text

READMEs, documents, comments and commit messages are read by experienced developers who know nothing of how the work was done.

- Keep them short and factual. No marketing and no filler such as "honest", "load-bearing", "robust", "carefully", "comprehensive", "seamless" or "deliberately".
- State what is decided. Mark a plan as a plan.
- Use generic wording for the hardware: handhelds with a QVGA-class panel and about 64 MiB of RAM. Name a device only as an example or as a tested device.
- A tool name that starts a sentence takes a capital, like any other word: "Havoc draws ...". Inline code stays as typed.
- Notes about how an image was made go in an HTML comment, not in visible text.
- Do not refer to files, repositories or sessions the reader cannot open, or to history ("was X, now Y").
- Never write "Yocto" for the build system or ecosystem: say OpenEmbedded or bitbake. Never use Poky.

## Code

- Comments say why, never what the next line does.
- C in the style of the surrounding code, indented with tabs. `tsm/` is libtsm; keep changes there small and separate from the havoc changes that use them.
- The code is under the terms in `LICENSE` (public domain, or MIT). Code taken from elsewhere must be under a license compatible with MIT. The tree has no SPDX headers and no `REUSE.toml`.
- A new feature that needs a library or costs memory can be turned off, with a make variable or a `havoc.cfg` key. With a feature turned off, havoc draws the same pixels as without it. A change in output with nothing set (such as damage, the opaque region or emoji from the default pack directory) is listed under Compatibility in NEWS.
- A new config key is documented in `havoc.cfg` in the same change.

## Build and test

```sh
make CFLAGS="-O2 -DNDEBUG"
make clean
```

- The Makefile needs `pkg-config`, `wayland-scanner`, wayland-protocols and the wayland-client, wayland-cursor and xkbcommon development files. New sources go into `OBJ` in the Makefile; nothing is globbed.
- `PIXFONT_OBJ=pixfont-none.o` builds without the bitmap font data. `EMOJI_DIR` sets the default emoji pack directory.
- `PICOWL_PALETTE_XML=<path>/picowl-palette-v1.xml` builds the C8 path. The XML comes with picowl and is not in this tree; without it there is no C8 code.
- `sh tests/run.sh` runs the test suite: libtsm, the sixel and kitty decoders, the placement lists, the C8 dither and window tests in a fake compositor. `doc/graphics.md` lists what each script needs. Set `PICOWL_PALETTE_XML` to include the C8 tests and `FUZZ_SECONDS=0` to skip the fuzzers. `make check` runs the login shell test (`tests/login.c`) on a pty; it needs no compositor. `HAVOC_FULL_REDRAW` in the environment paints and damages the whole window on every redraw: a picture from it is what the incremental drawing must equal.
- Build with every option off and with every option on before proposing a change. Say whether a result came from the host, a container, qemu or a device.
- Timing and memory are judged on a device, not under emulation.

## Pictures

- NEWS, release text and pull requests show a visible change (glyphs, emoji, pictures, colours) as a before and after pair of screenshots or animations, same scene, same size. A subtle change gets an annotation, an arrow and a short label, as vector SVG over the untouched capture; the unannotated captures stay next to it.
- Captures come from a device or from qemu; the caption says which. One full-screen window unless the change is about tiling. The terminal shows `uname -a` or htop, never an empty prompt.
- Pictures go in `doc/images/` and are committed in the branch before a tag. NEWS names them by path; pull requests and release text embed them by blob URL with `?raw=true`.
- A change that is not visible needs no picture: build variables, config keys, protocol, tests, internal changes. A CPU or memory cost is shown with htop beside the terminal.
- What cannot be shown, or did not run on a device, is said plainly: "Not shown: ...", "Not run on a device: ...".

## Releases

Releases are made by the owner from `pico`, the default branch: the NEWS section is merged through a pull request, an annotated tag goes on that commit of `pico`, then the GitHub release is made from the tag. NEWS and the pictures are done before the tag. The tag names for pico releases are not decided yet; `0.1.0` to `0.7.0` are upstream releases.
