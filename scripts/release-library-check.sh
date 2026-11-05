#!/usr/bin/env bash
# The regression tests that need no test-only function (limits, privileges, planner...) against the release library, built
# without SIFT_TESTING: the library users get. It builds a copy of the tree, so that its objects and test/results stay apart from
# the testing build's, installs it under $SIFT_TMP (default /tmp/sift-release-library) and runs them on a cluster of its own on
# SIFT_PORT + 8 (scripts/ci.sh gives each job the port block of its lane).
#
#   scripts/release-library-check.sh [TEST...]     (without names: every test the release build can run)
#   scripts/release-library-check.sh --build       only the release build and its install, no cluster and no test: the gate of a change in src/ or the Makefile
#                                                  (a compile of the flavour the testing shards never build; scripts/land-local.sh runs it)
#
# SIFT_TESTING is set, and empty, on purpose, for this script only: scripts/pgtest.sh reads ${SIFT_TESTING-1}, so unset builds the test library and empty the release one.
# A shell that exported the empty value (a copy of the line below, a session that ran this flavour before) builds the release library when it asks for the test
# one, and its run looks like a testing run that fails the tests that assert what only the test library does (build_pays in src/col/build_par.c). Set SIFT_TESTING=1,
# or unset it, in the command of a run that wants the test library; scripts/pgtest.sh install prints the flavour it builds.
set -uo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
build_only=; [[ ${1:-} != --build ]] || { build_only=1; shift; }
base=${SIFT_TMP:-/tmp/sift-release-library}
export SIFT_TMP=$base/run SIFT_ROOT=$base/run/root SIFT_PORT=$(( ${SIFT_PORT:-56100} + 8 )) SIFT_TESTING= SIFT_TEST_HELPERS=1
src=$base/src
mkdir -p "$src"
tar -C "$REPO" --exclude=.git --exclude=bench --exclude=docs --exclude=sqlite --exclude=packaging --exclude='*.o' --exclude='*.d' \
	--exclude='*.so' --exclude=test/results --exclude=.build-flavor -cf - . | tar -C "$src" -xf -
if [[ -n $build_only ]]; then
	"$src/scripts/pgtest.sh" install >"$base/build.log" 2>&1 || { tail -n 20 "$base/build.log"; exit 1; }
	exit 0
fi
rc=0
"$src/scripts/pgtest.sh" install && "$src/scripts/pgtest.sh" start && "$src/scripts/pgtest.sh" regress "$@" || rc=1
"$src/scripts/pgtest.sh" stop || rc=1
exit $rc
