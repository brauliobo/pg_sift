#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# kill -9 in the middle of the parallel build of a key_cut sift_col index (workers, gap, merge into the gap) with a streaming standby attached, then crash recovery:
# the build is rolled back (no index, on the primary and on the standby), the standby streams again and counts as the primary, and the build run again finishes with
# the file at most 1.1 times its live bytes (the gap and the cut of a build; the dead build leaves nothing the next one has to work around) and an index that verifies
# and answers as the table does. Kills: a parallel worker held at the start of its scan (kill -9 of its pid), and the leader before the n-th directory update of the build and after the last
# (sift.test_crash_dir: the publication of the segments, then each partition the merge into the gap writes). SIFT_BK_ROWS (400000, 150000 quick), SIFT_BK_POINTS
# (most directory updates killed in the leader, 8, 4 quick).
. "$(dirname "$0")/lib.bash"

ROWS=$(knob SIFT_BK_ROWS 400000 150000)
POINTS=$(knob SIFT_BK_POINTS 8 4)
node_init primary 2 "wal_level = replica
max_wal_senders = 4
wal_keep_size = 256MB
autovacuum = off
max_worker_processes = 16
max_parallel_workers = 8"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE t (id int, grp int, amount numeric(10,2), pad text) WITH (autovacuum_enabled = off)" \
	"INSERT INTO t SELECT g, g % 500, g % 997, md5(g::text) || md5((g * 7)::text) FROM generate_series(1, $ROWS) g" \
	"VACUUM (FREEZE, ANALYZE) t" >/dev/null || bail "setup failed"
node_clone primary standby 3 "hot_standby_feedback = on
wal_receiver_status_interval = 1s"
node_start standby || bail "standby start failed"
catchup primary standby || bail "the standby did not catch up"

OPTS=("SET max_parallel_maintenance_workers = 4" "SET min_parallel_table_scan_size = 0")
BUILD="CREATE INDEX t_idx ON t USING sift_col (id, grp, amount) WITH (key_cut = grp)"
size() { sql primary "SELECT pg_relation_size('t_idx')"; }
live() { sql primary "SELECT bytes FROM sift.col_index_info('t_idx')"; }
tight() { echo "# file $(size) live $(live)" >&2; test "$(size)" -le "$(( $(live) * 110 / 100 ))"; }
digest() { sql "$1" "SET enable_seqscan = off" "SELECT count(*) || ':' || sum(amount) FROM t WHERE grp = 7" | tail -1; }
heap() { sql primary "SET enable_indexscan = off" "SET enable_bitmapscan = off" "SELECT count(*) || ':' || sum(amount) FROM t WHERE grp = 7" | tail -1; }

same_as_table() { [[ -z $(sql primary "SELECT sift.col_verify('t_idx', true)" 2>&1) && $(digest primary) == "$(heap)" ]]; }

# the primary is down (restart_after_crash is off): up again, the standby streaming
restart() {
	for _ in $(seq 300); do kill -0 "$PM" 2>/dev/null || break; sleep 0.1; done
	kill -0 "$PM" 2>/dev/null && return 1
	node_start primary && catchup primary standby
}
# after the kill: nothing of the dead build, on both nodes, and the same build run again
recovered() { # LABEL
	[[ $(sql primary "SELECT to_regclass('t_idx') IS NULL") == t && $(sql standby "SELECT to_regclass('t_idx') IS NULL") == t ]] ||
		{ not_ok "$1: an index is left"; return 1; }
	ok "$1: recovery rolled the build back on the primary and on the standby"
	sql primary "${OPTS[@]}" "$BUILD" >/dev/null || { not_ok "$1: the build run again failed"; diag "$(tail -5 "$(dir primary)/postgres.log")"; return 1; }
	check "$1: the build run again: the file is at most 1.1 times its live bytes" tight
	check "$1: ... the index verifies and counts as the table" same_as_table
	catchup primary standby || { not_ok "$1: the standby does not catch up"; return 1; }
	check "$1: ... the standby counts as the primary" test "$(digest standby)" == "$(digest primary)"
	check "$1: ... and verifies the index" test -z "$(sql standby "SELECT sift.col_verify('t_idx', true)" 2>&1)"
	sql primary "DROP INDEX t_idx" >/dev/null && catchup primary standby
}

