#!/usr/bin/env bash
# DROP INDEX CONCURRENTLY of a sift_col index that tracks its changes, beside writers (an update, a delete and an insert-on-conflict in each transaction), and when it fails.
# The index depends on its two triggers and the transaction that deletes it removes them (src/col/track.c), so the drop needs no ACCESS EXCLUSIVE lock before its last step and
# finds no transaction id. The writers beside it must not fail or deadlock (they skip an index the drop has marked dead, whose lock it holds), the copy of a swap must keep the
# triggers of the index that stays, a drop that times out waiting for a writer leaves a valid-looking tracked table (the index invalid, its triggers in place) that drops on the next try,
# and no run leaves a trigger that no index depends on.
. "$(dirname "$0")/lib.bash"
plan 9

SECS=$(knob SIFT_DC_SECS 12 6)
ROWS=$(knob SIFT_DC_ROWS 20000 8000)
node_init n 41 "autovacuum = off
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
ans() { sql n "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = $([[ $1 == heap ]] && echo off || echo on)" "SET sift.test_no_heapfree = $([[ $1 == checked ]] && echo on || echo off)" "$Q" | tail -n 1; }
differential() { [[ $(ans free) == $(ans heap) && $(ans checked) == $(ans heap) ]]; }
ntrig() { sql n "SELECT count(*) FROM pg_trigger WHERE tgrelid = 'h'::regclass AND tgname LIKE 'sift_track_%'"; }
orphans() { sql n "SELECT count(*) FROM pg_trigger g WHERE g.tgname LIKE 'sift_track_%' AND NOT EXISTS (SELECT 1 FROM pg_depend d WHERE d.refclassid = 'pg_trigger'::regclass AND d.refobjid = g.oid AND d.classid = 'pg_class'::regclass)"; }
deadlocks() { grep -c 'deadlock detected' "$D/postgres.log"; }
failed() { sed -n 's/^number of failed transactions: \([0-9]*\).*/\1/p' "$D/pgbench.log" | head -n 1; }

# 1. a concurrent copy and DROP INDEX CONCURRENTLY of it, in a loop, under 8 writers
d0=$(deadlocks)
pgbench_bg n 8 "$((SECS + 2))" -f "$D/w.sql"
end=$((SECONDS + SECS)); rounds=0
while ((SECONDS < end)); do
	sql n "CREATE INDEX CONCURRENTLY h_y ON h USING sift_col (id, g, v)" >/dev/null 2>&1 && sql n "DROP INDEX CONCURRENTLY h_y" >/dev/null 2>&1 && rounds=$((rounds + 1))
done
wait "$PGBENCH_PID"
check "CREATE and DROP INDEX CONCURRENTLY under 8 writers: at least 5 rounds ($rounds done), no failed transaction" test "$rounds" -ge 5 -a "$(failed)" == 0
check "... no deadlock was detected" test "$(($(deadlocks) - d0))" -eq 0
check "... the table keeps the two triggers of h_x, no trigger is left without an index, no copy is left" \
	test "$(ntrig)|$(orphans)|$(sql n "SELECT count(*) FROM pg_class WHERE relname = 'h_y'")" == "2|0|0"
check "... the heap-free read, the checked read and the heap give one answer" differential

# 2. the tracked index itself, under the writers: its drop ends the triggers, the writers go on
d0=$(deadlocks)
pgbench_bg n 8 "$((SECS / 2 + 3))" -f "$D/w.sql"
sleep 1
sql n "DROP INDEX CONCURRENTLY h_x" >/dev/null 2>"$D/drop.err" || diag "$(cat "$D/drop.err")"
wait "$PGBENCH_PID"
check "DROP INDEX CONCURRENTLY of the tracked index under 8 writers: no trigger left, none without an index, no failed transaction, no deadlock" \
	test "$(ntrig)|$(orphans)|$(failed)|$(($(deadlocks) - d0))" == "0|0|0|0"

# 3. a drop that times out while a writer holds the table: the index is left invalid with its triggers, and the next drop removes both
sql n "CREATE INDEX h_y ON h USING sift_col (id, g, v)" >/dev/null
sql n "BEGIN" "LOCK TABLE h IN ROW EXCLUSIVE MODE" "SELECT pg_sleep(5)" "COMMIT" >/dev/null 2>&1 &
HOLD=$!
until [[ $(sql n "SELECT count(*) FROM pg_stat_activity WHERE query LIKE 'SELECT pg_sleep(5)%'") == 1 ]]; do sleep 0.1; done
out=$(sql n "SET lock_timeout = '300ms'" "DROP INDEX CONCURRENTLY h_y" 2>&1)  # timeout-ok: the lock_timeout ends the wait for the writer, the failure checked
check "a DROP INDEX CONCURRENTLY that waits for a writer past lock_timeout fails" grep -q 'lock timeout' <<<"$out"
check "... the index is invalid and keeps its two triggers, which no other index owns" \
	test "$(sql n "SELECT count(*) FROM pg_index WHERE indrelid = 'h'::regclass AND NOT indisvalid")|$(ntrig)|$(orphans)" == "1|2|0"
sql n "UPDATE h SET v = v + 1 WHERE id <= 10" "DELETE FROM h WHERE id = $ROWS" >/dev/null 2>&1
check "... writes on the table still work" test "$?" -eq 0
wait "$HOLD"
sql n "DROP INDEX CONCURRENTLY h_y" >/dev/null 2>&1
check "the next DROP INDEX CONCURRENTLY works: no index, no trigger" test "$(sql n "SELECT count(*) FROM pg_index WHERE indrelid = 'h'::regclass AND NOT indisprimary")|$(ntrig)|$(orphans)" == "0|0|0"
finish
