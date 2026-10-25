#!/usr/bin/env bash
# Physical streaming replica (pg_basebackup): inserts, updates, deletes, VACUUM, promotion and merge of a sift_col index with order_columns (the heads of the
# sorted runs, column format minor 18, and the global order of the keys, minor 21) on the primary replay to an index that verifies on the hot standby, and ORDER BY .. LIMIT n over it returns on the standby
# what the normal plan returns there and what the primary returns. Readers on the standby run all the while. The standby is then promoted and takes writes.
. "$(dirname "$0")/lib.bash"
. "$(dirname "$0")/lib_col.bash"

SECS=$(knob SIFT_COL_REPLICA_SECS 15 3)
plan 9
node_init primary 2 "wal_level = replica
max_wal_senders = 4
wal_keep_size = $(knob SIFT_WAL_KEEP 1GB 256MB)
autovacuum = off"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift_test CASCADE" \
	"${COL_TOPN_FIXTURE[@]}" >/dev/null || bail "setup failed"

node_clone primary standby 3 "hot_standby_feedback = on
wal_receiver_status_interval = 1s"
node_start standby || bail "standby start failed"
ok "standby streaming from a base backup"

P=$(dir primary)
cat >"$P/ins.sql" <<'EOS'
INSERT INTO t (k, v, pad) SELECT (random() * 100000)::int, i, repeat('x', 400) FROM generate_series(1, 20) i;
EOS
cat >"$P/upd.sql" <<'EOS'
\set id random(1, 100000)
UPDATE t SET k = k + 1 WHERE id BETWEEN :id AND :id + 100;
DELETE FROM t WHERE id BETWEEN :id + 1000 AND :id + 1020;
EOS
echo "VACUUM t;" >"$P/vac.sql"
cat >"$P/maint.sql" <<'EOS'
SELECT sift.col_promote('t_col');
SELECT sift.col_merge('t_col', random() < 0.3);
EOS
cat >"$P/read.sql" <<'EOS'
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT topn_same();
COMMIT;
EOS
pgbench_bg primary 6 "$SECS" -f "$P/ins.sql@40" -f "$P/upd.sql@10" -f "$P/vac.sql@1" -f "$P/maint.sql@2"
PRIMARY_PID=$PGBENCH_PID
pgbench -n -h "$(dir standby)/sock" -p "$(port standby)" -U postgres -c 2 -j 2 -T "$SECS" -f "$P/read.sql" postgres >"$P/reader.log" 2>&1 &
READER=$!
wait "$PRIMARY_PID"
wait "$READER"
check "churn on the primary had no failed transaction" grep -q "number of failed transactions: 0 " "$P/pgbench.log"
check "readers on the standby had no failed transaction" grep -q "number of failed transactions: 0 " "$P/reader.log"
# the standby's feedback (hot_standby_feedback) holds the horizon of its last reader for a status interval: the pages are all-visible only after it moves
sleep 3
sql primary "VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) t" "SELECT sift.col_merge('t_col', true)" "VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) t" >/dev/null
catchup primary standby

TOPN="SELECT string_agg(k::text, ',' ORDER BY pos) FROM (SELECT row_number() OVER () AS pos, k FROM (SELECT k FROM t ORDER BY k LIMIT 32) x) y"
check "the index verifies on the standby" test -z "$(sql standby "SELECT sift.col_verify('t_col', true)" 2>&1)"
check "the standby's top-N answers are its heap's" test "$(sql standby "SELECT topn_same()")" == t
check "and what the primary returns" test "$(sql standby "SET sift.enable_agg = on" "$TOPN")" == "$(sql primary "SET sift.enable_agg = on" "$TOPN")"
check "the standby's top-N scan answers units from the heads" test "$(sql standby "SET max_parallel_workers_per_gather = 0" "SET sift.test_topn_gorder = off" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) SELECT k FROM t ORDER BY k LIMIT 10" | grep -c 'Units Answered By Run Head')" == 1
check "and reads the global order of the key" test "$(sql standby "SET max_parallel_workers_per_gather = 0" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) SELECT k FROM t ORDER BY k LIMIT 100" | grep -c 'Global Order')" == 1

node_promote standby
for _ in $(seq 100); do [[ $(sql standby "SELECT pg_is_in_recovery()") == f ]] && break; sleep 0.1; done
sql standby "INSERT INTO t (k, v, pad) SELECT i, i, repeat('x', 400) FROM generate_series(1, 500) i" \
	"SELECT sift.col_promote('t_col')" "SELECT sift.col_merge('t_col', true)" "VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) t" >/dev/null
check "the promoted standby takes writes, its index verifies and its top-N answers are its heap's" \
	test "$(sql standby "SELECT topn_same()")" == t -a -z "$(sql standby "SELECT sift.col_verify('t_col', true)" 2>&1)"
finish
