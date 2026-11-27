#!/usr/bin/env bash
# A COPY into a Format 1 sift_table that no publication names, while a logical slot of the database decodes every table (test_decoding): the load keeps the write buffers, so the slot
# gets every row of the COPY, as it gets the rows of an INSERT.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

[[ -n ${SIFT_TESTING:-} ]] || { echo "1..0 # SKIP Format 1 tables are made by the testing library only"; exit 0; }
[[ -e $(pg_config --pkglibdir)/test_decoding.so ]] || { echo "1..0 # SKIP this PostgreSQL has no test_decoding"; exit 0; }
plan 3
node_init vb 2 "wal_level = logical
shared_preload_libraries = 'pg_sift'
max_replication_slots = 4
max_wal_senders = 4
autovacuum = off
sift.async_maintenance = off
sift.test_create_format1 = on"
node_start vb || bail "start failed"
sql vb "CREATE EXTENSION pg_sift_test CASCADE" "CREATE TABLE t (id int, v text) USING sift_table" \
	"SELECT 1 FROM pg_create_logical_replication_slot('td', 'test_decoding')" >/dev/null || bail "setup failed"
check "the table is Format 1" test "$(sql vb "SELECT (sift.index_format('t')).major")" == 1
sql vb "COPY t (id) FROM PROGRAM 'seq 1 100'" >/dev/null || bail "the COPY failed"
check "the slot gets every row of the COPY" test "$(sql vb "SELECT count(*) FROM pg_logical_slot_get_changes('td', NULL, NULL) WHERE data LIKE 'table public.t: INSERT:%'")" == 100
check "no backend died" test "$(grep -ac 'terminated by signal' "$(dir vb)/postgres.log")" -eq 0
finish
