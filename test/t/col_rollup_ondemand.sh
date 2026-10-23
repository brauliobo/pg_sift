#!/usr/bin/env bash
# The on-demand refresh of the rollups of a sift_col index (docs/COL-PREAGG.md section 7): a SELECT of a hot standby scans the units without
# the rollup and never queues them (the queue of the standby stays empty), the same SELECT of the primary queues their segments and the answers
# are those of the heap; a PANIC of the refresh after it wrote the new segment and before it replaced the old one leaves the index as it was (it
# verifies, the same segments, the same answers) and the request, made again, is served; the standby replays the rewritten segments and answers
# from the rollups.
. "$(dirname "$0")/lib.bash"

plan 15
node_init primary 2 "wal_level = replica
max_wal_senders = 4
wal_keep_size = $(knob SIFT_WAL_KEEP 1GB 256MB)
autovacuum = off"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE orders (id bigserial PRIMARY KEY, doc jsonb)" \
	"INSERT INTO orders (doc) SELECT (SELECT jsonb_build_object('items', jsonb_agg(jsonb_build_object('a', 'a' || (g + e) % 4, 'b', 'b' || (g * e) % 3, 'q', (g * e) % 9)))
		FROM generate_series(1, g % 4 + 1) e) FROM generate_series(1, 30000) g" \
	"CREATE INDEX o_col ON orders USING sift_col (id, doc) WITH (write_buffer_limit = 4096, paths = 'doc.items[]{a text, b text, q int4}', rollup = 'off')" \
	"VACUUM (DISABLE_PAGE_SKIPPING) orders" \
	"ALTER INDEX o_col SET (rollup = 'doc.items[].a, doc.items[].b')" >/dev/null || bail "setup failed"

node_clone primary standby 3 "hot_standby_feedback = on
wal_receiver_status_interval = 1s"
node_start standby || bail "standby start failed"
ok "standby streaming from a base backup"

Q="SELECT e.a, e.b, count(*), sum(e.q) FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.doc->'items') e(a text, b text, q int) GROUP BY 1, 2"
SETTINGS=("LOAD 'pg_sift'" "SET sift.col_rollup_worker = off" "SET max_parallel_workers_per_gather = 0" "SET seq_page_cost = 10000")
# the plan of the query: its Units, Units From Pre-aggregates, Units Without The Rollup, Rollup Refresh Queued (0 when the line is absent)
plan_of() {
	local out
	out=$(sql "$1" "${SETTINGS[@]}" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) $Q")
	for f in 'Units' 'Units From Pre-aggregates' 'Units Without The Rollup' 'Rollup Refresh Queued'; do
		grep -E "^\s*$f: [0-9]+" <<<"$out" | grep -oE '[0-9]+$' || echo 0
	done | paste -sd' '
}
answer() { sql "$1" "${SETTINGS[@]}" "SET sift.enable_agg = $2" "SELECT string_agg(x::text, ',' ORDER BY x::text) FROM ($Q) x" | tail -1; }
state() { sql "$1" "SELECT requested, refreshed, skipped FROM sift.col_rollup_state" | tr '|' ' '; }
SEGS="SELECT string_agg(gen || ':' || kind || ':' || coalesce(rows, 0), ' ' ORDER BY gen) FROM sift.col_segments('o_col') WHERE kind = 'immutable'"

read -r UNITS _ <<<"$(plan_of standby)"
check "a SELECT of the standby scans every unit for lack of the rollup and queues none" test "$(plan_of standby)" == "$UNITS 0 $UNITS 0"
check "the standby's queue stays empty" test "$(state standby)" == "0 0 0"
check "its answer is the heap's" test "$(answer standby on)" == "$(answer primary off)"

check "the same SELECT of the primary queues every unit" test "$(plan_of primary)" == "$UNITS 0 $UNITS $UNITS"
check "and the answer is the heap's" test "$(answer primary on)" == "$(answer primary off)"
check "the primary's queue holds requests, the standby's still none" test "$(sql primary "SELECT count(*) > 0 FROM sift.col_rollup_requests")|$(state standby)" == "t|0 0 0"

# the PANIC of the refresh: the new segment is written, the old one not yet replaced
BEFORE=$(sql primary "$SEGS")
SIFT_TMP=$(dir primary) leak_expect 1 'PANIC:'
SIFT_TMP=$(dir primary) leak_expect 1 'terminated by signal 6'
node_conf primary "sift.test_col_rollup_panic = 1"
pg_ctl -D "$(dir primary)/data" reload >/dev/null
sleep 1
refresh_panics() { ! sql primary "SELECT sift.col_rollup_refresh('o_col')" >/dev/null 2>&1; }
check "the refresh PANICs" refresh_panics
for _ in $(seq 300); do [[ -f $(dir primary)/data/postmaster.pid ]] || break; sleep 0.1; done
sed_i '/^sift.test_col_rollup_panic/d' "$(dir primary)/data/postgresql.conf"
check "the primary restarts through crash recovery" node_start primary
check "the index verifies after the crash" test -z "$(sql primary "SELECT sift.col_verify('o_col', true)" 2>&1)"
check "its segments are those it had" test "$(sql primary "$SEGS")" == "$BEFORE"
check "the units are still scanned, with the heap's answer" test "$(plan_of primary | cut -d' ' -f1-3)|$(answer primary on)" == "$UNITS 0 $UNITS|$(answer primary off)"

# the request made again is served
sql primary "SELECT sift.col_rollup_refresh('o_col') > 0" >/dev/null
check "the refresh, asked again, rewrites the segments: the units answer from the rollups" test "$(plan_of primary)" == "$UNITS $UNITS 0 0"
check "with the heap's answer" test "$(answer primary on)" == "$(answer primary off)"
catchup primary standby
check "the standby replays the segments and answers from the rollups too, queuing nothing" \
	test "$(plan_of standby)|$(state standby)|$(sql standby "SELECT sift.col_verify('o_col', true)" 2>&1)" == "$UNITS $UNITS 0 0|0 0 0|"
finish
