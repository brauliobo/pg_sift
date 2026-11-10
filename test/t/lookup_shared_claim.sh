#!/usr/bin/env bash
# The sessions that run one query at once load the rows of its joined table once (agg/lookshm.c): the first to ask claims the load, the others wait for its rows. A session that
# looked for the rows before they were kept and asked for the claim after the loader dropped its own found no claim to wait for and loaded the table again: the
# loader's claim ended between its two steps. An injection point (PostgreSQL built with --enable-injection-points: scripts/pg-assert.sh --inject) stops a session
# between the two, without a sleep, while another session loads and keeps the rows to the end: the stopped session then reads those rows and loads none.
. "$(dirname "$0")/lib.bash"

pg_config --configure | grep -q -- --enable-injection-points || { echo "1..0 # SKIP PostgreSQL without injection points (scripts/pg-assert.sh --inject)"; exit 0; }
((PG_MAJOR >= 18)) || { echo "1..0 # SKIP the wait of an injection point is named from PostgreSQL 18"; exit 0; }

plan 4
node_init lc 75 "shared_buffers = 256MB
max_parallel_workers_per_gather = 0
seq_page_cost = 10000
random_page_cost = 10000"
node_start lc || bail "start failed"
D=$(dir lc)
sql lc "CREATE EXTENSION pg_sift" "CREATE EXTENSION injection_points" >/dev/null || bail "extensions failed"
sql lc "CREATE TABLE ev (id int PRIMARY KEY, dim_a_id int, amount int) WITH (autovacuum_enabled = false)" \
	"INSERT INTO ev SELECT i, 1 + i % 40000, i % 100 FROM generate_series(1, 80000) i" \
	"CREATE INDEX ev_col ON ev USING sift_col (dim_a_id, amount)" \
	"CREATE TABLE ents (id int, label text) WITH (autovacuum_enabled = false)" \
	"INSERT INTO ents SELECT i, 'e' || i % 50 FROM generate_series(1, 40000) i" \
	"VACUUM (FREEZE, ANALYZE) ev, ents" >/dev/null || bail "setup failed"

Q='SELECT e.label, count(*) AS n, sum(amount) AS s FROM ev JOIN ents e ON e.id = ev.dim_a_id GROUP BY 1'
# the tables the statement read from the rows another session kept (0: it loaded its own)
cached() { sql lc "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) $Q" | { grep -o '[0-9]* cached tables' || echo '0 cached tables'; } | cut -d' ' -f1; }
waiting() { sql lc "SELECT count(*) FROM pg_stat_activity WHERE wait_event_type = 'InjectionPoint' AND wait_event = 'sift-lookshm-before-claim'"; }

sql lc "SELECT injection_points_attach('sift-lookshm-before-claim', 'wait')" >/dev/null
# the stopped session: it missed the rows, and has not asked for the claim
cached >"$D/b.out" & b=$!
for _ in $(seq 300); do [[ $(waiting) -ge 1 ]] && break; sleep 0.1; done
check "a session that missed the rows stops before it asks for the claim" test "$(waiting)" -ge 1
# another session loads the rows, keeps them and ends its claim; it passes the point (the point is detached for it)
sql lc "SELECT injection_points_detach('sift-lookshm-before-claim')" >/dev/null
check "a second session loads the rows and keeps them" test "$(cached)" == 0
sql lc "SELECT injection_points_wakeup('sift-lookshm-before-claim')" >/dev/null
wait "$b"
check "the stopped session reads the rows that were kept, it does not load them again" test "$(cat "$D/b.out")" == 1
check "a later session reads them too" test "$(cached)" == 1
finish
