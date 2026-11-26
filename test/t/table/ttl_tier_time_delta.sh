#!/usr/bin/env bash
# tap: pooled
# The TTL and the tiering of a sift_table go by the time bounds of the parts of a partition. An UPDATE of the time column of a row of a published part is a column delta: the part keeps the
# bounds of the row it held, and the partition's chain says that they do not hold (the dirty time flag of its anchor). The old rows of a table that took such an UPDATE move to the present with the
# chain, so no maintenance may drop the part they are in as expired or move it to the object store as cold: 100 rows of an old part updated to now under a snapshot older than the UPDATE, which holds
# the chain unfolded while VACUUM runs the TTL and the tiering; then the VACUUM that folds the chain gives the part its true bounds, and the TTL cuts it and drops only its rows older than the retention.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 6
node_init tt 0 "autovacuum = off
sift.async_maintenance = off
sift.test_create_format1 = off"
node_start tt || bail "start failed"
TIER=$(dir tt)/tier
mkdir -p "$TIER"
sql tt "CREATE EXTENSION pg_sift" "CREATE TABLE ev (id int8 PRIMARY KEY, ts timestamp NOT NULL, v int NOT NULL, pad text NOT NULL) USING sift_table" \
	"INSERT INTO ev SELECT i, timestamp '2020-01-01' + i * interval '1 hour', i % 100, md5(i::text) FROM generate_series(1, 5000) i" \
	"VACUUM (FREEZE) ev" \
	"INSERT INTO ev SELECT 5000 + i, localtimestamp - i * interval '1 minute', i % 100, md5((5000 + i)::text) FROM generate_series(1, 5000) i" \
	"VACUUM (FREEZE) ev" \
	"SELECT sift.set_ttl('ev', 'ts', interval '30 days')" \
	"SELECT sift.set_tier_policy('ev', 'file://$TIER', '45 days')" >/dev/null || bail "setup failed"

PGAPPNAME=holder sql tt "BEGIN ISOLATION LEVEL REPEATABLE READ" "SELECT count(*) FROM ev" "SELECT pg_sleep(60)" >/dev/null 2>&1 &
holder=$!
until [[ $(sql tt "SELECT count(*) FROM pg_stat_activity WHERE application_name = 'holder' AND state = 'active' AND query LIKE '%pg_sleep%'") == 1 ]]; do :; done
sql tt "UPDATE ev SET ts = localtimestamp WHERE id <= 100" >/dev/null
out=$(sql tt "SELECT dead_rows FROM sift.space('ev')")
check "the update of the time column of 100 rows is a column delta: no row was deleted" test "$out" == 0
sql tt "VACUUM ev" >/dev/null
out=$(sql tt "SELECT count(*) FROM ev WHERE id <= 100 AND ts > localtimestamp - interval '1 day'")
check "the 100 rows are there after a VACUUM ran the TTL while a snapshot held the chain" test "$out" == 100
out=$(sql tt "SELECT count(*) FROM ev")
check "and no part was dropped as expired" test "$out" == 10000
out=$(sql tt "SELECT count(*) FROM sift.tier_status")
check "and no part was moved to the object store as cold" test "$out" == 0
out=$(sql tt "SELECT count(*) FROM sift.check('ev', true)")
check "and the table verifies" test "$out" == 0

sql tt "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE application_name = 'holder'" >/dev/null
wait "$holder" 2>/dev/null
sql tt "VACUUM ev" "VACUUM ev" >/dev/null
out=$(sql tt "SELECT count(*) || ' ' || count(*) FILTER (WHERE id <= 100 AND ts > localtimestamp - interval '1 day') FROM ev")
check "the VACUUM that folds the chain keeps them too, and the TTL drops the 4,900 rows older than the retention" test "$out" == "5100 100"
finish
