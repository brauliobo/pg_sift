#!/usr/bin/env bash
# Self-maintaining background health: the defaults of the maintenance workers and of the dead-row rewrite, the queue that holds the requests of a server with many
# relations, and the cut of the file end that finds the gap between the statements of a reader (it tries again for a few milliseconds without ever queueing, so no
# reader waits behind it).
. "${SIFT_REPO:?}/test/t/lib/common.bash"

ROUNDS=$(knob SIFT_SH_ROUNDS 6 4)
REQUESTS=1500
plan 10

node_init mh 1 "autovacuum = off
max_worker_processes = 8"
node_start mh || bail "start failed"
D=$(dir mh)
sql mh "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "setup failed"

setting() { sql mh "LOAD 'pg_sift'" "SHOW $1"; }
check "sift.max_maint_workers is 4 by default" test "$(setting sift.max_maint_workers)" == 4
check "sift.dead_rewrite_pct is 10 by default" test "$(setting sift.dead_rewrite_pct)" == 10

sql mh "ALTER SYSTEM SET sift.max_maint_workers = 16" >/dev/null 2>&1
pg_ctl -D "$D/data" reload >/dev/null
check "sift.max_maint_workers takes up to 16" wait_for "[[ \$(setting sift.max_maint_workers) == 16 ]]" 10
check "the workers of the queue and of the other kinds are held to half of max_worker_processes" \
	test "$(sql mh "SELECT sift._test_maint_workers_hold(16)" "SELECT sift._test_maint_workers_hold(0)" "SELECT sift._test_worker_slots(16)" "SELECT sift._test_worker_slots(0)" | tr '\n' ' ')" == "4 0 4 0 "

# 1,500 requests of distinct relations with every worker held: none is dropped for another, and the workers that come claim from a queue that holds more than 512
cat >"$D/queue.sql" <<'SQL'
SET sift.async_maintenance = on;
SELECT sift._test_maint_workers_hold(16) \gset
CREATE TEMP TABLE answers AS SELECT g, sift._test_maint_enqueue((100000 + g)::oid, 3) AS r FROM generate_series(1, 1500) g;
SELECT count(*) FILTER (WHERE r = 'queued') || ' ' || count(*) FILTER (WHERE r <> 'queued') FROM answers;
SELECT count(DISTINCT relation) FROM sift_internal.maint_queue() WHERE queued;
SELECT completed FROM sift._test_maint_stats();
SELECT sift._test_maint_workers_hold(0) \gset
SELECT sift._test_maint_enqueue(100000, 3);
SQL
mapfile -t Q < <(psql -X -At -q -h "$D/sock" -p "$(port mh)" -U postgres -d postgres -f "$D/queue.sql" 2>&1)
check "the queue takes $REQUESTS requests of distinct relations" test "${Q[0]:-}" == "$REQUESTS 0"
check "it lists all of them" test "${Q[1]:-}" == "$REQUESTS"
check "the workers claim and complete from it" wait_for "[[ \$(sql mh 'SELECT completed - ${Q[2]:-0} >= 16 FROM sift._test_maint_stats()') == t ]]" 60
# the requests are for relations that do not exist: the worker that paces through them is ended, the others of the queue are left to the next start
workers() { sql mh "SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'sift maintenance'"; }
sql mh "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE backend_type = 'sift maintenance'" >/dev/null
wait_for "[[ \$(workers) == 0 ]]" 30 || bail "the worker of the queue is still there"

# the cut of the file end under a reader whose statements of 2 ms leave a gap of 1 ms between them
size() { sql mh "SELECT pg_relation_size('t')"; }
# no statement of the reader runs on a snapshot older than the delete of the round: the VACUUM may drop the parts
settled() { [[ $(sql mh "SELECT count(*) FROM pg_stat_activity WHERE backend_xmin IS NOT NULL AND backend_xmin::text::bigint <= $1") == 0 ]]; }
sql mh "CREATE TABLE t (id int, pad text) USING sift_table WITH (autovacuum_enabled = off)" \
	"INSERT INTO t SELECT g, md5(g::text) || md5((g * 7)::text) FROM generate_series(1, 20000) g" "VACUUM t" >/dev/null || bail "table setup failed"
LIMIT=$(( $(size) + 262144 ))
printf '%s\n' "SELECT count(*) FROM t, pg_sleep(0.002);" "\\sleep 1ms" >"$D/read.sql"
pgbench_bg mh 1 120 -f "$D/read.sql"
cut=0
for r in $(seq "$ROUNDS"); do
	sql mh "INSERT INTO t SELECT g + $r * 1000000, md5(g::text) || md5((g * 7)::text) FROM generate_series(1, 20000) g" "DELETE FROM t WHERE id > 1000000" >/dev/null || bail "round $r failed"
	xid=$(sql mh "SELECT pg_current_xact_id()::xid")
	wait_for "settled $xid" 30 || bail "the reader holds a snapshot of round $r"
	sql mh "SET sift.async_maintenance = off" "VACUUM t" >/dev/null || bail "VACUUM of round $r failed"
	[[ $(size) -le $LIMIT ]] && cut=$((cut + 1))
done
kill "$PGBENCH_PID" 2>/dev/null
wait "$PGBENCH_PID" 2>/dev/null
check "VACUUM cuts the file end in $ROUNDS of $ROUNDS rounds under a reader" test "$cut" == "$ROUNDS"
check "the reader never failed" test "$(grep -c -E "ERROR|failed" "$D/pgbench.log")" == 0
check "the table is intact and holds no lock of the cut" test "$(sql mh "SELECT count(*) FROM t" "SELECT count(*) FROM pg_locks WHERE relation = 't'::regclass AND mode = 'AccessExclusiveLock'" | tr '\n' ' ')" == "20000 0 "

finish
