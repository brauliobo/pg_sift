#!/usr/bin/env bash
# Logical replication of a table whose sift_col indexes share a value store, under a publication of all tables: the store is a materialized view that is never
# populated, which no publication lists, so a subscriber that has no store (no sharing index, or none of that name) syncs, refreshes its publication and follows
# the changes, and creating or dropping a sharing index on the publisher never adds a relation to the subscription. A subscriber with sharing indexes of its own
# has vstores of its own. The rows arrive and the indexes of the subscriber verify and answer as the heap.
. "$(dirname "$0")/lib.bash"

plan 11
node_init p 56 "wal_level = logical
max_wal_senders = 5
max_replication_slots = 5
autovacuum = off
max_logical_replication_workers = 4
max_worker_processes = 16"
node_start p || bail "publisher start failed"
node_init s 57 "wal_level = logical
max_replication_slots = 5
autovacuum = off
max_logical_replication_workers = 4
max_worker_processes = 16"
node_start s || bail "subscriber start failed"
node_init n 58 "wal_level = logical
max_replication_slots = 5
autovacuum = off
max_logical_replication_workers = 4
max_worker_processes = 16"
node_start n || bail "second subscriber start failed"
export PGOPTIONS="-c client_min_messages=warning"
ROWS=$(knob SIFT_VSLOGICAL_ROWS 4000 2000)

for node in p s n; do
	sql $node "CREATE EXTENSION pg_sift" "CREATE TABLE lv (id int PRIMARY KEY, uid uuid, url text)" >/dev/null || bail "ddl failed on $node"
done
SHARE="SET sift.test_vstore_columns = '*'"
for node in p s; do
	sql $node "$SHARE" "CREATE INDEX lv_a ON lv USING sift_col (id, uid, url)" "CREATE INDEX lv_b ON lv USING sift_col (uid, url, id)" >/dev/null || bail "indexes failed on $node"
done
sql p "INSERT INTO lv SELECT i, md5((i % 500)::text)::uuid, 'https://example.com/' || md5((i % 700)::text) FROM generate_series(1, $ROWS) i" "ANALYZE lv" >/dev/null
vstores() { sql "$1" "SELECT count(*) FROM sift.col_value_store_status"; }
check "the publisher has a store for each string column, the subscriber with indexes too, the other none" test "$(vstores p)|$(vstores s)|$(vstores n)" == "2|2|0"

sql p "CREATE PUBLICATION pall FOR ALL TABLES" >/dev/null || bail "publications failed"
sql s "CREATE SUBSCRIPTION s1 CONNECTION '$(conninfo p)' PUBLICATION pall" >/dev/null || bail "subscribe failed"
sql n "CREATE SUBSCRIPTION s2 CONNECTION '$(conninfo p)' PUBLICATION pall" >/dev/null || bail "second subscribe failed"
digest() { sql "$1" "SELECT count(*) || ':' || coalesce(sum(id), 0) || ':' || md5(string_agg(url, ',' ORDER BY id)) FROM lv"; }
wait_synced() { for _ in $(seq 400); do [[ $(digest p) == "$(digest "$1")" ]] && return 0; sleep 0.2; done; return 1; }
check "the subscriber with vstores has the rows" wait_synced s
check "the subscriber without vstores has the rows" wait_synced n

sql p "INSERT INTO lv SELECT i, md5((i % 500)::text)::uuid, 'https://example.com/n' || md5((i % 300)::text) FROM generate_series($((ROWS + 1)), $((ROWS + 600))) i" \
	"UPDATE lv SET url = 'https://example.com/u' || md5(id::text) WHERE id % 13 = 0" "DELETE FROM lv WHERE id % 29 = 0" >/dev/null
check "the subscriber with vstores follows the changes" wait_synced s
check "the subscriber without vstores follows the changes" wait_synced n

check "no publication lists a store" test "$(sql p "SELECT count(*) FROM pg_publication_tables WHERE tablename LIKE 'lv%sift_values'")" == 0
sql p "DROP INDEX lv_b" "CREATE INDEX lv_c ON lv USING sift_col (uid, id, url)" "CREATE INDEX lv_d ON lv USING sift_col (url, uid, id)" >/dev/null
refresh() { sql n "ALTER SUBSCRIPTION s2 REFRESH PUBLICATION" >/dev/null; }
check "the subscription refreshes after a store is dropped and made again" refresh
check "the subscriber without vstores still has the rows" wait_synced n
verified() { [[ -z $(sql s "SELECT sift.col_verify('lv_a', true), sift.col_verify('lv_b', true)" 2>&1 | tr -d '| \n') ]]; }
check "the indexes of the subscriber verify" verified
agree() { [[ $(sql s "SET sift.enable_agg = on" "SELECT count(*) || ':' || count(DISTINCT url) || ':' || count(DISTINCT uid) FROM lv" | tail -n 1) == $(sql s "SET sift.enable_agg = off" "SELECT count(*) || ':' || count(DISTINCT url) || ':' || count(DISTINCT uid) FROM lv" | tail -n 1) ]]; }
check "the indexes of the subscriber answer as the heap" agree
check "the subscribers are still replicating after a VACUUM of the table" test "$(sql s "VACUUM lv" "SELECT 1")" == 1
finish
