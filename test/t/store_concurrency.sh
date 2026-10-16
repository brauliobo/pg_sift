#!/usr/bin/env bash
# Concurrent writers, droppers, readers, recyclers, allocators and sweeps
# (pgbench, 16 clients): no errors, no leaked block, every segment intact.
# Then the horizon protocol on a primary: a reader holding an old directory
# copy keeps a dropped segment's extents from being reused.
. "$(dirname "$0")/lib.bash"

SECS=$(knob SIFT_CHURN_SECS 30 8)
plan 7
node_init conc 4
node_start conc || bail "start failed"
sql conc "CREATE EXTENSION pg_sift_test CASCADE" "CREATE TABLE t (v int)" \
	"CREATE INDEX t_idx ON t USING sift_store_test (v)" \
	"CREATE TABLE r (v int)" "CREATE INDEX r_idx ON r USING sift_store_test (v)" >/dev/null || bail "setup failed"

read -ra FILES <<<"$(churn conc t_idx 2000000)"
pgbench_bg conc 16 "$SECS" "${FILES[@]}"
wait "$PGBENCH_PID"
check "pgbench churn: no failed transaction" grep -q "number of failed transactions: 0 " "$(dir conc)/pgbench.log"
diag "$(grep -E "^(number of transactions actually|tps)" "$(dir conc)/pgbench.log")"
check "no leaked block after churn" test "$(sql conc "SELECT sift._test_check('t_idx')")" == 0
check "every segment reads back" test "$(sql conc "SELECT bool_and(sift._test_seg_read('t_idx', gen) = payload)
	FROM sift._test_segments('t_idx')")" == t

# Reader of segment g sleeps 3 s holding its directory copy while g is dropped
g=$(sql conc "SELECT sift._test_seg_write('r_idx', 20000000, 11)")
hwm=$(sql conc "SELECT value FROM sift._test_info('r_idx') WHERE key = 'high_water'")
sql conc "SELECT sift._test_seg_read('r_idx', $g, 0, 3000)" >"$(dir conc)/reader.out" 2>&1 &
reader=$!
sleep 0.5
sql conc "SELECT sift._test_seg_drop('r_idx', $g)" "SELECT sift._test_recycle('r_idx')" \
	"SELECT sift._test_seg_write('r_idx', 20000000, 12)" >/dev/null
pending=$(sql conc "SELECT value FROM sift._test_info('r_idx') WHERE key = 'pending_blocks'")
grown=$(sql conc "SELECT value > $hwm FROM sift._test_info('r_idx') WHERE key = 'high_water'")
wait "$reader"
check "reader of a dropped segment succeeds" test "$(cat "$(dir conc)/reader.out")" == 20000000
check "its extents stayed pending ($pending blocks) and the index grew instead" \
	test "$pending" -gt 2400 -a "$grown" == t
check "after the reader, the extents become reusable" \
	test "$(sql conc "SELECT sift._test_recycle('r_idx') > 0,
		(SELECT value FROM sift._test_info('r_idx') WHERE key = 'pending_blocks')")" == "t|0"
check "store invariants hold" test "$(sql conc "SELECT sift._test_check('r_idx') + sift._test_check('t_idx')")" == 0
finish
