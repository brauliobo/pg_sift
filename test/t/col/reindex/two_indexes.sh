#!/usr/bin/env bash
# REINDEX of two sift_col indexes of one table at the same time. PostgreSQL holds ShareLock of the table for a REINDEX; the build used to drop and make the two change-tracking
# triggers of its log again, which takes a stronger lock of the table while the other REINDEX holds its ShareLock, and one of them ended with "deadlock detected" (8 of 16 runs). A
# REINDEX keeps the triggers that are what the option made (their catalog rows are the log's witnesses: the new log names them), so it takes no lock stronger than its own.
# Here: a REINDEX that stays open does not hold up the REINDEX of the other index; rounds of two REINDEX at once, of two REINDEX CONCURRENTLY and of one of each, under writers, end
# without a deadlock of the extension's (the cycle that PostgreSQL has beside a REINDEX CONCURRENTLY is its own); the reads of the tracked indexes are the heap's answer and the log is complete after every round; a REINDEX of an index whose triggers were disabled or dropped
# makes them again.
. "$(dirname "$0")/lib.bash"
plan 8

ROUNDS=$(knob SIFT_RL_ROUNDS 30 12)
ROWS=$(knob SIFT_RL_ROWS 20000 8000)
node_init n 62 "autovacuum = off
sift.async_maintenance = off
deadlock_timeout = 200ms"
node_start n || bail "start failed"
D=$(dir n)
sql n "CREATE EXTENSION pg_sift" \
	"CREATE TABLE h (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false)" \
	"INSERT INTO h SELECT i, i % 10, i FROM generate_series(1, $ROWS) i" \
	"CREATE INDEX h_a ON h USING sift_col (id, g, v)" "CREATE INDEX h_b ON h USING sift_col (id, v, g)" "VACUUM (FREEZE, ANALYZE) h" >/dev/null || bail "setup failed"
cat >"$D/w.sql" <<EOF
\set id random(1, $((ROWS * 2)))
BEGIN;
UPDATE h SET v = v + 1 WHERE id = :id;
DELETE FROM h WHERE id = :id + 1;
INSERT INTO h VALUES (:id + 1, (:id + 1) % 10, :id) ON CONFLICT (id) DO UPDATE SET v = h.v + 1;
COMMIT;
EOF
Q="SELECT string_agg(g || ':' || n || ':' || s, ' ' ORDER BY g) FROM (SELECT g, count(*) n, sum(v) s FROM h GROUP BY g) q"
ans() { sql n "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = $([[ $1 == heap ]] && echo off || echo on)" "SET sift.test_no_heapfree = $([[ $1 == checked ]] && echo on || echo off)" "$Q" | tail -n 1; }
differential() { [[ $(ans free) == $(ans heap) && $(ans checked) == $(ans heap) ]]; }
complete() { [[ $(sql n "SELECT complete AND triggers FROM sift.col_tracking('h_a')") == t && $(sql n "SELECT complete AND triggers FROM sift.col_tracking('h_b')") == t ]]; }
ntrig() { sql n "SELECT count(*) FROM pg_trigger WHERE tgrelid = 'h'::regclass AND tgname LIKE 'sift_track_%'"; }
deadlocks() { grep -c 'deadlock detected' "$D/postgres.log" || true; }

# 1. a REINDEX that stays open holds ShareLock of the table: the REINDEX of the other index goes through it
rm -f "$D/held" "$D/release"
{ echo "BEGIN;"; echo "REINDEX INDEX h_a;"; echo "\\! touch '$D/held'"; echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"; echo "COMMIT;"; } | sql n >"$D/hold.out" 2>&1 &
hp=$!
wait_for "[[ -e $D/held ]]" 120 || bail "the open REINDEX did not start"
sql n "REINDEX INDEX h_b" >"$D/b.out" 2>&1 &
bp=$!
wait_for "! kill -0 $bp 2>/dev/null" 60; ended=$?
touch "$D/release"
wait "$bp"; brc=$?
wait "$hp"; hrc=$?
check "a REINDEX of h_b ends while the REINDEX of h_a is open (no lock above ShareLock)" test "$ended" == 0 -a "$brc" == 0 -a "$hrc" == 0
check "... and the log of both is complete with their triggers" complete

# 2. rounds of two statements at once, under 4 writers. Two plain REINDEX never fail. A REINDEX CONCURRENTLY has a deadlock of PostgreSQL's own with a statement on the table that took its snapshot
# and waits for the table lock, since it waits for older snapshots while it holds ShareUpdateExclusiveLock (plain btrees do it in 20 of 30 rounds beside a plain REINDEX, and beside another
# REINDEX CONCURRENTLY nearly every round): a round of those may end with one statement failed. What must hold in all of them is that no deadlock comes from a lock that the extension takes
# (AccessExclusiveLock or ShareRowExclusiveLock of the table for the triggers, AccessShareLock of the other index for the rules of a build).
own_deadlocks() { grep -c 'waits for \(AccessExclusive\|ShareRowExclusive\|AccessShare\)Lock' "$D/postgres.log" || true; }
rounds() {
	local r i fails=0
	pgbench_bg n 4 "$((ROUNDS * 6))" -f "$D/w.sql"
	for ((r = 0; r < ROUNDS; r++)); do
		sql n "$1 h_a" >/dev/null 2>&1 & i=$!
		sql n "$2 h_b" >/dev/null 2>&1 || fails=$((fails + 1))
		wait "$i" || fails=$((fails + 1))
	done
	kill "$PGBENCH_PID" 2>/dev/null; wait "$PGBENCH_PID" 2>/dev/null
	ROUND_FAILS=$fails
	# a REINDEX CONCURRENTLY that failed leaves an invalid copy (without triggers)
	sql n "SELECT format('DROP INDEX %s', indexrelid::regclass) FROM pg_index WHERE NOT indisvalid AND indrelid = 'h'::regclass" | while read -r drop; do sql n "$drop" >/dev/null; done
}
rounds "REINDEX INDEX" "REINDEX INDEX"
check "$ROUNDS rounds of two REINDEX at once under writers: no failed statement, no deadlock" test "$ROUND_FAILS|$(deadlocks)" == "0|0"
rounds "REINDEX INDEX CONCURRENTLY" "REINDEX INDEX CONCURRENTLY"
check "$ROUNDS rounds of two REINDEX CONCURRENTLY under writers: no deadlock of a lock the extension takes" test "$(own_deadlocks)" == 0
rounds "REINDEX INDEX" "REINDEX INDEX CONCURRENTLY"
check "$ROUNDS rounds of a REINDEX and a REINDEX CONCURRENTLY under writers: no deadlock of a lock the extension takes" test "$(own_deadlocks)" == 0
sql n "REINDEX INDEX h_a" "REINDEX INDEX h_b" >/dev/null
check "the heap-free read, the checked read and the heap give one answer after the rounds" differential
intact() { [[ $(ntrig) == 4 ]] && complete; }
check "... both indexes are complete with the two triggers each" intact

# 3. triggers that are not what the option made: the REINDEX makes them again
sql n "ALTER TABLE h DISABLE TRIGGER ALL" "REINDEX INDEX h_a" >/dev/null
check "a REINDEX of an index whose triggers were disabled makes them again, complete" test "$(sql n "SELECT complete AND triggers FROM sift.col_tracking('h_a')")" == t
sql n "ALTER TABLE h ENABLE TRIGGER ALL" >/dev/null
finish
