#!/usr/bin/env bash
# REINDEX CONCURRENTLY and CREATE INDEX CONCURRENTLY of a sift_col index that tracks its changes, under writers (an update, a delete and an insert-on-conflict in each transaction).
# The copy of REINDEX CONCURRENTLY has no triggers of its own: the triggers of the old index move to it at the swap, so dropping the old index drops nothing that is on the table and
# needs no ACCESS EXCLUSIVE lock on it. With triggers of its own the swap dropped the table's triggers with the old index: the writers queued behind that lock, deadlocked with the
# index swap (1.4% of the transactions failed in this test) and a loop of reindexes did 4 in 40 s against 650.
# A reindex cancelled in the middle leaves an invalid copy that owns no trigger; the dump of the result restores to the same answers.
. "$(dirname "$0")/lib.bash"
plan 12

SECS=$(knob SIFT_RC_SECS 12 6)
ROWS=$(knob SIFT_RC_ROWS 20000 8000)
node_init n 40 "autovacuum = off
sift.col_auto_merge = off"
node_start n || bail "start failed"
D=$(dir n)
sql n "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE h (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL, pad text NOT NULL) WITH (autovacuum_enabled = false, fillfactor = 70)" \
	"INSERT INTO h SELECT i, i % 10, i, repeat('x', 60) FROM generate_series(1, $ROWS) i" "CREATE INDEX h_x ON h USING sift_col (id, g, v)" "VACUUM (FREEZE, ANALYZE) h" >/dev/null || bail "setup failed"
cat >"$D/w.sql" <<EOF
\set id random(1, $((ROWS * 2)))
BEGIN;
UPDATE h SET v = v + 1 WHERE id = :id;
DELETE FROM h WHERE id = :id + 1;
INSERT INTO h VALUES (:id + 1, (:id + 1) % 10, :id, 'y') ON CONFLICT (id) DO UPDATE SET v = h.v + 1;
COMMIT;
EOF
Q="SELECT string_agg(g || ':' || n || ':' || s, ' ' ORDER BY g) FROM (SELECT g, count(*) n, sum(v) s FROM h GROUP BY g) q"
# ans MODE: free (the heap-free read), checked (the heap checked for the rows that need it), heap (the normal plan)
ans() { sql n "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = $([[ $1 == heap ]] && echo off || echo on)" "SET sift.test_no_heapfree = $([[ $1 == checked ]] && echo on || echo off)" "$Q" | tail -n 1; }
differential() { [[ $(ans free) == $(ans heap) && $(ans checked) == $(ans heap) ]]; }
ntrig() { sql n "SELECT count(*) FROM pg_trigger WHERE tgrelid = 'h'::regclass AND tgname LIKE 'sift_track_%'"; }
deadlocks() { grep -c 'deadlock detected' "$D/postgres.log"; }
# loop CMD...: runs the statements one after the other for SECS seconds, the number of rounds that succeeded in $D/rounds
loop() {
	local end=$((SECONDS + SECS)) n=0
	while ((SECONDS < end)); do "$@" >/dev/null 2>&1 && n=$((n + 1)); done
	echo "$n" >"$D/rounds"
}
failed() { sed -n 's/^number of failed transactions: \([0-9]*\).*/\1/p' "$D/pgbench.log" | head -n 1; }

# 1. REINDEX CONCURRENTLY in a loop, under 8 writers
d0=$(deadlocks)
pgbench_bg n 8 "$((SECS + 2))" -f "$D/w.sql"
loop sql n "REINDEX INDEX CONCURRENTLY h_x"
wait "$PGBENCH_PID"
check "REINDEX CONCURRENTLY under 8 writers does at least 5 rounds in ${SECS} s ($(cat "$D/rounds") done)" test "$(cat "$D/rounds")" -ge 5
check "... no transaction of the writers failed" test "$(failed)" == 0
check "... no deadlock was detected" test "$(($(deadlocks) - d0))" -eq 0
check "... the table has the two triggers of the index, which depend on it, and no copy is left" \
	test "$(ntrig)|$(sql n "SELECT count(*) FROM pg_trigger t JOIN pg_depend d ON d.classid = 'pg_trigger'::regclass AND d.objid = t.oid AND d.refobjid = 'h_x'::regclass WHERE t.tgrelid = 'h'::regclass")|$(sql n "SELECT count(*) FROM pg_class WHERE relname LIKE 'h_x_ccnew%'")" == "2|2|0"
check "... the heap-free read, the checked read and the heap give one answer" differential

