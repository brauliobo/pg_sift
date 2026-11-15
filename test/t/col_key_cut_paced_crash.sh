#!/usr/bin/env bash
# A crash between the passes of the paced rewrite of overlapping runs (sift.col_auto_merge_pass_mb), and in the middle of a step of one: the PANIC comes after the
# merge wrote its new segment and before it replaced the runs. Crash recovery leaves a valid index: the first pass is there, the runs the second was to replace are
# as they were, the answers are the heap's, and the passes asked again go on from that directory and end with a key in one unit.
. "$(dirname "$0")/lib.bash"

plan 9
node_init cr 22 "autovacuum = off
sift.test_col_net = off
sift.async_maintenance = off
sift.vacuum_rewrite_limit = 1
sift.col_auto_merge_pass_mb = 2
sift.col_build_key_ranges = off"
node_start cr || bail "start failed"
sql cr "CREATE EXTENSION pg_sift_test CASCADE" "CREATE TABLE kp (id int, tenant_id int, amount int, pad text) WITH (autovacuum_enabled = false)" \
	"INSERT INTO kp SELECT i, (i::bigint * 7919 % 400)::int, i % 100, md5(i::text) || md5((i + 1)::text) || md5((i + 2)::text) FROM generate_series(1, 337920) i" \
	"VACUUM (FREEZE, ANALYZE) kp" "SET max_parallel_maintenance_workers = 0" "SET sift.test_col_segment_kb = 7000" \
	"CREATE INDEX kp_idx ON kp USING sift_col (id, tenant_id, amount, pad) WITH (key_cut = 'tenant_id', dict_lookup = off)" >/dev/null || bail "setup failed"
sql cr "CHECKPOINT" >/dev/null

answer() { sql cr "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = $1" "SELECT string_agg(x::text, ',' ORDER BY x::text) FROM (SELECT tenant_id, count(*), sum(amount) FROM kp WHERE tenant_id IN (3, 42, 399) GROUP BY 1) x" | tail -1; }
runs() { sql cr "SELECT count(*) FROM sift.col_segments('kp_idx') WHERE kind = 'immutable'"; }
point_units() { sql cr "SELECT round(point_units::numeric, 1) FROM sift.col_clustering('kp_idx', 1) WHERE path = 'tenant_id'"; }
HEAP=$(answer off)
check "eight overlapping runs: a key is in each of them" test "$(runs)|$(point_units)" == "8|8.0"

sql cr "CREATE TABLE kp_runs AS SELECT gen FROM sift.col_segments('kp_idx') WHERE kind = 'immutable'" "SELECT sift.col_maintain('kp_idx')" >/dev/null
AFTER_ONE=$(sql cr "SELECT gen FROM sift.col_segments('kp_idx') WHERE kind = 'immutable' ORDER BY gen")
check "a pass rewrites some of the runs and leaves the rest" test "$(sql cr "SELECT count(*) FROM sift.col_segments('kp_idx') JOIN kp_runs USING (gen) WHERE kind = 'immutable'")" -lt 8 -a "$(sql cr "SELECT count(*) FROM sift.col_segments('kp_idx') JOIN kp_runs USING (gen) WHERE kind = 'immutable'")" -gt 1
check "the first pass left an index equal to the heap's with runs still to cut" test "$(answer on)|$([[ $(point_units) != 1.0 ]] && echo more)" == "$HEAP|more"

SIFT_TMP=$(dir cr) leak_expect 1 'PANIC:'
SIFT_TMP=$(dir cr) leak_expect 1 'terminated by signal 6'
maintain_panics() { ! sql cr "SET sift.test_col_merge_panic = on" "SELECT sift.col_maintain('kp_idx')" >/dev/null 2>&1; }
check "the second pass PANICs once its merge wrote its new segment" maintain_panics
for _ in $(seq 300); do [[ -f $(dir cr)/data/postmaster.pid ]] || break; sleep 0.1; done
check "the server restarts through crash recovery" node_start cr
check "the index verifies and the segments of the passes before are as they were" test "$(sql cr "SELECT sift.col_verify('kp_idx', true)" 2>&1)|$(sql cr "SELECT gen FROM sift.col_segments('kp_idx') WHERE kind = 'immutable' AND gen = ANY('{$(tr '\n' ',' <<<"$AFTER_ONE" | sed 's/,$//')}'::bigint[]) ORDER BY gen")" == "|$AFTER_ONE"
check "the answers are the heap's" test "$(answer on)" == "$HEAP"

sql cr "VACUUM kp" "VACUUM kp" >/dev/null
for _ in $(seq 40); do sql cr "SELECT sift.col_maintain('kp_idx')" >/dev/null; [[ $(point_units) == 1.0 ]] && break; done
check "the passes asked again end with a key in one unit" test "$(point_units)" == "1.0"
check "the index verifies and the answers are the heap's" test "$(sql cr "SELECT sift.col_verify('kp_idx', true)" 2>&1)|$(answer on)" == "|$HEAP"
finish
