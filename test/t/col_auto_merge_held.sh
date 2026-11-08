#!/usr/bin/env bash
# The automatic rewrite of the runs a build left waits for the horizon once the blocks a snapshot holds back are one pass (sift.col_auto_merge_pass_mb).
# A pass writes its output to new blocks, and what it frees is reusable only when the snapshots that may read it end (a standby with hot_standby_feedback, a long
# query, the transactions of REINDEX CONCURRENTLY). The gate stopped at half the index, so a held horizon took the file to 1.5 times the live bytes and a pass more; it
# stops at one pass now: the file stands within the live bytes plus the blocks of one pass, and the rewrite goes on when the snapshot ends.
. "$(dirname "$0")/lib.bash"

ROWS=$(knob SIFT_HELD_ROWS 337920 337920)
plan 8
node_init held 25 "autovacuum = off
sift.test_col_net = off
sift.col_auto_merge_pass_mb = 2
sift.vacuum_rewrite_limit = 1"
node_start held || bail "start failed"
sql held "CREATE EXTENSION pg_sift" \
	"CREATE TABLE kp (id int, tenant_id int, d date, amount int, pad text) WITH (autovacuum_enabled = false)" \
	"INSERT INTO kp SELECT i, (i::bigint * 7919 % 400)::int, date '2026-01-01' + (i * 31) % 300, i % 100, md5(i::text) || md5((i + 1)::text) || md5((i + 2)::text) FROM generate_series(1, $ROWS) i" \
	"VACUUM (FREEZE, ANALYZE) kp" >/dev/null || bail "setup failed"
# runs that each hold rows of every tenant (the build by key ranges is off): the rewrite cuts them by key, a window of them at a time
sql held "SET max_parallel_maintenance_workers = 0" "SET sift.col_build_key_ranges = off" "SET sift.test_col_segment_kb = $(knob SIFT_HELD_SEGKB 7000 7000)" \
	"CREATE INDEX kp_idx ON kp USING sift_col (id, tenant_id, d, amount, pad) WITH (key_cut = 'tenant_id', dict_lookup = off)" >/dev/null || bail "create failed"

size() { sql held "SELECT index_bytes FROM sift.col_index_status WHERE index = 'kp_idx'::regclass"; }
live() { sql held "SELECT segment_bytes FROM sift.col_index_status WHERE index = 'kp_idx'::regclass"; }
biggest() { sql held "SELECT max(bytes) FROM sift.col_segments('kp_idx') WHERE kind = 'immutable'"; }
runs() { sql held "SELECT count(*) FROM sift.col_segments('kp_idx') WHERE kind = 'immutable'"; }
due() { sql held "SELECT coalesce(sift.col_maintenance_due('kp_idx', true), '-')"; }
blocked() { sql held "SELECT array_to_string(blocked_by, ',') FROM sift.col_maintenance_status('kp_idx')"; }
snapshot_blocks() { [[ $(blocked) == *snapshot* ]]; }
holders() { sql held "SELECT count(*) FROM pg_stat_activity WHERE application_name = 'holder' AND backend_xmin IS NOT NULL"; }
# the passes of the worker: one while the automatic rule says the index is due and the last one changed the segments (the manual call has no gate)
gens() { sql held "SELECT string_agg(gen::text, ',' ORDER BY gen) FROM sift.col_segments('kp_idx') WHERE kind = 'immutable'"; }
passes=0
rewrite_while_due() {
	local before
	passes=0
	while [[ $(due) != - && $passes -lt 60 ]]; do
		before=$(gens)
		sql held "SELECT sift.col_maintain('kp_idx')" >/dev/null
		[[ $(gens) != "$before" ]] || break
		passes=$((passes + 1))
	done
}
bound() { test "$(( $(size) - $(live) ))" -le "$(( (2 << 20) + 2 * BIG ))"; }

first_runs=$(runs)
BIG=$(biggest)
check "the build left several runs that overlap in the key" test "$first_runs" -ge 6 -a "$(due)" == "key ranges overlap"
size_build=$(size)
diag "build: file $size_build live $(live) runs $first_runs due $(due) biggest $BIG"
check "the file is within 1.05 times the live bytes after the build" test "$size_build" -le "$(( $(live) * 105 / 100 ))"

PGAPPNAME=holder psql -X -q -h "$(dir held)/sock" -p "$(port held)" -U postgres -d postgres -c "BEGIN ISOLATION LEVEL REPEATABLE READ" \
	-c "SELECT count(*) FROM kp" -c "SELECT pg_sleep(600)" >/dev/null 2>&1 &
for _ in $(seq 50); do [[ $(holders) == 1 ]] && break; sleep 0.1; done
rewrite_while_due
diag "held: $passes passes, file $size_build -> $(size), live $(live), biggest run $BIG, runs $first_runs -> $(runs)"
check "a snapshot held: the rewrite stops after $passes pass(es), not after half the index" test "$passes" -ge 1 -a "$passes" -le 2
check "... col_maintenance_status names the snapshot" snapshot_blocks
check "... the file is within the live bytes plus one pass and a run" bound
sql held "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE application_name = 'holder'" >/dev/null
for _ in $(seq 50); do [[ $(holders) == 0 ]] && break; sleep 0.1; done
sql held "SELECT pg_current_xact_id()" >/dev/null
check "the snapshot ended: the rewrite is due again" test "$(due)" != "-"
size_held=$(size)
rewrite_while_due
diag "released: $passes passes, file $size_held -> $(size), live $(live), runs $(runs)"
check "... it finishes into the blocks the first passes freed: the file does not grow by more than a pass" test "$(size)" -le "$(( size_held + (2 << 20) + 2 * BIG ))"
check "the index verifies" test -z "$(sql held "SELECT sift.col_verify('kp_idx', true)" 2>&1)"
sql held "DROP TABLE kp" >/dev/null
finish
