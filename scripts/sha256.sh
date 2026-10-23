#!/bin/sh
# sha256sum on a host whose sha256sum is not GNU's (macOS: the BSD one wants `-c --quiet -` spelled out, and older releases have only shasum).
#   scripts/sha256.sh [FILE...]          the digests, as `HASH  FILE`
#   scripts/sha256.sh -c [--quiet] [FILE]  verifies such a list (FILE or standard input)
# The scripts and tests call this one, never sha256sum (scripts/ci.sh --selftest).
if sha256sum --version 2>/dev/null | grep -q GNU; then exec sha256sum "$@"; fi
exec shasum -a 256 "$@"
