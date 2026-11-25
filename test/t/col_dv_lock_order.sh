#!/usr/bin/env bash
# A delete that writes deletion records (core/v1_col/tomb/dv.h) holds the tombstone lock shared from before it finds the units of its rows until its records are appended; the merge that replaces
# the segments takes the lock exclusive from the carry of the records to the switch of the directory. Held only around the append, the lock would let a delete find a segment, lose to a whole
# merge, and then append a record for a segment that is gone: its rows would come back. The delete is held after its lookup (sift.test_phase_gate, gate SIFT_COL_GATE_DELETE = 8) with a merge started
# behind it: the merge must wait for the delete, then carry its records, and every answer is the heap's.
. "$(dirname "$0")/lib.bash"

plan 8
G=7400
node_init dl 12 "autovacuum = off"
node_start dl || bail "start failed"
D=$(dir dl)
sql dl "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE ev (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = off)" \
	"INSERT INTO ev SELECT i, i % 10, i FROM generate_series(1, 30000) i" \
	"CREATE INDEX ev_c ON ev USING sift_col (id, g, v) WITH (track_changes = 'id')" \
	"INSERT INTO ev SELECT 30000 + i, i % 10, i FROM generate_series(1, 30000) i" \
	"SELECT sift.col_promote('ev_c')" \
	"VACUUM (FREEZE, ANALYZE) ev" >/dev/null || bail "setup failed"

Q="SELECT g, count(*), sum(v) FROM ev GROUP BY g ORDER BY g"
rows() { sql dl "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = $1" "$Q"; }
agree() { test "$(rows on)" == "$(rows off)"; }
gens() { sql dl "SELECT string_agg(gen::text, ',' ORDER BY gen) FROM sift.col_segments('ev_c') WHERE kind = 'immutable'"; }
records() { sql dl "SELECT count(*) FROM sift._test_col_log_records('ev_c')"; }
waiting() { sql dl "SELECT count(*) FROM pg_stat_activity WHERE query LIKE '$1%' AND wait_event_type = 'Lock' AND wait_event = '$2'"; }

before=$(gens)
check "the index has two immutable segments" test "$(tr -cd , <<<"$before" | wc -c)" -ge 1

rm -f "$D/H.in"; mkfifo "$D/H.in"
psql -X -At -q -h "$D/sock" -p "$(port dl)" -U postgres -d postgres <"$D/H.in" >"$D/H.out" 2>&1 &
exec {FD}>"$D/H.in"
echo "SELECT pg_advisory_lock($((G + 8)));" >&"$FD"
echo '\echo gate_taken' >&"$FD"
wait_for "grep -q gate_taken '$D/H.out'" 30 || bail "the gate was not taken"

(sql dl "SET sift.test_phase_gate = $G" "DELETE FROM ev WHERE id % 20 = 3" >"$D/delete.out" 2>&1; echo $? >"$D/delete.rc") &
check "the delete waits at its gate with the units of its rows found" wait_for "[[ \$(waiting 'DELETE FROM ev' advisory) == 1 ]]" 60

(sql dl "SELECT sift.col_merge('ev_c', true)" >"$D/merge.out" 2>&1; echo $? >"$D/merge.rc") &
check "the merge waits for the tombstone lock the delete holds" wait_for "[[ \$(waiting 'SELECT sift.col_merge' page) == 1 ]]" 60
check "the merge has replaced nothing while the delete is held" test "$(gens)" == "$before"

echo "SELECT pg_advisory_unlock($((G + 8)));" >&"$FD"
wait_for "[[ -f '$D/delete.rc' && -f '$D/merge.rc' ]]" 120 || bail "the delete and the merge did not finish"
check "the delete wrote its records and the merge carried them to marks: both succeeded" test "$(cat "$D/delete.rc")|$(cat "$D/merge.rc")" == "0|0"
check "the merge replaced the segments after the delete" test "$(gens)" != "$before"
check "the answers after the merge are the heap's, with the 3,000 deleted rows gone" test "$(agree && sql dl 'SELECT count(*) FROM ev')" == 57000
sql dl "SELECT sift._test_col_fold_marks('ev_c')" "VACUUM ev" >/dev/null
check "after the fold and VACUUM no record is left and the answers are the heap's" test "$(records)|$(agree && echo same)" == "0|same"
exec {FD}>&-
wait
finish
