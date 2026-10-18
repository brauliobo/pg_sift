#!/usr/bin/env bash
# The regression tests that need no test-only function (limits, privileges, planner...) against the release library, built
# without SIFT_TESTING: the library users get. It builds a copy of the tree, so that its objects and test/results stay apart from
# the testing build's, installs it under $SIFT_TMP (default /tmp/sift-release-library) and runs them on a cluster of its own on
# SIFT_PORT + 8 (scripts/ci.sh gives each job the port block of its lane).
#
#   scripts/release-library-check.sh [TEST...]     (without names: every test the release build can run)
set -uo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
base=${SIFT_TMP:-/tmp/sift-release-library}
export SIFT_TMP=$base/run SIFT_ROOT=$base/run/root SIFT_PORT=$(( ${SIFT_PORT:-56100} + 8 )) SIFT_TESTING=
src=$base/src
mkdir -p "$src"
tar -C "$REPO" --exclude=.git --exclude=bench --exclude=docs --exclude=sqlite --exclude=packaging --exclude='*.o' --exclude='*.d' \
	--exclude='*.so' --exclude=test/results --exclude=.build-flavor -cf - . | tar -C "$src" -xf -
rc=0
"$src/scripts/pgtest.sh" install && "$src/scripts/pgtest.sh" start && "$src/scripts/pgtest.sh" regress "$@" || rc=1
"$src/scripts/pgtest.sh" stop || rc=1
exit $rc
