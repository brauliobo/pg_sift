#!/usr/bin/env bash
# A regress test that calls a sift._test_ function needs the test-only build: its name matches TESTING_REGRESS of the Makefile, or
# release-library-regress (the stock build) runs it and fails on a missing function (needs no PostgreSQL and no build).
#
#   scripts/check-testing-regress.sh
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -euo pipefail
cd "$(dirname "$0")/.."

mapfile -t pats < <(sed -En 's/^TESTING_REGRESS \+?= //p' Makefile | tr ' ' '\n')
bad=0
for f in $(grep -l 'sift\._test_' test/sql/*.sql); do
	t=$(basename "$f" .sql)
	for p in "${pats[@]}"; do [[ $t == ${p//%/*} ]] && continue 2; done
	echo "$t calls a sift._test_ function: add it to TESTING_REGRESS in the Makefile"; bad=1
done
((! bad)) || exit 1
echo "check-testing-regress: ok, the tests that call sift._test_ functions are in TESTING_REGRESS"
