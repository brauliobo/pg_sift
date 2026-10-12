#!/usr/bin/env bash
# kill -9 of the postmaster and all backends during CREATE INDEX, and right
# after a committed build (before any checkpoint), then crash recovery:
# an interrupted build leaves no index, a committed one replays from WAL and
# passes sift.verify with the same postings, the UNLOGGED index is reset to
# an empty one. SIFT_CRASH_ITERS (default 5).
. "$(dirname "$0")/lib.bash"

ITERS=${SIFT_CRASH_ITERS:-5}
plan $((2 * ITERS + 1))
node_init build 1 "checkpoint_timeout = 1h
max_wal_size = 4GB
maintenance_work_mem = 8MB"
node_start build || bail "start failed"
sql build "CREATE EXTENSION sift_test CASCADE" \
	"SELECT setseed(0.5)" \
	"CREATE TABLE z AS SELECT i AS id, 'all ' || (SELECT string_agg('w' || floor(exp(random() * ln(30000)))::int, ' ')
		FROM generate_series(1, 1 + (random() * 50)::int + i % 2)) AS body FROM generate_series(1, 200000) i" \
	"CREATE UNLOGGED TABLE u AS SELECT * FROM z WHERE id % 10 = 0" \
	"CHECKPOINT" >/dev/null || bail "setup failed"
ok "setup"

DIGEST="SELECT count(*) || ' ' || sum(hashtext(term || tid::text || tf || ' ' || dl)) FROM sift._test_postings('z_idx')"

for i in $(seq "$ITERS"); do
	# interrupted build: no index afterwards, the table stays usable
	sql build "CREATE INDEX z_idx ON z USING sift (body)" >/dev/null 2>&1 &
	sleep "0.$((RANDOM % 9 + 1))"
	crash build
	wait
	if ! node_start build; then
		not_ok "iteration $i: restart after an interrupted build"
		diag "$(tail -20 "$(dir build)/postgres.log")"
		break
	fi
	out=$(sql build "SELECT count(*) FROM pg_class WHERE relname = 'z_idx'" "DROP INDEX IF EXISTS z_idx" \
		"CREATE INDEX z_idx ON z USING sift (body) WITH (build_segments = $((i % 3 + 1)))" \
		"SELECT sift.verify('z_idx')" "$DIGEST" 2>&1)
	check "iteration $i: interrupted build left no index ($(head -1 <<<"$out"))" [ "$(head -1 <<<"$out")" -le 1 ]
	want=$(tail -1 <<<"$out")

	# committed build, crash before any checkpoint: WAL replay restores it
	sql build "CREATE INDEX u_idx ON u USING sift (body)" >/dev/null 2>&1
	crash build
	node_start build || bail "restart after a committed build failed"
	out=$(sql build "SELECT sift.verify('z_idx')" "$DIGEST" "SELECT sift.verify('u_idx')" \
		"SELECT count(*) FROM sift.segments('u_idx')" "DROP INDEX u_idx" "CHECKPOINT" 2>&1)
	if [[ $(sed -n 2p <<<"$out") == "$want" && $(sed -n 4p <<<"$out") == 0 ]]; then
		ok "iteration $i: committed index replayed ($want), unlogged index reset"
	else
		not_ok "iteration $i: after crash"
		diag "$out (want $want)"
		break
	fi
	sql build "DROP INDEX z_idx" >/dev/null
done
finish
