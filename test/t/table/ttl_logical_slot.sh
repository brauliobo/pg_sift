#!/usr/bin/env bash
# The TTL of a sift_table no publication names does not drop its parts while a logical slot of the database decodes every table (test_decoding, wal2json): the slot would never get the
# deletes of those rows. The VACUUM warns and keeps them; once the slot is gone it drops the expired part.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

[[ -e $(pg_config --pkglibdir)/test_decoding.so ]] || { echo "1..0 # SKIP this PostgreSQL has no test_decoding"; exit 0; }
plan 3
node_init ts 2 "wal_level = logical
shared_preload_libraries = 'pg_sift'
max_replication_slots = 4
max_wal_senders = 4
autovacuum = off
sift.async_maintenance = off
${SIFT_TESTING:+sift.test_create_format1 = off}"
node_start ts || bail "start failed"
EXT=pg_sift
[[ -z ${SIFT_TESTING:-} ]] || EXT="pg_sift_test CASCADE"
sql ts "CREATE EXTENSION $EXT" "CREATE TABLE ev (id int8, ts timestamp NOT NULL, pad text) USING sift_table" \
	"INSERT INTO ev SELECT i, timestamp '2020-01-01' + i * interval '1 hour', md5(i::text) FROM generate_series(1, 2000) i" "VACUUM (FREEZE) ev" \
	"INSERT INTO ev SELECT 2000 + i, localtimestamp - i * interval '1 minute', md5(i::text) FROM generate_series(1, 2000) i" "VACUUM (FREEZE) ev" \
	"SELECT sift.set_ttl('ev', 'ts', interval '30 days')" \
	"SELECT 1 FROM pg_create_logical_replication_slot('td', 'test_decoding')" >/dev/null || bail "setup failed"

out=$(sql ts "VACUUM ev" 2>&1)
check "the VACUUM warns that the TTL does not run" grep -q 'its TTL does not run' <<<"$out"
check "and drops no row while the slot decodes every table" test "$(sql ts "SELECT count(*) FROM ev")" == 4000
sql ts "SELECT pg_drop_replication_slot('td')" "VACUUM ev" >/dev/null
check "without the slot the expired part is dropped" test "$(sql ts "SELECT count(*) FROM ev")" == 2000
finish
