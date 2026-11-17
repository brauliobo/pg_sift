#!/usr/bin/env bash
# One VACUUM asks for at most one maintenance worker and for no rewrite, whether a worker or a session runs it: the postmaster at debug1 logs every background worker it starts, by name.
. "$(dirname "$0")/lib.bash"

plan 2
node_init pv 69 "autovacuum = off
log_min_messages = debug1
sift.maintenance_naptime = 1000
sift.col_auto_merge = on"
node_start pv || bail "start failed"
D=$(dir pv)
sql pv "CREATE EXTENSION pg_sift" "CREATE TABLE facts (id int, key int, kind text, amount numeric(10,2), pad text)" \
	"INSERT INTO facts SELECT g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 100) FROM generate_series(1, 100000) g" \
	"CREATE INDEX facts_idx ON facts USING sift_col (id, key, kind, amount)" "VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) facts" >/dev/null || bail "setup failed"
started() { grep -c "starting background worker process \"sift $1" "$D/postgres.log"; }
sql pv "UPDATE facts SET amount = amount + 1 WHERE id % 20 = 1" >/dev/null
for _ in $(seq 100); do
	sql pv "SET max_parallel_workers_per_gather = 0" "SELECT kind, count(*), sum(amount) FROM facts GROUP BY kind" >/dev/null 2>&1
	[[ $(started vacuum) -ge 1 ]] && break
	sleep 0.2
done
sleep 3
sql pv "SET max_parallel_workers_per_gather = 0" "SELECT kind, count(*), sum(amount) FROM facts GROUP BY kind" >/dev/null 2>&1
check "the VACUUM of the vacuum worker asked for at most one maintenance worker and no rewrite" test "$(started maintenance)" -le 1 -a "$(grep -c 'rewrote (' "$D/postgres.log")" == 0
before=$(started maintenance)
sql pv "UPDATE facts SET amount = amount + 1 WHERE id % 20 = 2" "VACUUM facts" >/dev/null
sleep 3
check "a VACUUM of a session asks for at most one maintenance worker and no rewrite" test "$(( $(started maintenance) - before ))" -le 1 -a "$(grep -c 'rewrote (' "$D/postgres.log")" == 0
finish
