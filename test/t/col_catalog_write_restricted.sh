#!/usr/bin/env bash
# tap: pooled
# sift never writes a catalog row where a lazy VACUUM or a parallel worker is (core/restrict.h): the writers of core/restrict.c refuse there, as the VACUUM runs them (the process flag
# PROC_IN_VACUUM set for the call) and in a worker of a parallel query, and write in a session, in a CREATE INDEX CONCURRENTLY and in a REINDEX, where a build chooses the options of the
# index. An assert build stops at the guard (Assert), so the test needs a release build of the library.
. "$(dirname "$0")/lib.bash"

node_init cw 50 "autovacuum = off
sift.async_maintenance = off"
node_start cw || bail "start failed"
D=$(dir cw)
sql cw "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" "CREATE TABLE a (id int)" "CREATE TABLE b (id int)" >/dev/null || bail "setup failed"
if [[ $(sql cw "SHOW debug_assertions") == on ]]; then echo '1..0 # SKIP an assert build stops at the guard'; exit 0; fi
plan 15

MSG='writes a catalog row in a VACUUM or a parallel operation'
refused() { sql cw "SELECT sift._test_catalog_write('$1', true, 'a', 'b')" 2>&1 | grep -c "$MSG"; }
for kind in update insert delete depend undepend trigger drop; do
	check "a VACUUM cannot write the catalog: $kind" test "$(refused $kind)" == 1
done
check "a parallel worker cannot write the catalog" test "$(sql cw "SET debug_parallel_query = on" "SELECT sift._test_catalog_write('update', false, 'a', 'b')" 2>&1 | grep -c "$MSG")" == 1
check "a session can update a catalog row" test "$(sql cw "SELECT sift._test_catalog_write('update', false, 'a', 'b')" 2>&1 | grep -c ERROR)" == 0
deps() { sql cw "SELECT count(*) FROM pg_depend WHERE classid = 'pg_class'::regclass AND objid = 'a'::regclass AND refobjid = 'b'::regclass AND deptype = 'n'"; }
sql cw "SELECT sift._test_catalog_write('depend', false, 'a', 'b')" >/dev/null 2>&1
check "... and a dependency" test "$(deps)" == 1
sql cw "SELECT sift._test_catalog_write('undepend', false, 'a', 'b')" >/dev/null 2>&1
check "... and delete it" test "$(deps)" == 0
check "the flag is off again after a refused call" test "$(sql cw "SELECT sift._test_catalog_write('update', false, 'a', 'b')" 2>&1 | grep -c ERROR)" == 0

# a build chooses the options of the index, also beside the writers and again for a REINDEX
sql cw "CREATE TABLE rc (id int PRIMARY KEY, tenant_id int, foreign_id int, counter_a int, updated_at timestamp, pad text) WITH (autovacuum_enabled = off)" \
	"INSERT INTO rc SELECT i, i / 150, 1 + i % 12, hashint4(i) & 1048575, TIMESTAMP '2026-01-01' + (hashint4(i + 7) & 16777215) * INTERVAL '1 second', repeat('x', 100) FROM generate_series(1, 60000) i" \
	"VACUUM ANALYZE rc" >/dev/null || bail "table failed"
options() { sql cw "SELECT reloptions::text FROM pg_class WHERE relname = 'rc_ix'"; }
sql cw "CREATE INDEX CONCURRENTLY rc_ix ON rc USING sift_col (id, tenant_id, foreign_id, counter_a, updated_at) WITH (key_cut = 'auto', key_columns = 'auto', point_tid_columns = 'auto')" >/dev/null 2>&1
check "CREATE INDEX CONCURRENTLY chooses the options of the index" test "$(options)" != "{key_cut=auto,key_columns=auto,point_tid_columns=auto}"
sql cw "ALTER INDEX rc_ix SET (key_cut = 'auto', key_columns = 'auto', point_tid_columns = 'auto')" >/dev/null
sql cw "REINDEX INDEX CONCURRENTLY rc_ix" >/dev/null 2>&1
check "REINDEX CONCURRENTLY chooses them again" test "$(options)" != "{key_cut=auto,key_columns=auto,point_tid_columns=auto}"
check "the log has no error but the refusals" test "$(grep ERROR "$D/postgres.log" | grep -vc "$MSG")" == 0
finish
