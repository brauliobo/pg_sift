#!/usr/bin/env bash
# kill -9 of the postmaster and all backends in the middle of concurrent
# segment writes, drops, frees, recycles, sweeps, REINDEX (BULK) and unlogged
# writes, then crash recovery and checks: store invariants, every published
# segment reads back byte-exact, the unlogged index is reset from its init
# fork, and the sweep reclaims every orphan; freed space is reused, so the
# node stays below SIFT_CRASH_MAX_MB (default 4096). SIFT_CRASH_ITERS
# (default 100, 10 quick).
. "$(dirname "$0")/lib.bash"

ITERS=$(knob SIFT_CRASH_ITERS 100 10)
MAX_MB=$(knob SIFT_CRASH_MAX_MB 4096 1024)
plan $((ITERS + 1))
node_init crash 1 "$WAL_CONF
checkpoint_timeout = 30s
max_wal_size = 256MB"
node_start crash || bail "start failed"
sql crash "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE t (v int)" "CREATE INDEX t_idx ON t USING sift_store_test (v)" \
	"CREATE TABLE b (v int)" "INSERT INTO b VALUES (0), (9000), (400000), (2500000)" \
	"CREATE INDEX b_idx ON b USING sift_store_test (v)" \
	"CREATE UNLOGGED TABLE u (v int)" "INSERT INTO u VALUES (70000)" \
	"CREATE INDEX u_idx ON u USING sift_store_test (v)" >/dev/null || bail "setup failed"
ok "setup"
read -ra FILES <<<"$(churn crash t_idx "$CHURN_BYTES" \
	"reindex:2:REINDEX INDEX b_idx;" \
	"unlogged:3:SELECT sift._test_seg_write('u_idx', 50000, 7);")"

VERIFY="SELECT sift._test_check('t_idx'),
	(SELECT count(*) || ' ' || coalesce(bool_and(sift._test_seg_read('t_idx', gen) = payload), true)
	 FROM sift._test_segments('t_idx')),
	(SELECT count(*) || ' ' || coalesce(bool_and(sift._test_seg_read('b_idx', gen) = payload), true)
	 FROM sift._test_segments('b_idx')),
	sift._test_check('b_idx'),
	(SELECT string_agg(value::text, ' ' ORDER BY key) FROM sift._test_info('u_idx')
	 WHERE key IN ('high_water', 'nsegs')),
	sift._test_sweep('t_idx'),
	sift._test_check('t_idx'),
	pg_relation_size('t_idx') / 1048576"

total_orphans=0
for i in $(seq "$ITERS"); do
	pgbench_bg crash 8 600 "${FILES[@]}"
	sleep "$((RANDOM % 3)).$((RANDOM % 9 + 1))$((RANDOM % 10))"
	crash crash
	wait "$PGBENCH_PID"
	if ! node_start crash; then
		not_ok "iteration $i: restart"
		diag "$(tail -20 "$(dir crash)/postgres.log")"
		break
	fi
	out=$(sql crash "$VERIFY" 2>&1)
	IFS='|' read -r orphans tsegs bsegs borphans unlogged swept after mb <<<"$out"
	used=$(node_mb crash)
	if [[ $tsegs == *" true" && $bsegs == "4 true" && $borphans == 0 && $unlogged == "2 0" &&
		$swept == "$orphans" && $after == 0 && $used -lt $MAX_MB ]]; then
		ok "iteration $i: ${tsegs% *} segments verified, $orphans orphans swept, index $mb MB, node $used MB"
		total_orphans=$((total_orphans + orphans))
	else
		not_ok "iteration $i"
		diag "$out"
		break
	fi
done
diag "orphans reclaimed over $ITERS crashes: $total_orphans"
finish
