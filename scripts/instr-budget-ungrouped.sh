#!/usr/bin/env bash
# Instruction budget of an ungrouped aggregate scan that the pre-aggregates do not answer, in callgrind instructions of a single-user backend: instructions do not move with the
# load of the host, so the budget is a number, not a ratio. An ungrouped aggregate has one group: counting and summing its rows is a pass over the selection, not a
# scatter through an array of group ids that are all 0. The statement below reads every row of the table twice (two runs, the second is measured); fails over BUDGET.
#
#   scripts/instr-budget-ungrouped.sh          (SIFT_TMP and SIFT_PORT as scripts/pgtest.sh; needs valgrind and an installed build: scripts/pgtest.sh install)
#
# Measured at 200,000 rows: 18,136,000 instructions with the counts and sums scattered through the group ids, 10,987,000 with one pass, 9,990,000 with the numeric sum of a
# block added by one kernel (sum_i64); BUDGET is 10,500,000.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
BUDGET=${BUDGET:-10500000}
command -v valgrind >/dev/null || { echo "instr-budget-ungrouped: valgrind is not installed, skipped"; exit 0; }
export SIFT_TMP=${SIFT_TMP:-/tmp/sift-instr} SIFT_PORT=${SIFT_PORT:-56232}
cd "$REPO"
scripts/pgtest.sh stop >/dev/null 2>&1 || true
rm -rf "$SIFT_TMP/data"
scripts/pgtest.sh start >/dev/null
eval "$(scripts/pgtest.sh env | grep '^export')"
P="psql -X -q -At -d postgres"
$P -c "CREATE EXTENSION pg_sift" -c "ALTER DATABASE postgres SET sift.col_auto_merge = off" >/dev/null
$P >/dev/null <<'SQL'
CREATE TABLE ug (id int, v int, m numeric(12,2)) WITH (autovacuum_enabled = off);
INSERT INTO ug SELECT g, hashint4(g) & 1023, (hashint4(g + 5) & 65535) / 100.0 FROM generate_series(1, 200000) g;
CREATE INDEX ug_idx ON ug USING sift_col (id, v, m);
VACUUM (ANALYZE) ug;
SQL
scripts/pgtest.sh stop >/dev/null
run() { # RUNS: the statement RUNS times in one backend
	{ echo "SET max_parallel_workers_per_gather = 0"; echo "SET sift.agg_preagg = off"; for _ in $(seq "$1"); do echo "SELECT count(*), sum(v), sum(m) FROM ug"; done; } > "$SIFT_TMP/ug$1.sql"
	valgrind --tool=callgrind --collect-atstart=no --toggle-collect=PortalRun --callgrind-out-file="$SIFT_TMP/ug$1.cg" \
		postgres --single -D "$SIFT_TMP/data" -c sift.async_maintenance=off -c jit=off -c autovacuum=off postgres < "$SIFT_TMP/ug$1.sql" 2>&1 >/dev/null | sed -n 's/.*Collected : *\([0-9]*\).*/\1/p' | tail -n 1
}
one=$(run 1); two=$(run 2)
rm -rf "$SIFT_TMP/data"
ir=$((two - one))
echo "instr-budget-ungrouped: count, sum(int), sum(numeric) of 200,000 rows in $ir instructions (budget $BUDGET)"
[[ $ir -gt 0 && $ir -le $BUDGET ]]
