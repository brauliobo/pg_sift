#!/usr/bin/env bash
# The tail of a bulk load: the seals that fall inside the sift.maintenance_naptime window of the request that started the
# background worker get no worker of their own, so the worker stays and promotes them; they are not left as sealed write buffers
# until the next seal or a VACUUM. Runs on the production build too (no test functions).
. "$(dirname "$0")/lib.bash"

plan 5
node_init tail 12 "autovacuum = off
max_worker_processes = 16"
node_start tail || bail "start failed"
sql tail "CREATE EXTENSION pg_sift" "CREATE TABLE t (id serial, body text) WITH (autovacuum_enabled = off)" \
	"CREATE INDEX t_idx ON t USING sift (body) WITH (write_buffer_limit = 64)" >/dev/null || bail "setup failed"

sealed() { sql tail "SELECT count(*) FROM sift.segments('t_idx') WHERE kind = 'sealed buffer'"; }

# loads of a few seconds, one after the other: each ends with seals inside the window of the last request
for round in 1 2 3; do
	sql tail "INSERT INTO t (body) SELECT 'zzcommon ' || (SELECT string_agg('w' || (random() * 3000)::int, ' ') FROM generate_series(1, 40)) FROM generate_series(1, 30000) i" >/dev/null
done

# the window (2 s) plus the worker's polling
for _ in $(seq 60); do
	[[ $(sealed) == 0 ]] && break
	sleep 0.1
done
check "no sealed segment is left after the window of the last request" test "$(sealed)" == 0
check "verify: intact" test "$(sql tail "SELECT sift.verify('t_idx', true)" 2>&1 | grep -c -E "ERROR|corrupt")" == 0
check "a query reads it back" test "$(sql tail "SET enable_seqscan = off" "SELECT count(*) FROM t WHERE body @@> 'zzcommon'" | tail -1)" == 90000

check "the naptime is a GUC of 200 to 60000 ms" test "$(sql tail "LOAD 'pg_sift'" "SHOW sift.maintenance_naptime")" == 2s
check "a naptime out of range is refused" test "$(sql tail "LOAD 'pg_sift'" "ALTER SYSTEM SET sift.maintenance_naptime = 100" 2>&1 | grep -c "outside the valid range")" == 1
finish
