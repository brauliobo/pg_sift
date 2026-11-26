#!/usr/bin/env bash
# The tiering of a sift_table moves the cold parts of a partition to the object store. A partition with a column delta chain is left alone whatever column the chain patches: an UPDATE of a column
# that is not the time column of 100 rows of an old part is a column delta, a snapshot older than the UPDATE holds the chain unfolded while VACUUM runs the tiering, and the rows read back as updated.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 3
node_init tc 0 "autovacuum = off
sift.async_maintenance = off
sift.test_create_format1 = off"
node_start tc || bail "start failed"
TIER=$(dir tc)/tier
mkdir -p "$TIER"
sql tc "CREATE EXTENSION pg_sift" "CREATE TABLE ev (id int8 PRIMARY KEY, ts timestamp NOT NULL, v int NOT NULL, pad text NOT NULL) USING sift_table" \
	"INSERT INTO ev SELECT i, timestamp '2020-01-01' + i * interval '1 hour', i % 100, md5(i::text) FROM generate_series(1, 5000) i" \
	"VACUUM (FREEZE) ev" \
	"INSERT INTO ev SELECT 5000 + i, localtimestamp - i * interval '1 minute', i % 100, md5((5000 + i)::text) FROM generate_series(1, 5000) i" \
	"VACUUM (FREEZE) ev" \
	"SELECT sift.set_tier_policy('ev', 'file://$TIER', '45 days')" >/dev/null || bail "setup failed"

PGAPPNAME=holder sql tc "BEGIN ISOLATION LEVEL REPEATABLE READ" "SELECT count(*) FROM ev" "SELECT pg_sleep(60)" >/dev/null 2>&1 &
holder=$!
until [[ $(sql tc "SELECT count(*) FROM pg_stat_activity WHERE application_name = 'holder' AND state = 'active' AND query LIKE '%pg_sleep%'") == 1 ]]; do :; done
sql tc "UPDATE ev SET v = v + 1000 WHERE id <= 100" >/dev/null
out=$(sql tc "SELECT dead_rows FROM sift.space('ev')")
check "the update of a column that is not the time column of 100 rows is a column delta: no row was deleted" test "$out" == 0

sql tc "VACUUM ev" >/dev/null
out=$(sql tc "SELECT count(*) FROM sift.tier_status")
check "no part of the partition with the chain was moved to the object store" test "$out" == 0
out=$(sql tc "SELECT count(*) FROM ev WHERE id <= 100 AND v >= 1000")
check "the 100 rows read back as updated" test "$out" == 100

sql tc "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE application_name = 'holder'" >/dev/null
wait "$holder" 2>/dev/null
finish
