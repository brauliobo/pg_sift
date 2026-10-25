#!/usr/bin/env bash
# VACUUM moves the segments that end the file down into free blocks (a copy of each, the WAL of its bytes) so that the file can be cut. For an index
# that is due for the automatic rewrite the rewrite writes them into those blocks, and the move only put it off: the WAL budget of the rewrite counts
# from the write of the oldest large segment, which the move made new, so every second round the rewrite was refused and the units stayed dirty.
# Here, rounds of churn, VACUUM and rewrite on a table whose VACUUM has sift.col_auto_merge on move nothing, and on one whose VACUUM has it off they do.
. "$(dirname "$0")/lib.bash"

plan 4
node_init sp 24 "autovacuum = off
sift.log_min_duration = 0"
node_start sp || bail "start failed"
sql sp "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"
for t in t_off t_on; do
	sql sp "CREATE TABLE $t (id int, key int, kind text, amount numeric(10,2), pad text)" \
		"INSERT INTO $t SELECT g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 100) FROM generate_series(1, 100000) g" \
		"CREATE INDEX ${t}_idx ON $t USING sift_col (id, key, kind, amount)" "VACUUM $t" >/dev/null || bail "setup of $t failed"
done
LOG=$(dir sp)/postgres.log
moves() { grep -c "relocated index \"$1_idx\"" "$LOG"; }
# a round: every thirty-third row, the VACUUM with the given settings, the rewrite
round() { sql sp "UPDATE $1 SET amount = amount + 1 WHERE id % 33 = $3" "SET sift.col_auto_merge = $2" "SET sift.async_maintenance = $2" "VACUUM $1" "SELECT sift.col_maintain('$1_idx')" >/dev/null; }
exact() { sql sp "SELECT count(*) FILTER (WHERE exact) || '/' || count(*) FROM sift.col_unit_agg('$1_idx', 'amount')"; }

for i in 1 2 3 4 5 6; do round t_off off "$i"; done
check "off: VACUUM moved a segment in some round" test "$(moves t_off)" -ge 1
for i in 1 2 3 4 5 6; do round t_on on "$i"; done
check "on: VACUUM moved none, the rewrite writes them" test "$(moves t_on)" == 0
check "on: every unit answers from its pre-aggregates after the last round" test "$(exact t_on)" == "$(sql sp "SELECT count(*) || '/' || count(*) FROM sift.col_unit_agg('t_on_idx', 'amount')")"
check "on: the answers are those of the heap" test "$(sql sp "SET sift.enable_agg = on" "SELECT sum(amount) FROM t_on")" == "$(sql sp "SET sift.enable_agg = off" "SELECT sum(amount) FROM t_on")"
finish
