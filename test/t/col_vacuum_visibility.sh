#!/usr/bin/env bash
# What leaves pages out of the visibility map after a plain VACUUM of a table with a sift_col index (the units over them answer from their rows, not their
# pre-aggregates): with synchronous_commit on and no old snapshot, nothing; a snapshot older than the churn (a transaction anywhere in the cluster) keeps the
# pages the churn touched out, and one more VACUUM after it ends sets them. (With synchronous_commit off the commit records not yet flushed do the same:
# col_auto_merge_vacuum_async.)
. "$(dirname "$0")/lib.bash"

plan 4
node_init vm 26 "autovacuum = off
synchronous_commit = on"
node_start vm || bail "start failed"
sql vm "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_visibility" "CREATE TABLE t (id int PRIMARY KEY, k int, pad text) WITH (autovacuum_enabled = false)" \
	"INSERT INTO t SELECT g, g % 100, repeat('x', 150) FROM generate_series(1, 100000) g" "CREATE INDEX t_c ON t USING sift_col (id, k)" \
	"VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) t" >/dev/null || bail "setup failed"
D=$(dir vm)
unclean() { sql vm "SELECT relpages - all_visible FROM pg_visibility_map_summary('t'), (SELECT relpages FROM pg_class WHERE relname = 't') c"; }
pages() { sql vm "SELECT relpages FROM pg_class WHERE relname = 't'"; }
# every thirty-third row changed, in ten transactions
churn() { for b in $(seq 0 9); do sql vm "UPDATE t SET k = k + 1 WHERE id % 33 = $1 AND id % 10 = $b" >/dev/null; done; }

churn 1
sql vm "VACUUM t" >/dev/null
check "no old snapshot: every page is all-visible after one VACUUM" test "$(unclean)" == 0

{ echo "BEGIN ISOLATION LEVEL REPEATABLE READ;"; echo "SELECT count(*) FROM t;"; echo "\\! touch '$D/locked'; while [ ! -e '$D/release' ]; do sleep 0.05; done"; echo "COMMIT;"; } | sql vm >/dev/null &
hp=$!
until [[ -e $D/locked ]]; do sleep 0.05; done
churn 2
sql vm "VACUUM t" >/dev/null
check "a snapshot older than the churn: the pages it touched stay out of the visibility map" test "$(unclean)" -gt $(( $(pages) / 2 ))
touch "$D/release"
wait "$hp"
sql vm "VACUUM t" >/dev/null
check "the snapshot ended: one more VACUUM sets them" test "$(unclean)" == 0
check "the answers are the heap's" test "$(sql vm "SET sift.enable_agg = on" "SELECT sum(k) FROM t")" == "$(sql vm "SET sift.enable_agg = off" "SELECT sum(k) FROM t")"
finish
