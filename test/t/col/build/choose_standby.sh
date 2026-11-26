#!/usr/bin/env bash
# The build chooser's three sources of rows replay on a physical hot standby: a CREATE INDEX of a key_cut index that reads the heap through a btree (no temporary file: the pg_stat_database
# temp_bytes of the cluster do not move), a REINDEX that takes the rows of the index it replaces (no heap block), and the same CREATE INDEX with sift.col_build_source = heap (the key ranges'
# files) give the same index on the standby: it verifies against the heap there and answers as the primary and the heap do.
. "${SIFT_REPO:?}/test/t/lib/common.bash"
plan 9

ROWS=$(knob SIFT_BCS_ROWS 60000 30000)
# Format 1: the build chooser of col/v1/build/choose.c (btree, index or heap rows, sift.col_build_source); Format 2 reads the heap
node_init primary 2 "sift.test_create_format1 = on
sift.col_build_source = auto
wal_level = replica
max_wal_senders = 4
wal_keep_size = $(knob SIFT_WAL_KEEP 1GB 256MB)
shared_buffers = 64MB
autovacuum = off"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE orders (id int, tenant_id int, created_at int, amount numeric(10,2), pad text) WITH (autovacuum_enabled = off)" \
	"INSERT INTO orders SELECT g, g % 30, g, g % 997, md5(g::text) || md5((g * 7)::text) FROM generate_series(1, $ROWS) g" \
	"CREATE INDEX orders_created_at ON orders (created_at)" \
	"VACUUM (FREEZE, ANALYZE) orders" >/dev/null || bail "setup failed"
node_clone primary standby 3 "hot_standby_feedback = on"
node_start standby || bail "standby start failed"

SETTINGS=("SET sift.async_maintenance = off" "SET max_parallel_workers_per_gather = 0" "SET sift.test_col_segment_kb = 1500" "SET sift.test_col_unit_rows = 1200" "SET maintenance_work_mem = '512MB'")
GROUPS_Q="SELECT coalesce(string_agg(tenant_id || ':' || n || ':' || s, ',' ORDER BY tenant_id), '') FROM (SELECT tenant_id, count(*) AS n, sum(amount) AS s FROM orders GROUP BY tenant_id) x"
TEMP="SELECT temp_bytes FROM pg_stat_database WHERE datname = current_database()"
FLUSH="SELECT pg_stat_force_next_flush()" ; CLEAR="SELECT pg_stat_clear_snapshot()"
answer() { sql "$1" "SET sift.enable_agg = $2" "SET max_parallel_workers_per_gather = 0" "$GROUPS_Q" | tail -1; }
verified() { tap_verified "$1" orders_idx; }
# build STATEMENT...: the statements in one session; BUILT = the source the last one took its rows from (0 heap, 1 btree, 2 old index), TEMPDELTA = the temp_bytes it added to the database
build() {
	local out
	out=$(sql primary "${SETTINGS[@]}" "$FLUSH" "$CLEAR" "$TEMP" "$@" "$FLUSH" "$CLEAR" "$TEMP" "SHOW sift.test_col_build_source_used" | grep -E '^[0-9]+$')
	BUILT=$(tail -n 1 <<<"$out")
	TEMPDELTA=$(($(tail -n 2 <<<"$out" | head -n 1) - $(head -n 1 <<<"$out")))
}
same_everywhere() {
	catchup primary standby || return 1
	local heap
	heap=$(answer primary off)
	[[ -n $heap && $(answer primary on) == "$heap" && $(answer standby on) == "$heap" && $(answer standby off) == "$heap" ]] && verified primary && verified standby
}
CREATE="CREATE INDEX orders_idx ON orders USING sift_col (id, tenant_id, created_at, amount) WITH (key_cut = 'created_at')"

build "$CREATE"
check "CREATE INDEX with a btree on the key reads the heap through it" test "$BUILT" = 1
check "... and writes no temporary file (pg_stat_database temp_bytes does not move)" test "$TEMPDELTA" = 0
check "... the index verifies on the standby and answers as the primary and the heap" same_everywhere

build "REINDEX INDEX orders_idx"
check "REINDEX takes the rows of the index it replaces, with no temporary file" test "$BUILT" = 2 -a "$TEMPDELTA" = 0
check "... the index verifies on the standby and answers as the primary and the heap" same_everywhere

sql primary "DROP INDEX orders_idx" >/dev/null
build "SET sift.col_build_source = heap" "$CREATE"
check "sift.col_build_source = heap reads the heap and writes the key ranges' files" test "$BUILT" = 0 -a "$TEMPDELTA" -gt 0
check "... the index verifies on the standby and answers as the primary and the heap" same_everywhere

sql primary "UPDATE orders SET amount = amount + 1 WHERE id % 7 = 0" "DELETE FROM orders WHERE id % 11 = 0" >/dev/null
build "REINDEX INDEX orders_idx"
check "REINDEX after updates and deletes takes the rows of the index it replaces" test "$BUILT" = 2
check "... the index verifies on the standby and answers as the primary and the heap" same_everywhere
finish
