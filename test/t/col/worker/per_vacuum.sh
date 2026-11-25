#!/usr/bin/env bash
# One VACUUM of a session asks for at most one maintenance worker and for no rewrite: the postmaster at debug1 logs every background worker it starts, by name.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 1
node_init pv 69 "autovacuum = off
log_min_messages = debug1
sift.maintenance_naptime = 1000
sift.async_maintenance = on"
node_start pv || bail "start failed"
D=$(dir pv)
sql pv "CREATE EXTENSION pg_sift" "CREATE TABLE facts (id int, key int, kind text, amount numeric(10,2), pad text)" \
	"INSERT INTO facts SELECT g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 100) FROM generate_series(1, 100000) g" \
	"CREATE INDEX facts_idx ON facts USING sift_col (id, key, kind, amount)" "VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) facts" >/dev/null || bail "setup failed"
started() { grep -c "starting background worker process \"sift $1" "$D/postgres.log"; }
before=$(started maintenance)
sql pv "UPDATE facts SET amount = amount + 1 WHERE id % 20 = 2" "VACUUM facts" >/dev/null
sleep 3
check "a VACUUM asks for at most one maintenance worker and no rewrite" test "$(( $(started maintenance) - before ))" -le 1 -a "$(grep -c 'rewrote (' "$D/postgres.log")" == 0
finish
