#!/usr/bin/env bash
# sift.drop_parts_before of frozen parts cancels no query of a hot standby that does not wait for replay (max_standby_streaming_delay = 0): no row it drops was written or deleted by a
# transaction a standby snapshot may still see in progress, so the drop logs no snapshot conflict. The query reads another table of the database, whose snapshot a conflict would cancel. Needs a preloading server (the map of a sift_table is logged by a resource manager).
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "${SIFT_REPO:?}/test/t/lib/common.bash"
plan 3

p=ttl_primary
s=ttl_standby
node_init "$p" 2 "shared_preload_libraries = 'pg_sift'
wal_level = replica
max_wal_senders = 4
sift.test_create_format1 = off
autovacuum = off
sift.async_maintenance = off"
node_start "$p" || bail "primary start failed"
sql "$p" "CREATE EXTENSION pg_sift" \
	"CREATE TABLE d (id int, at timestamptz NOT NULL) USING sift_table WITH (autovacuum_enabled = false, vacuum_truncate = false)" \
	"CREATE TABLE e AS SELECT g FROM generate_series(1, 10) g" >/dev/null || bail "setup failed"
for day in 0 1 2 3 4 5; do
	sql "$p" "INSERT INTO d SELECT $day * 1000 + g, timestamptz '2030-01-01' + $day * interval '1 day' + g * interval '1 second' FROM generate_series(1, 1000) g" "VACUUM (FREEZE) d" >/dev/null ||
		bail "load failed"
done
node_clone "$p" "$s" 3 "max_standby_streaming_delay = 0"
node_start "$s" || bail "standby start failed"
catchup "$p" "$s" || bail "the standby did not catch up"

D=$(dir "$s")
sess_open "$s" reader
sess_say reader "BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM e; \\echo @@read"
wait_for "grep -q '@@read' '$D/reader.out'" 30 || bail "the standby snapshot was not taken"
check "the drop takes the frozen parts of the first days" test "$(sql "$p" "SELECT rows_dropped FROM sift.drop_parts_before('d', timestamptz '2030-01-04')")" == 3000
catchup "$p" "$s" || bail "the standby did not replay the drop"
sess_say reader "SELECT count(*) FROM e; COMMIT; \\echo @@done"
wait_for "grep -q '@@done' '$D/reader.out'" 30
sess_close reader
out=$(cat "$D/reader.out")
check "the standby query that read before the drop is not cancelled" test "$(grep -c 'conflict with recovery' <<<"$out")" == 0
check "a new standby query sees the rows the drop left" test "$(sql "$s" "SELECT count(*) FROM d")" == 3000
finish
