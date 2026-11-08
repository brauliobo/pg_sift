#!/usr/bin/env bash
# The maintenance worker of a sift_col index reuses the space of the segments it merged, step after step: every step runs in a transaction of its own, so that what
# one step freed is reusable by the next (the worker used to keep one transaction and one snapshot for all its steps: the xid that released the blocks and the
# snapshot of the worker held the horizon back, and each step took new blocks at the end of the file).
. "$(dirname "$0")/lib.bash"

ROUNDS=$(knob SIFT_WS_ROUNDS 48 32)
plan 4
node_init reuse 6 "autovacuum = off"
node_start reuse || bail "start failed"
sql reuse "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE t (id int, grp int, amount numeric(10,2), pad text)" \
	"CREATE INDEX t_idx ON t USING sift_col (id, grp, amount, pad) WITH (vacuum_truncate = false, write_buffer_limit = 1024)" >/dev/null || bail "setup failed"

# promoted segments, no merge yet (the worker is off)
sql reuse "SET sift.async_maintenance = off" "DO \$\$ BEGIN FOR i IN 1..$ROUNDS LOOP
	INSERT INTO t SELECT i * 100000 + g, g % 50, g % 997, md5(g::text) || md5((g * i)::text) FROM generate_series(1, 3000) g;
	PERFORM sift.col_promote('t_idx'); COMMIT; END LOOP; END \$\$" >/dev/null || bail "load failed"
check "segments before the worker" test "$(sql reuse "SELECT count(*) FROM sift.col_segments('t_idx')")" -ge "$ROUNDS"
before=$(sql reuse "SELECT pg_relation_size('t_idx')")
# a short statement that seals the write buffer starts the worker after it commits: it merges step after step with no transaction of a client open
sql reuse "ALTER INDEX t_idx SET (write_buffer_limit = 64)" "INSERT INTO t SELECT 99000000 + g, g % 50, g % 997, md5(g::text) || md5((g * 3)::text) FROM generate_series(1, 3000) g" >/dev/null || bail "insert failed"
wait_stable reuse "SELECT count(*) FROM sift.col_segments('t_idx')" 900 || bail "the worker does not stop"
read -r segs live file <<<"$(sql reuse "SELECT (SELECT count(*) FROM sift.col_segments('t_idx')), bytes, pg_relation_size('t_idx') FROM sift.col_index_info('t_idx')" | tr '|' ' ')"
diag "$segs segments, live $((live / 1024)) kB, file $((before / 1024)) kB before the worker, $((file / 1024)) kB after"
check "the worker merged the segments" test "$segs" -le 8
check "the worker took no new blocks for the output of its steps: the file grew by under a third" test "$file" -lt "$((before * 4 / 3))"
check "intact" test -z "$(sql reuse "SELECT sift.col_verify('t_idx', true)" 2>&1)"
finish
