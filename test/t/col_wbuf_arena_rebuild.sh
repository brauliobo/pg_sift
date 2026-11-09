#!/usr/bin/env bash
# sift.col_wbuf_ref: the values of the buffered rows are in the arena, in the shared memory of the server that inserted them. A server that starts again (a crash, kill -9),
# a standby (it replays the records, nobody fills its arena) and a standby that was promoted have none: the first reader builds the entries from the heap, one reader at a time, and every
# reader after it takes them from the arena. The rows are a mix of what the heap does to tuples: committed, rolled back, savepoint rolled back, updated (HOT and not), deleted and pruned
# without the index cleaned, frozen. At each step the answers of the engine are the heap's.
. "$(dirname "$0")/lib.bash"

ROWS=$(knob SIFT_ARENA_ROWS 6000 2000)
plan 16
node_init primary 5 "wal_level = replica
max_wal_senders = 4
wal_keep_size = 256MB
autovacuum = off
sift.async_maintenance = off
sift.col_auto_merge = off
sift.col_wbuf_ref = on
max_parallel_workers_per_gather = 0"
node_start primary || bail "primary start failed"
D=$(dir primary)

ANSWERS="SELECT md5(concat_ws('|',
    (SELECT count(*) || ',' || sum(v) FROM a),
    (SELECT count(*) || ',' || sum(v) FROM a WHERE g = 3),
    (SELECT string_agg(id::text, ',' ORDER BY v DESC, id) FROM (SELECT id, v FROM a ORDER BY v DESC, id LIMIT 30) x),
    (SELECT string_agg(x::text, ',' ORDER BY x::text) FROM (SELECT g, count(*), sum(v) FROM a GROUP BY g) x)))"
sql primary "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE a (id int PRIMARY KEY, g int NOT NULL, v int, pad text) WITH (autovacuum_enabled = off)" \
	"CREATE INDEX a_ix ON a USING sift_col (id, g, v)" \
	"CREATE FUNCTION answers() RETURNS text LANGUAGE sql AS \$\$ $ANSWERS \$\$" \
	"CREATE FUNCTION same_as_heap() RETURNS boolean LANGUAGE plpgsql AS \$\$ DECLARE n text; s text; BEGIN
		PERFORM set_config('sift.enable_agg', 'off', true); n := answers(); PERFORM set_config('sift.enable_agg', 'on', true); s := answers(); RETURN n = s; END \$\$" >/dev/null || bail "setup failed"

# the rows: nothing of them is promoted, they stay in the buffer
sql primary "INSERT INTO a SELECT i, i % 10, i, 'x' FROM generate_series(1, $ROWS) i" \
	"BEGIN" "INSERT INTO a SELECT 100000 + i, i % 10, 50000 + i, 'r' FROM generate_series(1, 500) i" "ROLLBACK" \
	"BEGIN" "INSERT INTO a SELECT 200000 + i, i % 10, i, 'c' FROM generate_series(1, 100) i" "SAVEPOINT s" \
	"INSERT INTO a SELECT 300000 + i, i % 10, 70000 + i, 's' FROM generate_series(1, 300) i" "ROLLBACK TO s" "COMMIT" \
	"UPDATE a SET pad = 'hot' WHERE id % 5 = 0" "UPDATE a SET v = v + 1 WHERE id % 7 = 0" "DELETE FROM a WHERE id % 11 = 0" \
	"VACUUM (INDEX_CLEANUP off) a" "VACUUM (FREEZE, INDEX_CLEANUP off) a" \
	"INSERT INTO a SELECT 400000 + i, i % 10, i, 'y' FROM generate_series(1, 500) i" >/dev/null || bail "workload failed"
BUFFERED=$(sql primary "SELECT rows FROM sift.col_segments('a_ix') WHERE kind = 'buffer'")
stats() { sql "$1" "SELECT rebuilt FROM sift._test_arena_stats()"; }
same() { [[ $(sql "$1" "SELECT same_as_heap()") == t ]]; }

check "the primary: the buffer holds $BUFFERED rows and the engine gives the heap's answers" same primary
check "the primary: no row was built from the heap" test "$(stats primary)" == 0

# kill -9 of everything: the arena is empty after recovery
crash primary
node_start primary || bail "restart after the crash failed"
check "after the crash: the first read gives the heap's answers" same primary
R1=$(stats primary)
check "after the crash: the first reader built the entries from the heap ($R1 rows of the $BUFFERED records)" test "$R1" -gt 0 -a "$R1" -le "$((BUFFERED + 1000))"
same primary; same primary
check "after the crash: the readers after the first built nothing" test "$(stats primary)" == "$R1"
check "after the crash: sift_cwbuf_verify and the store check pass" test "$(sql primary "SELECT sift.col_verify('a_ix', true) IS NOT NULL")" == t

# four readers that start together on an empty arena: one builds the entries, the others wait for it and read them
crash primary
node_start primary || bail "second restart failed"
for i in 1 2 3 4; do (sql primary "SELECT same_as_heap()" >"$D/reader$i.out" 2>&1 &) ; done
wait_for "[[ \$(cat $D/reader?.out 2>/dev/null | grep -c .) == 4 ]]" 120
check "four first readers together: every one has the heap's answers" test "$(cat "$D"/reader?.out | sort | uniq -c | tr -s ' ')" == " 4 t"
R2=$(stats primary)
check "four first readers together: the entries were built once (not once for each reader)" test "$R2" -gt 0 -a "$R2" -le "$((BUFFERED + 1000))"

# new rows after the restart go to the arena again, next to the rows without entries
sql primary "INSERT INTO a SELECT 500000 + i, i % 10, i, 'z' FROM generate_series(1, 500) i" >/dev/null
check "rows inserted after the restart: the answers are the heap's" same primary
check "rows inserted after the restart: the heap was not read for them" test "$(stats primary)" == "$R2"

# a standby replays the records: its arena is empty
sql primary "CHECKPOINT" >/dev/null
node_clone primary standby 6 "hot_standby_feedback = on
wal_receiver_status_interval = 1s"
node_start standby || bail "standby start failed"
catchup primary standby || bail "the standby did not catch up"
check "the standby: the first read gives the heap's answers" same standby
S1=$(stats standby)
check "the standby: the first reader built the entries from the heap ($S1 rows)" test "$S1" -gt 0
same standby
check "the standby: the second read takes them from the arena" test "$(stats standby)" == "$S1"
sql primary "INSERT INTO a SELECT 600000 + i, i % 10, i, 'w' FROM generate_series(1, 300) i" "UPDATE a SET v = v + 1 WHERE id % 13 = 0" >/dev/null
catchup primary standby || bail "the standby did not catch up again"
check "the standby: rows replayed after the first read are in the answers" same standby

# the standby is promoted: its arena has the rows it built, and the new primary writes the next ones into it
node_promote standby
wait_for "[[ \$(sql standby 'SELECT pg_is_in_recovery()') == f ]]" 60
sql standby "INSERT INTO a SELECT 700000 + i, i % 10, i, 'p' FROM generate_series(1, 300) i" >/dev/null
check "the promoted standby: the answers are the heap's" same standby
sql standby "SELECT sift.col_promote('a_ix') >= 0" >/dev/null
check "the promoted standby: promotion of the buffer keeps them" same standby
finish