# 2. the lock a REINDEX CONCURRENTLY asks of the table, sampled while writers run: ShareUpdateExclusiveLock, never ACCESS EXCLUSIVE
pgbench_bg n 8 "$((SECS / 2 + 3))" -f "$D/w.sql"
( for _ in 1 2 3 4 5 6; do sql n "REINDEX INDEX CONCURRENTLY h_x" >/dev/null 2>&1; done ) &
RP=$!
ae=0
while kill -0 "$RP" 2>/dev/null; do
	ae=$((ae + $(sql n "SELECT count(*) FROM pg_locks l JOIN pg_stat_activity a USING (pid) WHERE l.relation = 'h'::regclass AND l.mode = 'AccessExclusiveLock' AND a.query LIKE 'REINDEX%'")))
	sleep 0.02
done
wait "$PGBENCH_PID"
check "REINDEX CONCURRENTLY holds or waits for no ACCESS EXCLUSIVE lock on the table (sampled every 20 ms)" test "$ae" -eq 0

# 3. CREATE INDEX CONCURRENTLY (and a plain DROP INDEX) under the writers
d0=$(deadlocks)
pgbench_bg n 8 "$((SECS + 2))" -f "$D/w.sql"
loop sql n "DROP INDEX IF EXISTS h_y" "CREATE INDEX CONCURRENTLY h_y ON h USING sift_col (id, g, v)"
wait "$PGBENCH_PID"
check "CREATE INDEX CONCURRENTLY under 8 writers: rounds done ($(cat "$D/rounds")), no failed transaction, no deadlock" test "$(cat "$D/rounds")" -ge 5 -a "$(failed)" == 0 -a "$(($(deadlocks) - d0))" -eq 0
sql n "DROP INDEX IF EXISTS h_y" "CREATE INDEX h_y ON h USING sift_col (id, g, v)" >/dev/null
check "two tracked indexes have four triggers; dropping one leaves the two of the other" test "$(ntrig)" == 4 -a "$(sql n "DROP INDEX h_y" "SELECT count(*) FROM pg_trigger WHERE tgrelid = 'h'::regclass AND tgname LIKE 'sift_track_%'" | tail -n 1)" == 2

# 4. cancelled in the middle, under the writers: the invalid copies own no trigger
pgbench_bg n 8 "$((SECS + 4))" -f "$D/w.sql"
for i in $(seq 1 $((SECS * 4))); do
	sql n "SET statement_timeout = $((RANDOM % 40 + 1))" "REINDEX INDEX CONCURRENTLY h_x" >/dev/null 2>&1
done
wait "$PGBENCH_PID"
invalid=$(sql n "SELECT count(*) FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid WHERE i.indrelid = 'h'::regclass AND NOT i.indisvalid")
diag "$invalid invalid index copies after the cancelled reindexes, $(failed) failed transactions"
check "cancelled reindexes leave invalid copies that own no trigger, and the two triggers of the index" \
	test "$(sql n "SELECT count(*) FROM pg_trigger t JOIN pg_depend d ON d.classid = 'pg_trigger'::regclass AND d.objid = t.oid JOIN pg_index i ON i.indexrelid = d.refobjid WHERE NOT i.indisvalid")" == 0 -a "$(ntrig)" == 2
check "... and the reads still answer as the heap" differential
for ix in $(sql n "SELECT c.relname FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid WHERE i.indrelid = 'h'::regclass AND NOT i.indisvalid"); do
	sql n "DROP INDEX CONCURRENTLY \"$ix\"" >/dev/null 2>&1 || sql n "DROP INDEX \"$ix\"" >/dev/null 2>&1
done
clean_then_reindex() { [[ $(sql n "SELECT count(*) FROM pg_index WHERE indrelid = 'h'::regclass AND NOT indisvalid") == 0 ]] && sql n "REINDEX INDEX CONCURRENTLY h_x" >/dev/null 2>&1; }
check "the invalid copies are dropped, and a REINDEX CONCURRENTLY after them runs" clean_then_reindex

# 5. the dump of the result
sql n "VACUUM h" >/dev/null
pg_dump -h "$D/sock" -p "$(port n)" -U postgres -d postgres >"$D/dump.sql" 2>"$D/dump.err" || diag "$(cat "$D/dump.err")"
sql n "CREATE DATABASE restored" >/dev/null
psql -X -q -h "$D/sock" -p "$(port n)" -U postgres -d restored -v ON_ERROR_STOP=1 -f "$D/dump.sql" >/dev/null 2>"$D/restore.err" || diag "$(head -n 5 "$D/restore.err")"
rans() { psql -X -At -q -h "$D/sock" -p "$(port n)" -U postgres -d restored -c "SET max_parallel_workers_per_gather = 0" -c "SET sift.enable_agg = $1" -c "$Q" | tail -n 1; }
check "the dump restores: the index is there with its two triggers, and answers as the heap" \
	test "$(psql -X -At -q -h "$D/sock" -p "$(port n)" -U postgres -d restored -c "SELECT count(*) FROM pg_trigger WHERE tgrelid = 'h'::regclass AND tgname LIKE 'sift_track_%'")" == 2 -a "$(rans on)" == "$(rans off)"
finish
