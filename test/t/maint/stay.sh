#!/usr/bin/env bash
# tap: pooled
# A maintenance worker that leaves early does not drop a request that lands while it leaves. The worker holds a lock of its index while it is up and lets it go before its
# last look; a seal that finds the lock held asks for no worker of its own, and the last look (made after the lock is free) promotes what it sealed. The worker is held at
# sift.test_stay_gate between its last idle look and the release of the lock, so no timing is involved: the seal lands there, and the segment is promoted without a VACUUM or a later seal.
. "$(dirname "$0")/lib.bash"

plan 4
node_init sy 74 "autovacuum = off
sift.test_create_format1 = on
sift.test_col_net = off
sift.async_maintenance = on
sift.maintenance_naptime = 2000"
node_start sy || bail "start failed"
D=$(dir sy)
sql sy "CREATE EXTENSION pg_sift" "CREATE TABLE w (id int PRIMARY KEY, k int, v int) WITH (autovacuum_enabled = false)" \
	"CREATE INDEX w_col ON w USING sift_col (id, k, v) WITH (write_buffer_limit = 64)" "ALTER DATABASE postgres SET sift.test_stay_gate = on" >/dev/null || bail "setup failed"
rm -f "$D/locked" "$D/release"
{ echo "SELECT pg_advisory_lock(20983, 12);"; echo "\\! touch '$D/locked'"; echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"; } | sql sy >/dev/null &
GATE=$!
until [[ -e $D/locked ]]; do sleep 0.05; done

held() { [[ $(sql sy "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND classid = 20983 AND objid = 12 AND NOT granted") -ge 1 ]]; }
sealed() { sql sy "SELECT count(*) FROM sift.col_segments('w_col') WHERE kind = 'sealed buffer'"; }
segments() { sql sy "SELECT count(*) FILTER (WHERE kind = 'immutable') FROM sift.col_segments('w_col')"; }

sql sy "INSERT INTO w SELECT i, i % 10, i FROM generate_series(1, 9000) i" >/dev/null
check "the worker that promoted it is held between its last idle look and its exit" wait_for held 60
before=$(segments)
sql sy "INSERT INTO w SELECT 10000 + i, i % 10, i FROM generate_series(1, 9000) i" >/dev/null
check "a second seal lands there (it asks for no worker of its own)" test "$(sealed)" -ge 1
touch "$D/release"
wait "$GATE"
check "the last look promotes it, without a VACUUM or a later seal" wait_for '[[ $(sealed) == 0 ]]' 60
check "and the rows are all in the index" test "$(sql sy "SELECT sift.col_verify('w_col', true)")" == ""
finish
