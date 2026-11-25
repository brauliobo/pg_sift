#!/usr/bin/env bash
# A regress test that calls a sift._test_ function, creates the test-only extension (pg_sift_test) or sets a test-only setting (sift.test_*) needs the test-only build: its name matches
# TESTING_REGRESS of the Makefile, or release-library-regress (the stock build) runs it and fails on a missing function or setting (needs no PostgreSQL and no build). The settings the stock
# library has (scripts/test-settings-shipped.allow) and a test that probes a setting with current_setting(name, true) are exempt.
#
#   scripts/check-testing-regress.sh
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -euo pipefail
cd "$(dirname "$0")/.."

shipped=$(sed -n 's/^\(sift\.test_[a-z0-9_]*\)\t.*/\1/p' scripts/test-settings-shipped.allow | tr '\n' '|' | sed 's/|$//')
mapfile -t pats < <(sed -En 's/^TESTING_REGRESS \+?= //p' Makefile | tr ' ' '\n')
bad=0
# the list is one name for each line, in byte order inside a block (the lines up to the next comment), no name twice: concurrent additions merge cleanly
grep -nE '^TESTING_REGRESS \+?= .* ' Makefile && { echo "TESTING_REGRESS: one name or pattern for each line"; bad=1; }
dups=$(printf '%s\n' "${pats[@]}" | sort | uniq -d)
[[ -z $dups ]] || { echo "TESTING_REGRESS: listed twice: $dups"; bad=1; }
unsorted=$(LC_ALL=C awk '/^TESTING_REGRESS \+= / { if (prev != "" && $3 < prev) print $3 " after " prev; prev = $3; next } { prev = "" }' Makefile | LC_ALL=C sort)
[[ -z $unsorted ]] || { echo "TESTING_REGRESS: not in byte order (LC_ALL=C sort): $unsorted"; bad=1; }
# the tests that need the test-only build: one perl for all of them
mapfile -t sqls < <(find test/sql -name '*.sql' | LC_ALL=C sort)
mapfile -t needing < <(SHIPPED=$shipped perl -0777 -ne '
	BEGIN { $shipped = qr/^(?:$ENV{SHIPPED})$/ }
	if (/sift\._test_|^create extension (?:if not exists )?pg_sift_test/mi) { print "$ARGV\n"; next }
	my %probed = map { $_ => 1 } /current_setting\(\s*\x27(sift\.test_\w+)\x27\s*,\s*true\s*\)/g;
	for (/sift\.test_\w+/g) { if (!$probed{$_} && !/$shipped/) { print "$ARGV\n"; last } }
' "${sqls[@]}")
for f in "${needing[@]}"; do
	t=${f#test/sql/}; t=${t%.sql}
	for p in "${pats[@]}"; do [[ $t == ${p//%/*} ]] && continue 2; done
	echo "$t calls a sift._test_ function, sets a sift.test_ setting or creates pg_sift_test: add it to TESTING_REGRESS in the Makefile"; bad=1
done
((! bad)) || exit 1
echo "check-testing-regress: ok, the tests that call sift._test_ functions, set sift.test_ settings or create pg_sift_test are in TESTING_REGRESS"