# a parallel worker, killed while the scan runs: the workers wait to start the scan for an advisory lock this session holds (sift.test_phase_gate + the scan phase), so one is there to
# be found and killed whatever the speed and the cores of the runner
PM=$(head -1 "$(dir primary)/data/postmaster.pid")
D=$(dir primary)
GATE=7000
rm -f "$D/held"
{ echo "SELECT pg_advisory_lock($(( GATE + 2 )));"; echo "\\! touch '$D/held'"; echo "SELECT pg_sleep(600);"; } | sql primary >/dev/null 2>&1 &
until [[ -e $D/held ]]; do sleep 0.05; done
sql primary "${OPTS[@]}" "SET sift.test_phase_gate = $GATE" "$BUILD" >"$D/build.out" 2>&1 &
BUILDER=$!
victim=
for _ in $(seq 1200); do
	victim=$(sql primary "SELECT pid FROM pg_stat_activity WHERE backend_type = 'parallel worker' AND wait_event_type = 'Lock' LIMIT 1" 2>/dev/null)
	[[ -z $victim ]] || break
	kill -0 "$BUILDER" 2>/dev/null || break
	sleep 0.05
done
if [[ -n $victim ]]; then
	SIFT_TMP=$D leak_expect 1 'terminated by signal 9'
	kill -9 "$victim"
fi
wait "$BUILDER"; built=$?
check "a parallel worker was found and killed, the build failed" test -n "$victim" -a "$built" -ne 0
if restart; then ok "the primary restarts and the standby streams again"; recovered "worker killed in the scan" || true
else not_ok "the primary restarts and the standby streams again"; fi

# the leader, before the n-th directory update of the build
# the count varies from build to build (4 to 6 on a loaded runner): the smallest of three keeps the last kill points reachable
updates=
for _ in 1 2 3; do
	sql primary "${OPTS[@]}" "$BUILD" "SELECT sift._test_dir_updates()" >"$(dir primary)/k.out" 2>&1 && n=$(tail -1 "$(dir primary)/k.out") || n=0
	sql primary "DROP INDEX t_idx" >/dev/null
	(( ${updates:-$n} >= n )) && updates=$n
done
diag "the build publishes $updates directory updates"
check "the build publishes directory updates (the segments, then the partitions of the merge)" test "${updates:-0}" -gt 1
points=(1 "$(( (updates + 1) / 2 ))" "$updates" "-$updates" 2 3 4 5 6 7 8)
declare -A seen=()
done_points=0
for n in "${points[@]}"; do
	(( n >= 1 && n <= updates || n == -updates )) && [[ -z ${seen[$n]:-} ]] || continue
	(( done_points < POINTS )) || break
	seen[$n]=1
	done_points=$((done_points + 1))
	# the participants claim the heap by superblock, so the leader publishes 4 or 5 updates, build after build of one table: a build that ends before the n-th one is dropped and run again
	crashed=
	for _ in $(seq 8); do
		PM=$(head -1 "$(dir primary)/data/postmaster.pid")
		SIFT_TMP=$(dir primary) leak_expect 1 'terminated by signal 9'
		if sql primary "${OPTS[@]}" "SET sift.test_crash_dir = $n" "$BUILD" >/dev/null 2>&1; then sql primary "DROP INDEX t_idx" >/dev/null; else crashed=1; break; fi
	done
	what="leader killed $([[ $n -gt 0 ]] && echo before || echo after) update ${n#-} of $updates"
	if [[ -z $crashed ]]; then not_ok "$what: eight builds ended before it"
	elif restart; then recovered "$what" || true
	else not_ok "$what: restart"; fi
done
echo "1..$NTEST"
finish
