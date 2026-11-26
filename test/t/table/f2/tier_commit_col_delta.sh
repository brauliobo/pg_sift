#!/usr/bin/env bash
# tap: pooled
# The commit of a tiered part looks again at its partition under the partition lock: an UPDATE of a column that is not the time column, which becomes a column delta while the object is
# uploaded, keeps the part local (the fold of the chain rewrites it), the rows read back as updated, and the object no commit names is left to the sweep.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 4
GATE=7100
PH_TIER=12
node_init tg 0 "autovacuum = off
sift.async_maintenance = off
sift.test_create_format1 = off"
node_start tg || bail "start failed"
D=$(dir tg)
mkdir -p "$D/tier"
sql tg "CREATE EXTENSION pg_sift" "CREATE TABLE ev (id int8 PRIMARY KEY, ts timestamp NOT NULL, v int NOT NULL, pad text NOT NULL) USING sift_table" \
	"INSERT INTO ev SELECT i, timestamp '2020-01-01' + i * interval '1 hour', i % 100, md5(i::text) FROM generate_series(1, 5000) i" "VACUUM (FREEZE) ev" \
	"SELECT sift.set_tier_policy('ev', 'file://$D/tier', '45 days')" >/dev/null || bail "setup failed"

{ echo "SELECT pg_advisory_lock($((GATE + PH_TIER)));"; echo "\\! touch '$D/held'"; echo "SELECT pg_sleep(600);"; } | sql tg >/dev/null 2>&1 &
holder=$!
wait_for "[[ -e '$D/held' ]]" 60 || bail "the holder of the gate did not start"
sql tg "SET sift.test_phase_gate = $GATE" "VACUUM ev" >/dev/null 2>&1 &
vac=$!
wait_for "[[ \$(sql tg \"SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted AND objid = $((GATE + PH_TIER))\") -ge 1 ]]" 60 || bail "the upload did not reach its gate"
PGAPPNAME=snap sql tg "BEGIN ISOLATION LEVEL REPEATABLE READ" "SELECT count(*) FROM ev" "SELECT pg_sleep(600)" >/dev/null 2>&1 &
snap=$!
wait_for "[[ \$(sql tg \"SELECT count(*) FROM pg_stat_activity WHERE application_name = 'snap' AND query LIKE '%pg_sleep%'\") == 1 ]]" 60 || bail "the snapshot did not open"
sql tg "UPDATE ev SET v = v + 1000 WHERE id <= 100" >/dev/null
check "the update of 100 rows during the upload is a column delta" test "$(sql tg "SELECT dead_rows FROM sift.space('ev')")" == 0

sql tg "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE query LIKE 'SELECT pg_sleep(600)%' AND application_name <> 'snap'" >/dev/null
wait "$holder" "$vac" 2>/dev/null
check "the part with the chain stays local" test "$(sql tg "SELECT count(*) FROM sift.tier_status")" == 0
check "the 100 rows read back as updated" test "$(sql tg "SELECT count(*) FROM ev WHERE id <= 100 AND v >= 1000")" == 100
sql tg "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE application_name = 'snap'" >/dev/null
wait "$snap" 2>/dev/null
check "sift.check finds the table sound" test "$(sql tg "SELECT count(*) FROM sift.check('ev')")" == 0
finish
