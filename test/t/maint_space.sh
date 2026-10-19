#!/usr/bin/env bash
# The maintenance worker reuses the space of the segments it merged, step after step: 60 small
# segments merged by the worker leave an index file of under twice the live segments (a worker that
# kept one snapshot and transaction, and ran its steps on fragmented free space, left 2.6 times).
. "$(dirname "$0")/lib.bash"

plan 4
node_init space 7 "autovacuum = off"
node_start space || bail "start failed"
sql space "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE t (id serial, body text)" \
	"CREATE INDEX t_idx ON t USING sift (body) WITH (write_buffer_limit = 1024)" >/dev/null || bail "setup failed"

BATCH="INSERT INTO t (body) SELECT string_agg(md5((g * %s + j)::text), ' ') FROM generate_series(1, 1500) g, generate_series(1, 6) j GROUP BY g"

# 60 promoted segments, no merge yet (the worker is off)
sql space "SET sift.async_maintenance = off" "DO \$\$ BEGIN FOR i IN 1..60 LOOP
	$(printf "$BATCH" "(1000 + i * 7)"); PERFORM sift.promote('t_idx'); COMMIT; END LOOP; END \$\$" >/dev/null || bail "load failed"
check "60 segments before the worker" test "$(sql space "SELECT count(*) FROM sift.segments('t_idx')")" -ge 60

# a few short statements seal the write buffer, the last one with the worker allowed: it starts the worker, which merges with nothing else
# taking space meanwhile (statements after the first sealing one, with the worker running, made the file 2.2 times the live segments)
for k in $(seq 7); do sql space "SET sift.async_maintenance = off" "$(printf "$BATCH" "$((5000 + k))")" >/dev/null || bail "insert failed"; done
sql space "$(printf "$BATCH" 5008)" >/dev/null || bail "insert failed"
for _ in $(seq 600); do
	[[ $(sql space "SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'sift maintenance'") == 0 ]] && break
	sleep 0.2
done
read -r segs live file <<<"$(sql space "SELECT count(*), sum(bytes), pg_relation_size('t_idx') FROM sift.segments('t_idx')" | tr '|' ' ')"
diag "$segs segments, $((live / 1048576)) MB live, file $((file / 1048576)) MB, ratio $((file * 100 / live))%"
check "the worker merged the segments" test "$segs" -le 12
check "the file is under twice the live segments" test "$file" -lt "$((live * 2))"

check "intact, no orphan block" test "$(sql space "SELECT sift._test_check('t_idx')")" == 0
finish
