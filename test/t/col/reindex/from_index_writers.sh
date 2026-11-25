#!/usr/bin/env bash
# tap: pooled
# (each client works on ids of its own and no insert is speculative: the records of the write buffer name their inserters, which a build from the old index needs for the stamps of the log)
# REINDEX of a sift_col index that tracks its changes takes the rows from the index it replaces (col/v1/build/from_index.c) while writers commit and roll back around it: the writers
# wait for its ShareLock between statements, so every round finds the index with the rows of the writers before it in segments and the write buffer, and the table's dead versions
# no VACUUM has taken. After every round the heap-free read, the checked read and the heap give one answer and the index passes its check. REINDEX CONCURRENTLY is built from the heap
# (its writers change the index it would read), and so are the answers it gives. The rounds force the source (sift.test_col_from_index_forced): the entries of the dead versions no VACUUM
# has taken stay in the index and the heap drops them, so after enough writes the build prices the heap read below the old index (part 4), whatever the speed of the writers.
. "${SIFT_REPO:?}/test/t/lib/common.bash"
plan 9

SECS=$(knob SIFT_RIW_SECS 8 4)
ROWS=$(knob SIFT_RIW_ROWS 20000 8000)
node_init n 40 "sift.col_build_source = auto
autovacuum = off
sift.async_maintenance = off"
node_start n || bail "start failed"
D=$(dir n)
sql n "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE h (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL, pad text NOT NULL) WITH (autovacuum_enabled = false, fillfactor = 70)" \
	"INSERT INTO h SELECT i, i % 10, i, repeat('x', 60) FROM generate_series(1, $ROWS) i" "CREATE INDEX h_x ON h USING sift_col (id, g, v) WITH (key_cut = 'id')" \
	"VACUUM (FREEZE, ANALYZE) h" >/dev/null || bail "setup failed"
cat >"$D/w.sql" <<EOF
\set id random(0, $((ROWS / 8 - 2))) * 8 + :client_id + 1
\set rb random(1, 4)
BEGIN;
UPDATE h SET v = v + 1 WHERE id = :id;
DELETE FROM h WHERE id = :id + 8;
INSERT INTO h VALUES (:id + 8, (:id + 8) % 10, :id, 'y');
\if :rb = 1
ROLLBACK;
\else
COMMIT;
\endif
EOF
Q="SELECT string_agg(g || ':' || n || ':' || s, ' ' ORDER BY g) FROM (SELECT g, count(*) n, sum(v) s FROM h GROUP BY g) q"
# ans MODE: free (the heap-free read), checked (the heap checked for the rows that need it), heap (the normal plan)
ans() { sql n "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = $([[ $1 == heap ]] && echo off || echo on)" "SET sift.test_no_heapfree = $([[ $1 == checked ]] && echo on || echo off)" "$Q" | tail -n 1; }
differential() { [[ $(ans free) == $(ans heap) && $(ans checked) == $(ans heap) ]]; }
verified() { [[ $(sql n "SELECT sift.col_verify('h_x', true)::text") == "" ]]; }
failed() { sed -n 's/^number of failed transactions: \([0-9]*\).*/\1/p' "$D/pgbench.log" | head -n 1; }
# rounds STATEMENT: the statement one after the other for SECS seconds under 8 writers; the rounds in $D/rounds, those that took their rows from the replaced index in $D/used,
# and whether the index passed its check after each round, the writers stopped, in $D/clean
rounds() {
	local end n=0 u=0 c=0
	pgbench_bg n 8 "$((SECS + 2))" -f "$D/w.sql"
	end=$((SECONDS + SECS))
	while ((SECONDS < end)); do
		[[ $(sql n "SET sift.async_maintenance = off" "SET sift.test_col_from_index_forced = on" "$1" "SHOW sift.test_col_from_index_used" 2>/dev/null | tail -n 1) == 1 ]] && u=$((u + 1))
		n=$((n + 1))
	done
	wait "$PGBENCH_PID"
	echo "$n" >"$D/rounds"
	echo "$u" >"$D/used"
}

# 1. REINDEX in a loop under writers that commit and roll back
rounds "REINDEX INDEX h_x"
diag "$(cat "$D/rounds") rounds, $(cat "$D/used") from the replaced index, $(failed) failed transactions"
check "REINDEX under 8 writers: every round took the rows of the replaced index, no transaction failed" test "$(cat "$D/rounds")" -ge 3 -a "$(cat "$D/used")" == "$(cat "$D/rounds")" -a "$(failed)" == 0
check "... the heap-free read, the checked read and the heap give one answer" differential
check "... and the index passes its check" verified

# 2. the rounds again, each REINDEX followed by writers that roll back and commit before the next, differential after every one
ok=1
for _ in $(seq 1 6); do
	pgbench -n -h "$(node_sock n)" -p "$(port n)" -U postgres -c 8 -j 4 -t 200 -f "$D/w.sql" "$(node_db n)" >/dev/null 2>&1
	sql n "SET sift.async_maintenance = off" "REINDEX INDEX h_x" >/dev/null 2>&1
	differential && verified || ok=0
done
check "six REINDEX, each after a burst of writers: the three reads agree and the index passes its check after each" test "$ok" == 1

# 3. REINDEX CONCURRENTLY is built from the heap, under the writers
rounds "REINDEX INDEX CONCURRENTLY h_x"
diag "$(cat "$D/rounds") rounds, $(cat "$D/used") from the replaced index, $(failed) failed transactions"
check "REINDEX CONCURRENTLY under 8 writers: every round read the heap, no transaction failed" test "$(cat "$D/rounds")" -ge 3 -a "$(cat "$D/used")" == 0 -a "$(failed)" == 0
check "... the three reads agree" differential
check "... and the index passes its check" verified
# 4. the dead entries of the replaced index grow with every write and the heap read does not: the build prices the heap below it, and the forced source still takes the old index
used() { sql n "SET sift.async_maintenance = off" "SET sift.test_col_from_index_forced = $1" "REINDEX INDEX h_x" "SHOW sift.test_col_from_index_used" | tail -n 1; }
priced_out=0
for _ in $(seq 1 12); do
	sql n "UPDATE h SET v = v + 1" >/dev/null
	[[ $(used off) == 0 ]] && { priced_out=1; break; }
done
check "REINDEX after enough updates with no VACUUM: the heap read is priced below the old index and is taken" test "$priced_out" == 1
sql n "UPDATE h SET v = v + 1" >/dev/null
check "... and the forced source takes the old index all the same" test "$(used on)" == 1
finish
