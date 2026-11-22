#!/usr/bin/env bash
# A regress test that turns the maintenance worker on (sift.col_auto_merge = on without sift.async_maintenance forced off) and prints what the worker changes (segments, store codes,
# maintenance advice, all-visible pages) reads a state that depends on who came first: the worker that promotes the write buffer in the middle of a statement, or vacuums and merges beside
# the test's own VACUUM, changes the answer in a few runs of a hundred. Such a test either keeps the worker off (the test clusters run with sift.col_auto_merge = off) or is in
# scripts/worker-tests.allow with the reason its output does not depend on the worker's timing (it waits for a condition, or asserts only what holds either way).
#
#   scripts/check-worker-tests.sh [--selftest]
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -euo pipefail
cd "$(dirname "$0")/.."

ALLOW=scripts/worker-tests.allow
ENABLES='(sift\.col_auto_merge[[:space:]]*(=|TO)[[:space:]]*(on|true)|set_config\([[:space:]]*.sift\.col_auto_merge.[[:space:]]*,[[:space:]]*.(on|true).)'
WORKER_ON='sift\.async_maintenance[[:space:]]*(=|TO)[[:space:]]*(on|true)'
WORKER_OFF='sift\.async_maintenance[[:space:]]*(=|TO)[[:space:]]*(off|false)'
PRINTS='col_segments|col_maintenance_status|col_maintenance_due|relallvisible|blocked_by'

runs_worker() { grep -qiE "$ENABLES" "$1" && { grep -qiE "$WORKER_ON" "$1" || ! grep -qiE "$WORKER_OFF" "$1"; }; }
prints_worker_state() { grep -qiE "$PRINTS" "$1"; }

violations() {
	for f in "$@"; do
		runs_worker "$f" && prints_worker_state "$f" || continue
		t=$(basename "$f" .sql)
		grep -qE "^$t	." "$ALLOW" || echo "$t"
	done
}

selftest() {
	d=$(mktemp -d); trap 'rm -rf "$d"' EXIT
	printf 'SET sift.col_auto_merge = on;\nSELECT count(*) FROM sift.col_segments(%s);\n' "'i'" >"$d/flagged.sql"
	printf 'SET sift.col_auto_merge = on;\nSET sift.async_maintenance = off;\nSELECT * FROM sift.col_segments(%s);\n' "'i'" >"$d/off.sql"
	printf 'SET sift.col_auto_merge = on;\nSELECT 1;\n' >"$d/silent.sql"
	printf 'SELECT * FROM sift.col_segments(%s);\n' "'i'" >"$d/default.sql"
	printf 'SELECT set_config(%s, %s, false);\nSET sift.async_maintenance = on;\nSELECT blocked_by FROM x;\n' "'sift.col_auto_merge'" "'on'" >"$d/config.sql"
	[[ $(violations "$d"/*.sql | tr '\n' ' ') == "config flagged " ]] || { echo "check-worker-tests selftest: wrong set flagged: $(violations "$d"/*.sql | tr '\n' ' ')"; exit 1; }
	echo "check-worker-tests selftest: ok"
}

[[ ${1:-} != --selftest ]] || { selftest; exit 0; }
bad=0
for t in $(violations test/sql/*.sql); do
	echo "$t turns the maintenance worker on and prints segments, store counts, advice or all-visible pages: keep the worker off in the test, or list it in $ALLOW with the reason its output does not depend on the worker's timing"
	bad=1
done
while IFS=$'\t' read -r t reason; do
	[[ $t == '#'* || -z $t ]] && continue
	[[ -n $reason ]] || { echo "$ALLOW: $t has no reason"; bad=1; }
	[[ -f test/sql/$t.sql ]] || { echo "$ALLOW: $t is not a regress test"; bad=1; continue; }
	[[ -z $(violations "test/sql/$t.sql") ]] && runs_worker "test/sql/$t.sql" && prints_worker_state "test/sql/$t.sql" || { echo "$ALLOW: $t no longer turns the worker on and prints its effects: remove the line"; bad=1; }
done <"$ALLOW"
((! bad)) || exit 1
echo "check-worker-tests: ok, the regress tests that turn the worker on and print what it changes are all listed with a reason"
