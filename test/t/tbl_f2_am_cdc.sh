#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# A sift_table (Format 2.0) at the edges of the table access method: ALTER TABLE .. SET TABLESPACE copies the file with the staged deletes of the transaction in it (and an UNLOGGED table keeps its init
# fork), a published table writes a change record that carries the value of a TOAST column of another table, VACUUM gives relminmxid
# the oldest MultiXactId a row lock still holds, and a VACUUM that has a cost delay pays it (pg_stat_progress_vacuum.delay_time).
. "$(dirname "$0")/lib.bash"

plan 7
node_init am 0 "wal_level = logical
shared_preload_libraries = 'pg_sift'
max_replication_slots = 4
autovacuum = off
sift.async_maintenance = off
${SIFT_TESTING:+sift.test_create_format1 = off}"
node_start am || bail "start failed"
D=$(dir am)
EXT=pg_sift
[[ -z ${SIFT_TESTING:-} ]] || EXT="pg_sift_test CASCADE"
mkdir -p "$D/ts1"
sql am "CREATE EXTENSION $EXT" "CREATE TABLESPACE ts1 LOCATION '$D/ts1'" >/dev/null || bail "setup failed"

sql am "CREATE TABLE moved (id int PRIMARY KEY, v int) USING sift_table" "INSERT INTO moved VALUES (1, 10), (2, 20)" \
	"BEGIN" "INSERT INTO moved VALUES (3, 30)" "DELETE FROM moved WHERE id = 1" "ALTER TABLE moved SET TABLESPACE ts1" "COMMIT" >/dev/null
check "SET TABLESPACE in the transaction that wrote and deleted rows keeps both" test "$(sql am "SELECT string_agg(id || '=' || v, ',' ORDER BY id) FROM moved")" == "2=20,3=30"
check "sift.check finds nothing in the moved table" test "$(sql am "SELECT count(*) FROM sift.check('moved'::regclass)")" == 0

sql am "CREATE UNLOGGED TABLE lost (id int, v int) USING sift_table" "INSERT INTO lost SELECT g, g FROM generate_series(1, 100) g" "ALTER TABLE lost SET TABLESPACE ts1" >/dev/null
check "an UNLOGGED table that moved keeps its init fork" test "$(sql am "SELECT pg_relation_size('lost'::regclass, 'init') > 0")" == t
crash am
node_start am || bail "restart failed"
check "the UNLOGGED table that moved is empty after the crash and takes rows" test "$(sql am "INSERT INTO lost VALUES (1, 1)" "SELECT count(*) FROM lost" | tail -n 1)" == 1

sql am "CREATE TABLE heap_src (id int PRIMARY KEY, big text)" "ALTER TABLE heap_src ALTER COLUMN big SET STORAGE EXTERNAL" \
	"INSERT INTO heap_src VALUES (1, (SELECT string_agg(md5(g::text), '') FROM generate_series(1, 4000) g))" \
	"CREATE TABLE pub_t (id int PRIMARY KEY, big text, pad text) USING sift_table" "CREATE PUBLICATION pb FOR TABLE pub_t" \
	"SELECT pg_create_logical_replication_slot('cdc', 'pgoutput')" >/dev/null || bail "publication setup failed"
sql am "INSERT INTO pub_t SELECT id, big, 'p' FROM heap_src" "DROP TABLE heap_src" >/dev/null 2>&1
out=$(sql am "SELECT max(length(data)) FROM pg_logical_slot_peek_binary_changes('cdc', NULL, NULL, 'proto_version', '1', 'publication_names', 'pb')" 2>&1)
check "a row inserted from a TOASTed value is decoded with the value, not its pointer" test "${out:-0}" -gt 128000
sql am "SELECT count(*) FROM pg_logical_slot_get_binary_changes('cdc', NULL, NULL, 'proto_version', '1', 'publication_names', 'pb')" >/dev/null 2>&1

sql am "CREATE TABLE locked (id int PRIMARY KEY, v int) USING sift_table" "INSERT INTO locked SELECT g, 0 FROM generate_series(1, 10) g" >/dev/null
rm -f "$D"/held.* "$D/release"
holders=()
for k in 1 2 3 4 5 6 7 8 9 10; do
	{ echo "BEGIN;"; echo "SELECT id FROM locked WHERE id = 1 FOR KEY SHARE;"; echo "\\! touch '$D/held.$k'"
	  echo "\\! for i in \$(seq 600); do [ -e '$D/release' ] && break; sleep 0.05; done"; echo "COMMIT;"; } | sql am >"$D/holder.$k.out" 2>&1 &
	holders+=($!)
done
all_held() { [[ $(ls "$D"/held.* 2>/dev/null | wc -l) -eq 10 ]]; }
wait_for all_held 60 || bail "the ten holders did not take the row"
sql am "VACUUM locked" >/dev/null
check "VACUUM keeps relminmxid at the newest MultiXactId a row lock still holds" test "$(sql am "CHECKPOINT" "SELECT (SELECT relminmxid::text::bigint FROM pg_class WHERE oid = 'locked'::regclass) < next_multixact_id::text::bigint FROM pg_control_checkpoint()" | tail -n 1)" == t
touch "$D/release"
wait "${holders[@]}"

sql am "CREATE TABLE slow (id int, a int, b text) USING sift_table" "INSERT INTO slow SELECT g, g, repeat('x', 40) FROM generate_series(1, 60000) g" \
	"DELETE FROM slow WHERE id % 3 = 0" >/dev/null
sql am "SET vacuum_cost_delay = 2" "SET vacuum_cost_limit = 1" "SET track_cost_delay_timing = on" "VACUUM slow" >/dev/null 2>&1 &
vac=$!
delayed() { [[ $(sql am "SELECT coalesce(max(delay_time), 0) > 0 FROM pg_stat_progress_vacuum") == t ]]; }
check "a VACUUM with a cost delay reports the time it slept" wait_for delayed 30
kill "$vac" 2>/dev/null; wait "$vac" 2>/dev/null
finish
