# Contributing to havoc-pico

This repository is a fork of [havoc](https://github.com/ii8/havoc). The
branch `pico` carries the patches for small handhelds on top of upstream
commit `73e3467`.

## Reporting bugs and requesting features

Issues are not enabled here. A problem that also occurs in upstream havoc
goes to [its issue tracker](https://github.com/ii8/havoc/issues). A
problem in one of the `pico` patches goes into a
[pull request](../../pulls) with the fix, or with a description: the
output of `havoc -v`, the compositor, the device, the `havoc.cfg` in use
and what was sent to the terminal.

## Submitting changes

1. Open a [pull request](../../pulls) against `pico`, ready for review.
   Merging is the owner's decision. Decisions and test results are
   recorded on the pull request.
2. Keep commits to one logical change that builds on its own. The
   subject is `component: summary`; the message explains why, not what
   the diff already shows, and ends with a `Signed-off-by:` line from
   `git config`. A patch for `pico` carries `Upstream-Status:` in its
   body.
3. A change that upstream havoc could take is kept in its own commit,
   without handheld-specific code, so it can be sent upstream as it is.
4. New code is under the terms in [LICENSE](LICENSE) (public domain, or
   MIT). Code copied from elsewhere must be under a license compatible
   with MIT.
5. A feature that costs memory or needs a library can be turned off with
   a make variable or a `havoc.cfg` key, and NEWS lists under
   Compatibility any change in output with nothing set. A new key is
   documented in `havoc.cfg`. The change builds with every variable off and every
   variable on.
6. The message says how the change was verified: on the host, in a
   container, under qemu or on which device. Timing and memory are
   measured on a device.
7. A change that is visible on the screen shows before and after
   screenshots or animations in the pull request, annotated when the
   difference is subtle, from a device or qemu. A
   change that cannot be shown says so.
