#!/usr/bin/env bash
# What the background maintenance leaves behind and what it costs when it has nothing to do:
# (a) a rewrite worker that fails after it wrote its output leaves no block in the index that no segment lists: the next rewrite writes into them, and the file does not hold
#     a third copy of the index (a killed worker is swept by the next rewrite worker the same way); the extra space of the rewrite is the index once, not more;
# (b) a worker that was asked for a promotion looks for more work at growing intervals and, when it did none, ends without a line in the log of the server;
# (c) a worker the server has no slot for says so with the slots in the detail, by the job that holds them.
. "$(dirname "$0")/lib.bash"

plan 15
node_init mp 71 "autovacuum = off
sift.test_col_net = off
sift.col_auto_merge = on
sift.col_auto_merge_wal_pct = 100
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
moved_parents() { sql mp "UPDATE dim_a SET id = id + $1 WHERE id BETWEEN $2 AND $3"; }
live=$(blocks events_col)

# (a) the worker is shut down after its output is written (an operator's SIGTERM), then fails with an error
fails() { sql mp "ALTER DATABASE postgres SET sift.test_col_merge_error = $1" >/dev/null; }
rewrite() { sql mp "VACUUM events" >/dev/null; }
fails 2
moved_parents 10000 1 300 >/dev/null
check "the keys the parent changed make the connection stale" test "$(due events_col)" == "stale connection"
rewrite
check "the rewrite worker was shut down" wait_for '[[ $(grep -c "terminating\|sift rewrite.*exited with exit code 1" "$D/postgres.log") -ge 1 ]]' 60
check "and ended" wait_for quiet 60
peak=$(blocks events_col)
diag "index blocks before the rewrite $live, after the one that was shut down $peak"
check "its output is in the file for the next rewrite worker to sweep" test "$(orphans events_col)" -gt 0
# the next rewrite worker sweeps what it left, and gives its own output back at once when it fails with an error
fails 1
rewrite
check "and ended" wait_for quiet 60
check "nothing the index cannot reuse is left in the file" test "$(orphans events_col)" == 0
fails 0
check "the rewrite is still due" test "$(due events_col)" == "stale connection"
sql mp "SELECT sift.col_maintain('events_col')" >/dev/null
after=$(blocks events_col)
diag "index blocks after the rewrite that succeeded $after"
check "the rewrite is done" test -z "$(due events_col)"
check "the file did not grow past the rewrite that was shut down (the output went where it left blocks)" test "$after" -le "$peak"
check "the extra space a rewrite needs is the index once" test "$peak" -le "$((live * 21 / 10))"

# (b) a worker asked for a promotion, with nothing more to do
node_init mi 72 "autovacuum = off
sift.col_auto_merge = on
sift.test_col_net = off
sift.maintenance_naptime = 2000
sift.log_min_duration = 0"
node_start mi || bail "start of mi failed"
sql mi "CREATE EXTENSION pg_sift" "CREATE TABLE w (id int PRIMARY KEY, k int, v int) WITH (autovacuum_enabled = false)" \
	"CREATE INDEX w_col ON w USING sift_col (id, k, v) WITH (write_buffer_limit = 64)" >/dev/null || bail "setup of mi failed"
POLLS=0
idle() { POLLS=$((POLLS + 1)); [[ $(sql mi "SELECT count(*) FROM pg_stat_activity WHERE backend_type LIKE 'sift %'") == 0 ]]; }
promoted() { POLLS=$((POLLS + 1)); [[ $(sql mi "SELECT count(*) FROM sift.col_segments('w_col') WHERE kind = 'immutable'") -ge 1 ]]; }
commits() { sql mi "SELECT pg_stat_force_next_flush()" "SELECT xact_commit FROM pg_stat_database WHERE datname = 'postgres'" | tail -1; }
c0=$(commits)
sql mi "INSERT INTO w SELECT i, i % 10, i FROM generate_series(1, 9000) i" >/dev/null
check "the worker promoted the write buffer" wait_for promoted 60
check "and ended" wait_for idle 60
c1=$(commits)
looks=$((c1 - c0 - 2 * POLLS - 6))
diag "the worker's looks and its promotion committed about $looks transactions"
check "its looks at the index cost few transactions" test "$looks" -le 12
check "it said nothing of an idle stay" test "$(grep -c 'worker finished' "$(dir mi)/postgres.log")" == 0

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
