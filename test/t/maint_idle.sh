#!/usr/bin/env bash
# What a maintenance worker costs when it has nothing more to do: it leaves after two looks that find nothing (not after sift.maintenance_naptime), its looks are spaced
# at growing intervals, and it writes nothing to the log of the server. Measured as the transactions of the database for one promotion and the worker's looks.
. "$(dirname "$0")/lib.bash"

plan 4
node_init mi 72 "autovacuum = off
sift.async_maintenance = on
sift.test_col_net = off
sift.maintenance_naptime = 2000
sift.log_min_duration = 0"
node_start mi || bail "start failed"
sql mi "CREATE EXTENSION pg_sift" "CREATE TABLE w (id int PRIMARY KEY, k int, v int) WITH (autovacuum_enabled = false)" \
	"CREATE INDEX w_col ON w USING sift_col (id, k, v) WITH (write_buffer_limit = 64)" >/dev/null || bail "setup failed"
POLLS=0
idle() { POLLS=$((POLLS + 1)); [[ $(sql mi "SELECT count(*) FROM pg_stat_activity WHERE backend_type LIKE 'sift %'") == 0 ]]; }
promoted() { POLLS=$((POLLS + 1)); [[ $(sql mi "SELECT count(*) FROM sift.col_segments('w_col') WHERE kind = 'immutable'") -ge 1 ]]; }
commits() { sql mi "SELECT pg_stat_force_next_flush()" "SELECT xact_commit FROM pg_stat_database WHERE datname = 'postgres'" | tail -1; }
c0=$(commits)
T0=$(date +%s.%N)
sql mi "INSERT INTO w SELECT i, i % 10, i FROM generate_series(1, 9000) i" >/dev/null
check "the worker promoted the write buffer" wait_for promoted 60
check "and ended" wait_for idle 60
T1=$(date +%s.%N)
c1=$(commits)
looks=$((c1 - c0 - 2 * POLLS - 6))
diag "worker run: about $looks transactions, $(awk "BEGIN { printf \"%.1f\", $T1 - $T0 }") s from the insert to the worker's end"
diag "log lines of the worker's end: $(grep -c 'worker finished' "$(dir mi)/postgres.log")"
check "its looks at the index cost few transactions" test "$looks" -le 12
check "it said nothing of an idle stay" test "$(grep -c 'worker finished' "$(dir mi)/postgres.log")" == 0
finish
