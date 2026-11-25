#!/usr/bin/env bash
# tap: pooled
# Deletion records (core/v1_col/tomb/dv.h) under concurrent work: one session deletes scattered rows in statements large enough to write records and inserts as many, scanners compare the index with the
# heap inside one snapshot (a REPEATABLE READ transaction asks both), and the maintenance loop runs VACUUM, the fold of the log, the promotion of the write buffer and the forced merge, which carries the
# records of the segments it replaces to marks under the tombstone lock. No scanner may see a row the heap does not, and no statement may fail.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 5
SECS=$(knob SIFT_DVCONC_SECS 40 12)
node_init dc 14 "autovacuum = off
max_parallel_workers_per_gather = 0"
node_start dc || bail "start failed"
D=$(dir dc)
sql dc "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE ev (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = off)" \
	"INSERT INTO ev SELECT i, i % 10, i FROM generate_series(1, 40000) i" \
	"CREATE INDEX ev_c ON ev USING sift_col (id, g, v) WITH (track_changes = 'id')" \
	"VACUUM (FREEZE, ANALYZE) ev" \
	"CREATE FUNCTION scan_ok() RETURNS bool LANGUAGE plpgsql AS \$f\$
	DECLARE a text; o text; q text := 'SELECT g, count(*), sum(v) FROM ev GROUP BY g ORDER BY g';
	BEGIN
		PERFORM set_config('sift.enable_agg', 'on', true);
		EXECUTE 'SELECT string_agg(r::text, '';'') FROM (' || q || ') r' INTO a;
		PERFORM set_config('sift.enable_agg', 'off', true);
		EXECUTE 'SELECT string_agg(r::text, '';'') FROM (' || q || ') r' INTO o;
		RETURN a = o;
	END \$f\$" >/dev/null || bail "setup failed"

psqlq() { psql -X -At -q -h "$(node_sock dc)" -p "$(port dc)" -U postgres -d "$(node_db dc)" "$@"; }
END=$((SECONDS + SECS))
running() { ((SECONDS < END)); }

deleter() {
	local i=0
	while running; do
		i=$((i + 1))
		psqlq -c "WITH d AS (DELETE FROM ev WHERE id % 7 = $((i % 7)) AND id BETWEEN $((i * 300 % 30000 + 1)) AND $((i * 300 % 30000 + 4000)) RETURNING g, v) INSERT INTO ev SELECT 1000000 + $i * 10000 + row_number() OVER (), g, v FROM d" 2>&1
	done
}
scanner() { while running; do psqlq -c "BEGIN ISOLATION LEVEL REPEATABLE READ" -c "SELECT scan_ok()" -c "COMMIT" 2>&1; done; }
maintainer() {
	while running; do
		psqlq -c "VACUUM ev" -c "SELECT sift._test_col_fold_marks('ev_c')" -c "SELECT sift.col_promote('ev_c')" -c "SELECT sift.col_merge('ev_c', true)" 2>&1
	done
}
deleter >"$D/deleter.out" &
scanner >"$D/scan1.out" &
scanner >"$D/scan2.out" &
maintainer >"$D/maint.out" &
wait

scans=$(cat "$D"/scan?.out | grep -c '^t$' || true)
check "the scanners ran ($scans checks)" test "$scans" -gt 20
check "no scanner saw an answer other than the heap's" test "$(cat "$D"/scan?.out | grep -c '^f$' || true)" == 0
check "no statement of the deleter, the scanners and the maintenance failed" test "$(cat "$D"/*.out | grep -c 'ERROR' || true)" == 0
check "the index verifies against the heap" test "$(sql dc "SELECT sift.col_verify('ev_c', true) IS NOT NULL")" == t
check "after a last fold and VACUUM the answers are the heap's" test "$(sql dc "VACUUM ev" "SELECT scan_ok()" | tail -1)" == t
finish
