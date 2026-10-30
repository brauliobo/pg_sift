#!/usr/bin/env bash
# CREATE INDEX CONCURRENTLY of a sift_col index (with key_cut, which reserves a gap, and without; parallel workers) on a table that writers insert into, update and delete from
# the whole time, and that aborts some of its transactions: the index ends valid and ready, verifies against the heap (heapallindexed), answers the grouped aggregate as the plain
# plan does through the aggregate pushdown, and so does the standby that replayed the build. The leak gate of every node runs at the end (finish).
# SIFT_BC_ROWS (300000, 100000 quick).
. "$(dirname "$0")/lib.bash"

ROWS=$(knob SIFT_BC_ROWS 300000 100000)
node_init primary 2 "wal_level = replica
max_wal_senders = 4
wal_keep_size = 256MB
autovacuum = off
max_worker_processes = 16
max_parallel_workers = 8"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift" \
	"CREATE TABLE t (id int, grp int, amount numeric(10,2), pad text) WITH (autovacuum_enabled = off)" \
	"INSERT INTO t SELECT g, g % 500, g % 997, md5(g::text) || md5((g * 7)::text) FROM generate_series(1, $ROWS) g" \
	"VACUUM (FREEZE, ANALYZE) t" >/dev/null || bail "setup failed"
node_clone primary standby 3 "hot_standby_feedback = on
wal_receiver_status_interval = 1s"
node_start standby || bail "standby start failed"
catchup primary standby || bail "the standby did not catch up"

cat >"$(dir primary)/dml.sql" <<SQL
\set id random(1, $ROWS)
\set id2 random(1, $ROWS)
BEGIN;
UPDATE t SET amount = amount + 1, grp = (grp + 1) % 500 WHERE id = :id;
INSERT INTO t VALUES ($ROWS + :id, :id % 500, :id % 997, 'w');
DELETE FROM t WHERE id = :id2 AND id % 11 = 0;
END;
SQL
cat >"$(dir primary)/abort.sql" <<SQL
\set id random(1, $ROWS)
BEGIN;
UPDATE t SET amount = amount + 5 WHERE id = :id;
INSERT INTO t VALUES (-1, 7, 1, 'aborted');
ROLLBACK;
SQL

GROUPS_Q="SELECT coalesce(string_agg(grp || ':' || n || ':' || s, ',' ORDER BY grp), '') FROM (SELECT grp, count(*) AS n, sum(amount) AS s FROM t GROUP BY grp) x"
answer() { sql "$1" "LOAD 'pg_sift'" "SET sift.enable_agg = $2" "SET max_parallel_workers_per_gather = 0" "$GROUPS_Q" | tail -1; }
pushdown() { sql "$1" "LOAD 'pg_sift'" "SET sift.enable_agg = on" "SET max_parallel_workers_per_gather = 0" "EXPLAIN (COSTS OFF) $GROUPS_Q" | grep -c SiftAgg; }
valid() { [[ $(sql primary "SELECT indisvalid AND indisready FROM pg_index WHERE indexrelid = 't_idx'::regclass") == t ]]; }

variant() { # LABEL OPTIONS
	local before
	pgbench_bg primary 4 600 -f "$(dir primary)/dml.sql@4" -f "$(dir primary)/abort.sql@1"
	local writers=$PGBENCH_PID
	sleep 1
	before=$(sql primary "SELECT count(*) FROM t WHERE pad = 'w'")
	sql primary "SET max_parallel_maintenance_workers = 4" "SET min_parallel_table_scan_size = 0" \
		"CREATE INDEX CONCURRENTLY t_idx ON t USING sift_col (id, grp, amount) $2" >/dev/null 2>"$(dir primary)/cic.err"
	local built=$?
	local during=$(( $(sql primary "SELECT count(*) FROM t WHERE pad = 'w'") - before ))
	kill "$writers" 2>/dev/null; wait "$writers" 2>/dev/null
	echo "# $1: $during rows inserted by the writers while the index was built" >&2
	check "$1: CREATE INDEX CONCURRENTLY under writers finishes" test "$built" -eq 0
	check "$1: ... with the writers inserting while it ran" test "$during" -gt 0
	check "$1: ... the index is valid and ready" valid
	check "$1: ... it verifies against the heap" test -z "$(sql primary "SELECT sift.col_verify('t_idx', true)" 2>&1)"
	check "$1: ... the aggregate pushdown reads it" test "$(pushdown primary)" -ge 1
	check "$1: ... and answers the grouped aggregate as the plain plan does" test "$(answer primary on)" == "$(answer primary off)"
	catchup primary standby
	check "$1: ... the standby answers as the primary" test "$(answer standby on)" == "$(answer primary off)"
	check "$1: ... and verifies the index" test -z "$(sql standby "SELECT sift.col_verify('t_idx', true)" 2>&1)"
	sql primary "DROP INDEX t_idx" >/dev/null
	catchup primary standby
}

variant "key_cut" "WITH (key_cut = grp)"
variant "no key_cut" ""
echo "1..$NTEST"
finish
