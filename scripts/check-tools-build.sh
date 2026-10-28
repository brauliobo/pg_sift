#!/usr/bin/env bash
# The standalone harnesses (tools/codec, colwide, kernels, aggkern and the coverage-guided targets of test/fuzz) compile src/ files outside the extension: a new
# #include chain in a file they share breaks them with no test in the way, and only a fuzz run finds it. This builds them, nothing is run (about 10 s;
# needs the PostgreSQL server headers, pg_config).
#
#   scripts/check-tools-build.sh
set -euo pipefail
cd "$(dirname "$0")/.."

out=$(mktemp -d "${SIFT_TMP:-/tmp}/sift-tools-XXXXXX")
trap 'rm -rf "$out"' EXIT
build() { local dir=$1; shift; make -s -j4 -C "$dir" OUT="$out/$(basename "$dir")" "${@/#/$out/$(basename "$dir")/}" >"$out/$(basename "$dir").log" 2>&1 || { cat "$out/$(basename "$dir").log"; echo "check-tools-build: $dir does not build"; return 1; }; }
bad=0
build tools/codec test fuzz || bad=1
build tools/colwide test || bad=1
build tools/kernels test || bad=1
build tools/aggkern test || bad=1
build test/fuzz colchunk shred bm25 || bad=1
((! bad)) || exit 1
echo "check-tools-build: ok, the harnesses of tools/ and test/fuzz build"
