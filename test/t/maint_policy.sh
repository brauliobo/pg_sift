#!/usr/bin/env bash
# The server never rewrites an index by itself: the user's REINDEX does, and sift.col_maintenance_status says when to.
# (a) rounds of non-key updates of the parent of a connected index, at 3% and at 8% of its rows, with the workers on: no rewrite, and the child stays within what
#     merges and VACUUM leave of it;
# (b) when the connection is stale (the parent changed keys) or the units are dirty (a key_cut index), the workers and VACUUM leave the segments as they are,
#     the status recommends REINDEX with the reason, and the reads answer as the plan without sift does;
# (c) REINDEX CONCURRENTLY of the child restores the fast path, clears the recommendation, and the answers are those of the index before it;
# (d) the incremental work stays automatic: the rows of the write buffer are promoted;
# (e) a worker the server has no slot for says so with the slots in the detail, by the job that holds them.
. "$(dirname "$0")/lib.bash"

plan 21
node_init mp 71 "autovacuum = off
sift.test_col_net = off
sift.async_maintenance = on
sift.maintenance_naptime = 200
sift.log_min_duration = 0"
node_start mp || bail "start failed"
D=$(dir mp)
sql mp "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "extensions failed"
sql mp "CREATE TABLE dim_a (id int PRIMARY KEY, label text, price int, touched int) WITH (autovacuum_enabled = false)" \
	"INSERT INTO dim_a SELECT i, 'l' || i, i * 10, 0 FROM generate_series(1, 2000) i" \
	"CREATE INDEX dim_a_col ON dim_a USING sift_col (id, label, price)" \
	"CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, v int NOT NULL, pad text) WITH (autovacuum_enabled = false)" \
	"INSERT INTO events SELECT i, 1 + i % 2000, i, repeat('x', 40) FROM generate_series(1, 120000) i" \
	"CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, v, pad) WITH (connect = 'dim_a_id -> dim_a(id)')" \
	"CREATE INDEX events_kc ON events USING sift_col (id, dim_a_id, v, pad) WITH (key_cut = 'dim_a_id', dead_rewrite_pct = 100)" \
	"SELECT sift.col_maintain('events_col')" >/dev/null || bail "setup failed"
sql mp "ALTER DATABASE postgres SET sift.vacuum_rewrite_limit = 0" >/dev/null

quiet() { [[ $(sql mp "SELECT count(*) FROM pg_stat_activity WHERE backend_type LIKE 'sift %'") == 0 ]]; }
blocks() { sql mp "SELECT pg_relation_size('$1') / 8192"; }
due() { sql mp "SELECT coalesce(sift.col_maintenance_due('$1'), '')"; }
advice() { sql mp "SELECT coalesce((SELECT b FROM unnest(blocked_by) b WHERE b LIKE 'REINDEX recommended%'), '') FROM sift.col_maintenance_status('$1')"; }
gens() { sql mp "SELECT string_agg(gen::text, ',' ORDER BY gen) FROM sift.col_segments('$1') WHERE kind = 'immutable'"; }
missing() { sql mp "SELECT count(*) FROM unnest(string_to_array('$2', ',')::bigint[]) g WHERE g NOT IN (SELECT gen FROM sift.col_segments('$1') WHERE kind = 'immutable')"; }
rewrites() { echo $(($(grep -c 'rewrote (' "$D/postgres.log") - base)); }
answers() { sql mp "SET sift.enable_agg = $1" "SELECT count(*) || ':' || sum(e.v) || ':' || sum(a.price) FROM events e JOIN dim_a a ON a.id = e.dim_a_id WHERE a.price > 5000 AND e.v % 7 = 0" | tail -1; }
vacuum_all() { sql mp "VACUUM dim_a" "VACUUM events" >/dev/null; wait_for quiet 60; }
live=$(blocks events_col)
base=$(grep -c 'rewrote (' "$D/postgres.log")

# (a) the loop that rewrote the child again and again
for pct in 3 8; do
	for r in 1 2 3 4 5 6 7 8; do
		sql mp "UPDATE dim_a SET price = price + 1 WHERE (id + $r * 7) % 100 < $pct" \
			"INSERT INTO events SELECT 200000 + $pct * 1000 + $r * 100 + i, 1 + i % 2000, i, 'n' FROM generate_series(1, 50) i" \
			"UPDATE events SET pad = pad || 'x' WHERE id BETWEEN $r * 100 AND $r * 100 + 99" >/dev/null
		vacuum_all
	done
	check "8 rounds of $pct% parent updates: no rewrite" test "$(rewrites)" == 0
	check "and the child stays within what merges and VACUUM leave of it" test "$(blocks events_col)" -le "$((live * 3 / 2))"
	diag "after $pct%: due '$(due events_col)', child blocks $(blocks events_col) of $live"
