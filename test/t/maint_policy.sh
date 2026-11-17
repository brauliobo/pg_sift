#!/usr/bin/env bash
# What the background maintenance leaves behind and what it says when it cannot start:
# (a) a rewrite worker that fails with an error after it wrote its output gives the blocks back before it ends (no VACUUM runs after it was asked for);
# (b) one that the operator shut down leaves them, and the next rewrite worker returns them when it starts, before anything else: the sweep that returns them is the
#     worker's (its pid is in the line of the log), and the rewrite then writes where they were: the file does not hold a third copy of the index;
# (c) a worker the server has no slot for says so with the slots in the detail, by the job that holds them.
. "$(dirname "$0")/lib.bash"

plan 13
node_init mp 71 "autovacuum = off
sift.test_col_net = off
sift.col_auto_merge = on
sift.col_auto_merge_wal_pct = 0
sift.maintenance_naptime = 200
sift.log_min_duration = 0"
node_start mp || bail "start failed"
D=$(dir mp)
sql mp "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "extensions failed"
sql mp "CREATE TABLE dim_a (id int PRIMARY KEY, label text, price int) WITH (autovacuum_enabled = false)" \
	"INSERT INTO dim_a SELECT i, 'l' || i, i * 10 FROM generate_series(1, 2000) i" \
	"CREATE INDEX dim_a_col ON dim_a USING sift_col (id, label, price)" \
	"CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, v int NOT NULL, pad text) WITH (autovacuum_enabled = false)" \
	"INSERT INTO events SELECT i, 1 + i % 2000, i, repeat('x', 40) FROM generate_series(1, 120000) i" \
	"CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, v, pad) WITH (connect = 'dim_a_id -> dim_a(id)')" \
	"SELECT sift.col_maintain('events_col')" >/dev/null || bail "setup failed"
sql mp "ALTER DATABASE postgres SET sift.vacuum_rewrite_limit = 0" >/dev/null

quiet() { [[ $(sql mp "SELECT count(*) FROM pg_stat_activity WHERE backend_type LIKE 'sift %'") == 0 ]]; }
blocks() { sql mp "SELECT pg_relation_size('$1') / 8192"; }
orphans() { sql mp "SELECT sift._test_check('$1')"; }
failures() { sql mp "SELECT coalesce(max(failures), 0) FROM sift.workers WHERE index = '$1'::regclass"; }
due() { sql mp "SELECT coalesce(sift.col_maintenance_due('$1', false), '')"; }
returned() { grep -c 'sift: returned [0-9]* blocks that no segment lists to index "events_col"' "$D/postgres.log"; }
fails() { sql mp "ALTER DATABASE postgres SET sift.test_col_merge_error = $1" >/dev/null; }
live=$(blocks events_col)

# (a) a rewrite worker that fails with an error
fails 1
sql mp "UPDATE dim_a SET id = id + 10000 WHERE id BETWEEN 1 AND 300" >/dev/null
check "the keys the parent changed make the connection stale" test "$(due events_col)" == "stale connection"
sql mp "VACUUM events" >/dev/null
check "the rewrite worker failed" wait_for '[[ $(failures events_col) -ge 1 ]]' 60
check "and ended" wait_for quiet 60
check "it returned the blocks of its output before it ended" test "$(returned)" == 1
check "and nothing the index cannot reuse is left in the file" test "$(orphans events_col)" == 0

# (b) a rewrite worker that the operator shut down
fails 2
sql mp "VACUUM events" >/dev/null
check "the second rewrite worker was shut down" wait_for '[[ $(grep -c "sift rewrite.*exited with exit code 1" "$D/postgres.log") -ge 1 ]]' 60
check "and ended" wait_for quiet 60
peak=$(blocks events_col)
diag "index blocks before the rewrites $live, with the output of the one that was shut down $peak"
check "its output stays in the file until a worker returns it" test "$(orphans events_col)" -gt 0
fails 0
check "a rewrite worker asked for by no VACUUM starts" test "$(sql mp "SELECT sift._test_col_rewrite_worker('events_col')")" == t
check "it returned the output of the one that was shut down, and rewrote" wait_for '[[ $(returned) -ge 2 ]] && quiet && [[ -z $(due events_col) ]]' 60
after=$(blocks events_col)
diag "index blocks after the rewrite $after"
check "the output of the rewrite went where those blocks were" test "$after" -le "$peak"
check "the extra space a rewrite needs is the index once" test "$peak" -le "$((live * 21 / 10))"

# (c) no slot
node_init ms 73 "autovacuum = off
max_worker_processes = 2
sift.test_col_net = off
sift.col_auto_merge = on"
node_start ms || bail "start of ms failed"
sql ms "CREATE EXTENSION pg_sift" "CREATE TABLE t (id int, k int) WITH (autovacuum_enabled = false)" "INSERT INTO t SELECT g, g % 10 FROM generate_series(1, 2000) g" \
	"CREATE INDEX t_idx ON t USING sift_col (id, k)" "ALTER DATABASE postgres SET sift.vacuum_rewrite_limit = 0" \
	"DELETE FROM t WHERE id % 2 = 0" "VACUUM t" >/dev/null || bail "setup of ms failed"
check "a worker the server has no slot for names who holds the slots" wait_for 'grep -q "slots are held, [0-9]* of them by sift workers (none)" "$(dir ms)/postgres.log"' 60
finish
