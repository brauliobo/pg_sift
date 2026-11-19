#!/usr/bin/env bash
# The second pass of CREATE INDEX CONCURRENTLY of a scope index sorts the rows it replicates through temporary files, so the index keeps only the segments it ends with (the blocks of intermediate runs, and of the
# scope segments of the first pass that the second replaces, stayed in the file until a VACUUM), and no temporary file outlives the statement, whether it ends, fails, is cancelled or its backend is terminated.
. "$(dirname "$0")/lib.bash"
plan 7

node_init ct 57 "autovacuum = off
sift.col_auto_merge = off
temp_file_limit = -1"
node_start ct || bail "start failed"
sql ct "CREATE EXTENSION pg_sift" \
	"CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id))" \
	"INSERT INTO tenant_scope SELECT m, 1 + (m * 31 + j * 13) % 40 FROM generate_series(1, 20000) m, generate_series(0, 2) j" \
	"CREATE TABLE events (id bigint PRIMARY KEY, dim_a_id int, created_at timestamptz, kind int NOT NULL, value_num int NOT NULL, region text, name text)" \
	"INSERT INTO events SELECT i, 1 + ((i::bigint * 7919) % 20000)::int, timestamptz '2026-01-01' + (i % 100000) * interval '1 minute', i % 7, i % 1000, 'r' || i % 40, 'name-' || i % 5000 FROM generate_series(1, 400000) i" \
	"ANALYZE" >/dev/null || bail "setup failed"

COLS="id, dim_a_id, created_at, kind, value_num, region, name"
SCOPE="scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)'"
MEM="SET maintenance_work_mem = '4MB'"
tmpfiles() { sql ct "SELECT count(*) FROM pg_ls_tmpdir()" | tail -n 1; }
size() { sql ct "SELECT pg_relation_size('$1')" | tail -n 1; }
cic() { sql ct "$MEM" "$@" "CREATE INDEX CONCURRENTLY ev_cc ON events USING sift_col ($COLS) WITH ($SCOPE)"; }
bg_cic() { PGOPTIONS="-c maintenance_work_mem=4MB" psql -X -q -h "$(dir ct)/sock" -p "$(port ct)" -U postgres -d postgres -c "CREATE INDEX CONCURRENTLY ev_cc ON events USING sift_col ($COLS) WITH ($SCOPE)" >/dev/null 2>&1 & }
spilling() { [[ $(tmpfiles) -gt 0 ]]; }
settled() { [[ $(sql ct "SELECT count(*) FROM pg_stat_activity WHERE query LIKE 'CREATE INDEX CONCURRENTLY%' AND pid <> pg_backend_pid()" | tail -n 1) == 0 ]]; }
backend() { sql ct "SELECT pid FROM pg_stat_activity WHERE query LIKE 'CREATE INDEX CONCURRENTLY%' AND pid <> pg_backend_pid()" | tail -n 1; }

cic >/dev/null
live=$(sql ct "SELECT sum(bytes) FROM sift.col_segments('ev_cc')" | tail -n 1)
file=$(size ev_cc)
out="file $file live segments $live"
check "the file of the concurrent build holds the segments it ends with (within 10%)" test "$file" -le $((live * 11 / 10))
check "no temporary file is left after it" test "$(tmpfiles)" == 0
sql ct "DROP INDEX ev_cc" >/dev/null

out=$(cic "SET temp_file_limit = '8MB'" 2>&1)
check "a temporary disk too small for the sort fails the statement" grep -qE 'temporary file size exceeds "?temp_file_limit' <<<"$out"
check "... and no temporary file is left" test "$(tmpfiles)" == 0
sql ct "DROP INDEX IF EXISTS ev_cc" >/dev/null

bg_cic
wait_for spilling 120 || bail "the build never spilled"
sql ct "SELECT pg_cancel_backend($(backend))" >/dev/null
wait_for settled 120
sql ct "DROP INDEX IF EXISTS ev_cc" >/dev/null
check "a cancelled build leaves no temporary file" test "$(tmpfiles)" == 0

bg_cic
wait_for spilling 120 || bail "the build never spilled"
sql ct "SELECT pg_terminate_backend($(backend))" >/dev/null
wait_for settled 120
check "a terminated build leaves no temporary file" test "$(tmpfiles)" == 0
sql ct "DROP INDEX IF EXISTS ev_cc" >/dev/null
cic >/dev/null
check "the next concurrent build is sound" test "$(sql ct "SELECT count(*) FROM sift.col_scope_verify('ev_cc', 100)" | tail -n 1)" == 0

finish
