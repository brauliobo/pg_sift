#!/usr/bin/env bash
# RETURNING OLD / NEW of a sift_table (PostgreSQL 18) for the row another session changed while this one waited for it: the statement that waited re-reads the row the other session
# committed, and its OLD is that row, its NEW what it wrote, for an UPDATE (a column delta), a MERGE, a DELETE and an INSERT ... ON CONFLICT DO UPDATE. A heap twin gives the expected values.
. "$(dirname "$0")/lib.bash"

node_init ret 3 "shared_preload_libraries = 'pg_sift'
sift.test_create_format1 = off
statement_timeout = '3s'  # timeout-ok: a lost upsert race hangs a statement for ever, the 3 s cap fails it so the check names it
autovacuum = off"
node_start ret || bail "start failed"
if (( $(sql ret "SHOW server_version_num") < 180000 )); then
	plan 1
	skip 1 "RETURNING OLD needs PostgreSQL 18"
	finish
	exit 0
fi
plan 4
sql ret "CREATE EXTENSION pg_sift" \
	"CREATE TABLE ro_t (id int PRIMARY KEY, c1 int, c2 int, c3 int, c4 int, v int, note text) USING sift_table" "CREATE TABLE ro_h (LIKE ro_t INCLUDING ALL)" \
	"INSERT INTO ro_t SELECT g, g, g, g, g, g * 10, 'n' || g FROM generate_series(1, 500) g" "INSERT INTO ro_h SELECT * FROM ro_t" "VACUUM ro_t" >/dev/null || bail "setup failed"

# waits TABLE ID STATEMENT: another session updates row ID and holds it for a second; STATEMENT (with @T) waits for it and prints what it returned
waits() {
	local t=$1 id=$2 stmt=${3//@T/$1}
	sql ret "BEGIN" "UPDATE $t SET v = v + 1 WHERE id = $id" "SELECT pg_sleep(1.5)" "COMMIT" >/dev/null &
	local holder=$!
	wait_for "[[ \$(sql ret \"SELECT count(*) FROM pg_locks WHERE locktype = 'transactionid' AND granted\") -ge 1 ]]" 10
	sql ret "$stmt"
	wait "$holder"
}

for case in \
	"update|UPDATE @T SET v = v + 100 WHERE id = 90 RETURNING old.v, new.v" \
	"merge|MERGE INTO @T t USING (SELECT 91 AS id) s ON t.id = s.id WHEN MATCHED THEN UPDATE SET v = t.v + 100 RETURNING merge_action(), old.v, new.v" \
	"delete|DELETE FROM @T WHERE id = 92 RETURNING old.v, old.note" \
	"upsert|INSERT INTO @T (id, v) VALUES (93, 5) ON CONFLICT (id) DO UPDATE SET v = @T.v + 100 RETURNING old.v, new.v"; do
	name=${case%%|*}
	stmt=${case#*|}
	id=$(grep -o 'id = [0-9]*\|(id, v) VALUES ([0-9]*\|SELECT [0-9]* AS id' <<<"$stmt" | grep -o '[0-9]*' | head -n 1)
	sift_out=$(waits ro_t "$id" "$stmt")
	heap_out=$(waits ro_h "$id" "$stmt")
	out="sift=[$sift_out] heap=[$heap_out]"
	check "$name: the row another session changed is OLD, what this one wrote is NEW ($sift_out)" test "$sift_out" == "$heap_out" -a -n "$sift_out"
done
finish
