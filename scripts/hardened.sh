#!/usr/bin/env bash
# The hardened flavor in CI (docs/SAFETY.md): the tree is built with SIFT_HARDEN (default 1: stack protector, FORTIFY_SOURCE=3, zero-initialized
# locals, trap-mode UBSan for bounds, shifts, nulls and pointer overflow...) in a private copy, a cluster of its own loads it, and the regression
# tests run on it. A violation is a trap instruction: the backend dies, the test fails. Complements the ASan flavor (which sees heap and stack
# but not an overrun inside a palloc chunk or a struct) with checks that cost nothing to enable and run on the release code generation.
#
#   scripts/hardened.sh [--quick|TEST...]   (SIFT_HARDEN=pkg|init|1, default 1; SIFT_TMP and SIFT_PORT as scripts/pgtest.sh); --quick: the six tests of the quick tier
#
# The default tests are the decoders and the readers of stored bytes: codec, column unit, kernels at every SIMD level, shredder, term
# blobs, corruption tests, positions, the table AM. scripts/ci.sh runs this as the job `hardened`. SIFT_PREBUILT_HARDENED (scripts/ci-build.sh): the staged root of
# the hardened flavor, so that nothing is copied or compiled here. Without it the private copy is compiled with the debug info of scripts/ci-build.sh (line tables, SIFT_FAST_DEBUG) through a
# compiler cache keyed by content (CCACHE_BASEDIR): the copy is at a path of this run, and a cache keyed by it missed every object.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
source "$REPO/scripts/source-snapshot.sh"
TMP=${SIFT_TMP:-/tmp/sift-hardened-$$}
WORK=$TMP/hardened
QUICK_TESTS=(col/codec/basic col/enc bm25/kernels/simd col/shred/values bm25/verify/corrupt bm25/query/errors)
if [[ ${1:-} == --quick ]]; then TESTS=("${QUICK_TESTS[@]}"); else TESTS=("$@"); fi
(( ${#TESTS[@]} )) || TESTS=(col/codec/basic col/codec/simd col/enc col/kernels_simd bm25/kernels/simd bm25/kernels/filter col/shred/values col/shred/kernels col/shred/diff
	bm25/verify/corrupt bm25/store/corrupt bm25/query/errors bm25/phrase_positions bm25/position_blocks bm25/index/build_postings table/basic table/agg/basic)
export SIFT_HARDEN=${SIFT_HARDEN:-1}
# the trap flavor, not the ASan flavor scripts/ci.sh full exports
unset SANITIZE

rm -rf "$WORK" && mkdir -p "$WORK/tree"
if [[ -n ${SIFT_PREBUILT_HARDENED:-} ]]; then
	# the staged root of scripts/ci-build.sh: nothing is copied or compiled, the tests run from this checkout
	cd "$REPO"
	export SIFT_TMP=$WORK/cluster SIFT_ROOT=$WORK/root SIFT_PREBUILT_ROOT=$SIFT_PREBUILT_HARDENED
	scripts/pgtest.sh install
else
	# a private copy: the build writes into the tree and the flavor must not disturb the checkout's own build
	copy_git_sources "$REPO" "$WORK/tree"
	cd "$WORK/tree"
	export SIFT_TMP=$WORK/cluster SIFT_ROOT=$WORK/root SIFT_FAST_DEBUG=1 CCACHE_BASEDIR=$WORK/tree CCACHE_NOHASHDIR=1
	scripts/pgtest.sh install
	drop_build "$WORK/tree"
fi
trap 'scripts/pgtest.sh stop >/dev/null 2>&1 || true; [[ -n ${KEEP:-} ]] || rm -rf "$WORK"' EXIT
scripts/pgtest.sh start
scripts/pgtest.sh regress "${TESTS[@]}"
echo "hardened: ${#TESTS[@]} tests passed on SIFT_HARDEN=$SIFT_HARDEN ($(gcc --version | head -1))"
