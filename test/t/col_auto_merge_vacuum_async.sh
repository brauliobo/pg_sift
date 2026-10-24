#!/usr/bin/env bash
# The VACUUM the worker runs (sift.col_auto_merge) with synchronous_commit = off. The commit record of the churn is not on disk when the VACUUM starts
# (the WAL writer waits wal_writer_delay), and PostgreSQL sets no hint bit for a transaction whose commit record is not flushed: the pages that hold only
# its new row versions stay out of the visibility map and the scans that follow check their rows in the heap, until another VACUUM. The worker flushes
# the WAL first: right after its VACUUM every page is all-visible.
. "$(dirname "$0")/lib.bash"

plan 3
node_init as 25 "autovacuum = off
synchronous_commit = off
wal_writer_delay = 10s
wal_writer_flush_after = 1GB
sift.maintenance_naptime = 1000"
node_start as || bail "start failed"
sql as "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_visibility" "CREATE TABLE facts (id int, key int, kind text, amount numeric(10,2), pad text)" \
	"INSERT INTO facts SELECT g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 100) FROM generate_series(1, 100000) g" \
	"CREATE INDEX facts_idx ON facts USING sift_col (id, key, kind, amount)" "VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) facts" "SELECT pg_logical_emit_message(true, 'sift', 'flush')" >/dev/null || bail "setup failed"

vacuumed() { sql as "SELECT coalesce(extract(epoch FROM last_vacuum)::text, '-') FROM pg_stat_user_tables WHERE relname = 'facts'"; }
vetted() { sql as "SET max_parallel_workers_per_gather = 0" "EXPLAIN (ANALYZE, COSTS OFF) SELECT kind, count(*), sum(amount) FROM facts GROUP BY kind" | sed -n 's/.*Rows Vetted By Heap: //p'; }
# the pages that are not all-visible
unclean() { sql as "SELECT relpages - all_visible FROM pg_visibility_map_summary('facts'), (SELECT relpages FROM pg_class WHERE relname = 'facts') c"; }
await_vacuum() {
	for _ in $(seq 100); do
		vetted >/dev/null
		[[ $(vacuumed) == "$1" ]] || return 0
		sleep 0.2
	done
	return 1
}

T0=$(vacuumed)
sql as "ALTER DATABASE postgres SET sift.col_auto_merge = on" "UPDATE facts SET amount = amount + 1 WHERE id % 20 = 1" >/dev/null
check "the churn is spread over the pages (the scan checks the rows in the heap)" test "$(vetted)" -gt 50000
check "the worker vacuums the table" await_vacuum "$T0"
diag "pages not all-visible after the first VACUUM of the worker: $(unclean) of $(sql as "SELECT relpages FROM pg_class WHERE relname = 'facts'")"
check "right after its VACUUM every page is all-visible although the commit was not flushed" test "$(unclean)" == 0
finish
