#!/usr/bin/env bash
# A regress test that calls a sift._test_ function, creates the test-only extension (pg_sift_test) or sets a test-only setting (sift.test_*) needs the test-only build: its name matches
# TESTING_REGRESS of the Makefile, or release-library-regress (the stock build) runs it and fails on a missing function or setting (needs no PostgreSQL and no build). The setting the stock
# library has (sift.test_search_chunk_sbs) and a test that probes a setting with current_setting(name, true) are exempt.
#
#   scripts/check-testing-regress.sh
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -euo pipefail
cd "$(dirname "$0")/.."

mapfile -t pats < <(sed -En 's/^TESTING_REGRESS \+?= //p' Makefile | tr ' ' '\n')
bad=0
needs_testing() {
	grep -q -i -E 'sift\._test_|^create extension (if not exists )?pg_sift_test' "$1" ||
		{ ! grep -q "current_setting('sift\.test_" "$1" && grep -o -E 'sift\.test_[a-z_]+' "$1" | grep -qv '^sift\.test_search_chunk_sbs$'; }
}
for f in test/sql/*.sql; do
	needs_testing "$f" || continue
	t=$(basename "$f" .sql)
	for p in "${pats[@]}"; do [[ $t == ${p//%/*} ]] && continue 2; done
	echo "$t calls a sift._test_ function, sets a sift.test_ setting or creates pg_sift_test: add it to TESTING_REGRESS in the Makefile"; bad=1
done
((! bad)) || exit 1
echo "check-testing-regress: ok, the tests that call sift._test_ functions, set sift.test_ settings or create pg_sift_test are in TESTING_REGRESS"
