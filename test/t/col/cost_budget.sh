#!/usr/bin/env bash
# tap: pooled
# The common requests of a table with a sift_col index stay inside absolute resource bounds, so that a change that makes them dearer fails here: the WAL of a row inserted in its own transaction and of a row of a batch, the WAL a VACUUM of an index nothing changed writes (none), and the
# background workers a table with nothing to do starts (none, and so no log line).
. "${SIFT_REPO:?}/test/t/lib/common.bash"

ROWS=$(knob SIFT_CB_ROWS 20000 20000)
SINGLE=300
BATCH=5000
plan 5
node_init primary 28 "autovacuum = off
sift.log_min_duration = 0"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift" \
	"CREATE TABLE t (id int PRIMARY KEY, grp int, amount numeric(10,2), label text) WITH (autovacuum_enabled = off)" \
	"INSERT INTO t SELECT g, g % 50, g % 1000, 'x' || g % 97 FROM generate_series(1, $ROWS) g" \
	"CREATE INDEX t_idx ON t USING sift_col (id, grp, amount, label)" \
	"VACUUM (FREEZE, ANALYZE) t" >/dev/null || bail "setup failed"

wal() { sql primary "SELECT pg_current_wal_insert_lsn()"; }
wal_bytes() { sql primary "SELECT pg_wal_lsn_diff('$2', '$1')::bigint"; }
workers() { grep -c "sift maintenance worker\|sift: .* worker finished" "$(dir primary)/postgres.log"; }

sql primary "CHECKPOINT" >/dev/null
start=$(wal)
for i in $(seq $SINGLE); do echo "INSERT INTO t VALUES ($((ROWS + i)), 7, 3.5, 'abc');"; done | sql primary >/dev/null
single=$(( $(wal_bytes "$start" "$(wal)") / SINGLE ))
check "a row inserted in its own transaction writes at most 700 bytes of WAL (it wrote $single)" test "$single" -le 700

start=$(wal)
sql primary "INSERT INTO t SELECT $((ROWS * 2)) + g, g % 50, g % 1000, 'x' || g % 97 FROM generate_series(1, $BATCH) g" >/dev/null
batch=$(( $(wal_bytes "$start" "$(wal)") / BATCH ))
check "a row of a batch of $BATCH writes at most 300 bytes of WAL (it wrote $batch)" test "$batch" -le 300

sql primary "VACUUM t" >/dev/null
sql primary "CHECKPOINT" >/dev/null
start=$(wal)
sql primary "VACUUM t" "VACUUM t" >/dev/null
check "a VACUUM of an index nothing changed writes at most 1 kB of WAL (it wrote $(wal_bytes "$start" "$(wal)"))" test "$(wal_bytes "$start" "$(wal)")" -le 1024

before=$(workers)
sql primary "SELECT pg_sleep(6)" >/dev/null
check "a table with nothing to do starts no background worker (it started $(( $(workers) - before )))" test "$(( $(workers) - before ))" -eq 0

check "the index answers after all of it" test "$(sql primary "SELECT count(*) FROM t WHERE grp = 7")" -ge 1
finish