done

# (b) a stale connection and dirty units: left as they are, with the advice
sql mp "SELECT sift.col_promote('events_col')" >/dev/null
wait_for quiet 60
g_col=$(gens events_col)
sql mp "UPDATE dim_a SET id = id + 10000 WHERE id BETWEEN 1 AND 300" >/dev/null
vacuum_all
check "the keys the parent changed make the connection stale" test "$(due events_col)" == "stale connection"
check "the workers and VACUUM kept every segment of the child" test "$(missing events_col "$g_col")" == 0
check "no rewrite was made" test "$(rewrites)" == 0
check "the status recommends REINDEX with the reason" test "$(advice events_col)" == "REINDEX recommended (stale connection): the server does not rewrite an index by itself, REINDEX INDEX CONCURRENTLY events_col does"
check "the answers are those of the plan without sift" test "$(answers on)" == "$(answers off)"

sql mp "SELECT sift.col_promote('events_kc')" >/dev/null
wait_for quiet 60
g_kc=$(gens events_kc)
sql mp "UPDATE events SET v = v + 1 WHERE id % 3 = 0" >/dev/null
vacuum_all
check "dirty units of a key_cut index are due" test "$(due events_kc)" == "dirty units"
check "and every segment of it was kept" test "$(missing events_kc "$g_kc")" == 0
check "with the REINDEX advice" test "$(advice events_kc)" == "REINDEX recommended (dirty units): the server does not rewrite an index by itself, REINDEX INDEX CONCURRENTLY events_kc does"

# (c) REINDEX CONCURRENTLY
sql mp "REINDEX INDEX CONCURRENTLY events_col" "REINDEX INDEX CONCURRENTLY events_kc" >/dev/null
check "the child is not stale after the REINDEX" test "$(due events_col)" == ""
check "and the recommendation is gone" test "$(advice events_col)" == ""
check "the key_cut index is clean after the REINDEX" test "$(due events_kc)" == ""
check "and its recommendation is gone" test "$(advice events_kc)" == ""
check "the answers after the REINDEX are the plan without sift's" test "$(answers on)" == "$(answers off)"
check "the index verifies" sql mp "SELECT sift.col_verify('events_col', true)"

# (d) the incremental work
sql mp "INSERT INTO events SELECT 500000 + i, 1 + i % 2000, i, 'w' FROM generate_series(1, 3000) i" "VACUUM events" >/dev/null
check "the rows of the write buffer are promoted by the worker" wait_for '[[ $(sql mp "SELECT coalesce(sum(rows), 0) FROM sift.col_segments('"'"'events_col'"'"') WHERE kind = '"'"'write_buffer'"'"'") == 0 ]]' 60
check "and still no rewrite was made" test "$(rewrites)" == 0

# (e) no slot: the session holds every slot of sift.max_maint_workers, so the worker its VACUUM asks for is refused, and the message says how many slots sift has and how many are running
node_init ms 73 "autovacuum = off
sift.test_col_net = off
sift.async_maintenance = on"
node_start ms || bail "start of ms failed"
sql ms "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" "CREATE TABLE t (id int, k int) WITH (autovacuum_enabled = false)" "INSERT INTO t SELECT g, g % 10 FROM generate_series(1, 2000) g" \
	"CREATE INDEX t_idx ON t USING sift_col (id, k)" "ALTER DATABASE postgres SET sift.vacuum_rewrite_limit = 0" >/dev/null || bail "setup of ms failed"
sql ms "SELECT sift._test_worker_slots(8)" "INSERT INTO t SELECT g, g % 10 FROM generate_series(2001, 4000) g" "VACUUM t" >/dev/null || bail "VACUUM of ms failed"
refused() { grep -q 'no background worker for promote of index "public.t_idx"' "$(dir ms)/postgres.log" && grep -q 'DETAIL:  sift.max_maint_workers is 2 and 2 are running; max_worker_processes is 8' "$(dir ms)/postgres.log"; }
check "a worker the server has no slot for names who holds the slots" refused
finish
