#!/usr/bin/env bash
# A key_cut build holds its rows in runs and writes one segment for each, and the directory has room for 121 of them: a build whose runs cannot fit stops before it reads
# the table, with the maintenance_work_mem it needs. CREATE INDEX CONCURRENTLY leaves only the invalid index its HINT tells to drop; REINDEX keeps the index it had; the
# same index builds with the memory the HINT names. sift.test_col_segment_kb stands for a table too large for maintenance_work_mem.
. "$(dirname "$0")/lib.bash"

plan 13
node_init kr 33 "autovacuum = off"
node_start kr || bail "start failed"
sql kr "CREATE EXTENSION pg_sift CASCADE" >/dev/null || bail "pg_sift"
sql kr "CREATE TABLE kr (id int, cust int, d date, amount int, pad text) WITH (autovacuum_enabled = false)" \
	"INSERT INTO kr SELECT g, (g * 7919) % 600, date '2026-01-01' + (g * 31) % 300, g % 100, repeat('x', 100) FROM generate_series(1, 100000) g" \
	"VACUUM (FREEZE, ANALYZE) kr" >/dev/null || bail "setup failed"
DDL="ON kr USING sift_col (id, cust, d, amount) WITH (key_cut = 'cust, d')"
err() { sql kr "SET sift.test_col_segment_kb = 16" "$@" 2>&1 >/dev/null; }

out=$(err "CREATE INDEX CONCURRENTLY kr_idx $DDL")
check "CREATE INDEX CONCURRENTLY stops with the runs it would need" test "$(grep -c 'would need about [0-9]* segments, and the directory has room for 121' <<<"$out")" == 1
check "the HINT names maintenance_work_mem and the workers" test "$(grep -c "HINT:.*SET maintenance_work_mem = '[0-9]*[MG]B'; SET max_parallel_maintenance_workers = 0;" <<<"$out")" == 1
check "the HINT tells to drop the invalid index" test "$(grep -c 'leaves the invalid index "kr_idx": DROP INDEX CONCURRENTLY it first' <<<"$out")" == 1
check "only the invalid index is left" test "$(sql kr "SELECT string_agg(indexrelid::regclass || ':' || indisvalid, ',') FROM pg_index WHERE indrelid = 'kr'::regclass")" == "kr_idx:false"
sql kr "DROP INDEX CONCURRENTLY kr_idx" >/dev/null
check "dropping it leaves the table with no index" test "$(sql kr "SELECT count(*) FROM pg_index WHERE indrelid = 'kr'::regclass")" == 0

out=$(err "CREATE INDEX kr_plain $DDL")
check "CREATE INDEX stops the same way and leaves nothing, no CONCURRENTLY advice" test "$(grep -c 'would need about' <<<"$out")|$(grep -c 'DROP INDEX CONCURRENTLY' <<<"$out")|$(sql kr "SELECT count(*) FROM pg_index WHERE indrelid = 'kr'::regclass")" == "1|0|0"

sql kr "CREATE INDEX kr_idx $DDL" >/dev/null
check "the same index builds with the default memory" test "$(sql kr "SELECT indisvalid FROM pg_index WHERE indexrelid = 'kr_idx'::regclass")" == t
out=$(err "REINDEX INDEX kr_idx")
check "REINDEX stops before it reads the table" test "$(grep -c 'would need about' <<<"$out")" == 1
check "and the index it had is still valid" test "$(sql kr "SELECT indisvalid FROM pg_index WHERE indexrelid = 'kr_idx'::regclass")" == t
check "and verifies" test -z "$(sql kr "SELECT sift.col_verify('kr_idx', true)" 2>&1)"

sql kr "DROP INDEX kr_idx" >/dev/null
check "an automatic cut that does not fit is not chosen: the index is built without it" test "$(sql kr "SET sift.test_col_segment_kb = 16" "CREATE INDEX kr_auto ON kr USING sift_col (id, cust, d, amount)" "SELECT coalesce((SELECT o FROM pg_class c, unnest(c.reloptions) o WHERE c.oid = 'kr_auto'::regclass AND o ~ '^key_cut'), 'none')")" != "key_cut=auto: cust"
check "and it verifies" test -z "$(sql kr "SELECT sift.col_verify('kr_auto', true)" 2>&1)"
sql kr "DROP INDEX kr_auto" >/dev/null
check "the build with the memory the HINT names (default here) leaves the key-cut segments" test "$(sql kr "CREATE INDEX kr_idx $DDL" "SELECT min(format) FROM sift.col_segments('kr_idx')")" == 17
finish
