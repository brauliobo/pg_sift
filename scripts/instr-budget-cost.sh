#!/usr/bin/env bash
# Absolute instruction budgets of the statements a sift_col index adds work to on every request, in callgrind instructions of a single-user backend (they do not move with the load of
# the host): planning a trivial statement on a table with the index, a single-row INSERT in its own transaction, a batch INSERT per row, and the VACUUM of a table whose index is unchanged.
# Measured (20,000 rows): plan 124,148, over no index 24,701, insert 65,427, batch 17,205 a row, three VACUUMs 7,536,543. Each is a ceiling about 1.5 times; a regression that makes the common request dearer fails here, and so does one that makes planning of a table with the index
# cost more than PLAN_OVER instructions over the same table with no index.
#
#   scripts/instr-budget-cost.sh          (SIFT_TMP and SIFT_PORT as scripts/pgtest.sh; needs valgrind and an installed build: scripts/pgtest.sh install)
set -euo pipefail

. "$(dirname "$0")/instr-budget-lib.sh"
PLAN_MAX=${PLAN_MAX:-190000}
PLAN_OVER=${PLAN_OVER:-40000}
INSERT_MAX=${INSERT_MAX:-100000}
BATCH_ROW_MAX=${BATCH_ROW_MAX:-26000}
VACUUM_MAX=${VACUUM_MAX:-11000000}
N=${N:-100}
BATCH=${BATCH:-2000}
ib_begin instr-budget-cost 56234 <<'SQL'
CREATE TABLE cb_none (id int PRIMARY KEY, tenant int, amount numeric(10,2), label text) WITH (autovacuum_enabled = off);
CREATE TABLE cb_sift (LIKE cb_none INCLUDING ALL) WITH (autovacuum_enabled = off);
INSERT INTO cb_none SELECT g, g % 50, g % 1000, 'x' || g % 97 FROM generate_series(1, 20000) g;
INSERT INTO cb_sift SELECT * FROM cb_none;
CREATE INDEX cb_sift_i ON cb_sift USING sift_col (id, tenant, amount, label);
VACUUM (FREEZE, ANALYZE) cb_none;
VACUUM (FREEZE, ANALYZE) cb_sift;
SQL
run() { # NAME FUNCTION: the statements on stdin, instructions inside FUNCTION
	local f=$SIFT_TMP/ibc_$1.sql
	{ echo "SET max_parallel_workers_per_gather = 0"; cat; } > "$f"
	ib_ir "$f" "$2"
}
rep() { for i in $(seq "$N"); do echo "${1//\{i\}/$i}"; done; }
plan_sift=$(rep 'EXPLAIN SELECT * FROM cb_sift WHERE id = {i}' | run plan_sift PortalRun)
plan_none=$(rep 'EXPLAIN SELECT * FROM cb_none WHERE id = {i}' | run plan_none PortalRun)
insert_one=$(rep "INSERT INTO cb_sift VALUES (100000 + {i}, 7, 3.5, 'abc')" | run insert_one PortalRun)
insert_batch=$(echo "INSERT INTO cb_sift SELECT 200000 + g, g % 50, g % 1000, 'x' || g % 97 FROM generate_series(1, $BATCH) g" | run insert_batch PortalRun)
vacuum_idle=$(printf 'VACUUM cb_none\nVACUUM cb_sift\nVACUUM cb_sift\n' | run vacuum_idle PortalRun)
ib_end
p=$((plan_sift / N)); o=$(((plan_sift - plan_none) / N)); i=$((insert_one / N)); b=$((insert_batch / BATCH))
echo "instr-budget-cost: plan $p (max $PLAN_MAX), plan over no index $o (max $PLAN_OVER), insert $i (max $INSERT_MAX), batch insert $b a row (max $BATCH_ROW_MAX), vacuum x3 $vacuum_idle (max $VACUUM_MAX)"
[[ $p -le $PLAN_MAX && $o -le $PLAN_OVER && $i -le $INSERT_MAX && $b -le $BATCH_ROW_MAX && $vacuum_idle -le $VACUUM_MAX ]]
