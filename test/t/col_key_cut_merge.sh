#!/usr/bin/env bash
# A key_cut build holds its rows in runs and writes one segment for each, and the directory has room for 121 of them: the build merges the runs as it writes them, so
# CREATE INDEX (serial and parallel), CREATE INDEX CONCURRENTLY, REINDEX and REINDEX CONCURRENTLY finish with the memory they have, however many runs the table makes.
# sift.test_col_segment_kb stands for a table too large for maintenance_work_mem: 8 runs of a superblock for this one, and room for 6 segments.
. "$(dirname "$0")/lib.bash"

plan 9
node_init km 33 "autovacuum = off"
node_start km || bail "start failed"
sql km "CREATE EXTENSION pg_sift CASCADE" >/dev/null || bail "pg_sift"
sql km "CREATE TABLE km (id int, cust int, d date, amount int, pad text) WITH (autovacuum_enabled = false, parallel_workers = 2)" \
	"INSERT INTO km SELECT g, (g * 7919) % 600, date '2026-01-01' + (g * 31) % 300, g % 100, repeat('x', 100) FROM generate_series(1, 100000) g" \
	"VACUUM (FREEZE, ANALYZE) km" >/dev/null || bail "setup failed"
DDL="ON km USING sift_col (id, cust, d, amount) WITH (key_cut = 'cust, d')"
small() { sql km "SET sift.test_col_segment_kb = 16" "SET sift.test_col_max_segments = 6" "SET max_parallel_maintenance_workers = 0" "$@"; }
good() { test "$(sql km "SELECT indisvalid FROM pg_index WHERE indexrelid = 'km_idx'::regclass")" == t &&
	test -z "$(sql km "SELECT sift.col_verify('km_idx', true)" 2>&1)" &&
	test "$(sql km "SELECT count(*) < ${1:-6} AND bool_and(format = 17) FROM sift.col_segments('km_idx') WHERE kind = 'immutable'")" == t; }

small "CREATE INDEX km_idx $DDL" >/dev/null
check "CREATE INDEX merges the runs: valid, verified, fewer segments than the runs, of format 17" good

sql km "DROP INDEX km_idx" >/dev/null
small "SET sift.test_col_max_segments = 3" "SET max_parallel_maintenance_workers = 2" "SET maintenance_work_mem = '256MB'" "SET min_parallel_table_scan_size = 0" "CREATE INDEX km_idx $DDL" >/dev/null
check "the participants of a parallel build merge their own runs" good 99
check "and a worker took part" test "$(sql km "SET sift.test_col_segment_kb = 16" "SET sift.test_col_max_segments = 3" "SET max_parallel_maintenance_workers = 2" "SET maintenance_work_mem = '256MB'" "SET min_parallel_table_scan_size = 0" "REINDEX INDEX km_idx" "SELECT current_setting('sift.test_col_workers')::int > 0")" == t

small "REINDEX INDEX km_idx" >/dev/null
check "REINDEX merges the runs" good
small "REINDEX INDEX CONCURRENTLY km_idx" >/dev/null
check "REINDEX CONCURRENTLY merges the runs" good
sql km "DROP INDEX km_idx" >/dev/null

small "CREATE INDEX CONCURRENTLY km_idx $DDL" >/dev/null
check "CREATE INDEX CONCURRENTLY merges the runs" good
check "and leaves no other index of the table" test "$(sql km "SELECT count(*) FROM pg_index WHERE indrelid = 'km'::regclass")" == 1
check "the merged segments hold every row once" test "$(sql km "SELECT sum(rows) FROM sift.col_segments('km_idx') WHERE kind = 'immutable'")" == "$(sql km 'SELECT count(*) FROM km')"
sql km "DROP INDEX km_idx" >/dev/null

small "CREATE INDEX km_auto ON km USING sift_col (id, cust, d, amount)" >/dev/null
check "an automatic cut is built the same way" test "$(sql km "SELECT sift.col_verify('km_auto', true)")" == ""
sql km "DROP INDEX km_auto" >/dev/null
finish
