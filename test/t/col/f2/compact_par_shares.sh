#!/usr/bin/env bash
# The shares of a VACUUM (PARALLEL 2) of a sift_col index. A partition whose rows lie on both sides of the hot cut, compacted under a tight maintenance_work_mem, writes a part of each side
# for every granule of a share: the slot of the share holds them all, and the VACUUM succeeds. When the leader and a worker both fail (both cancelled while they wait at the gate after a share,
# sift.test_phase_gate), the leader waits out the second error too and gives back the parts of the shares that were done: no block is left that nothing names.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "${SIFT_REPO:?}/test/t/lib/common.bash"

done_testing
node_init cs 3 "autovacuum = off
sift.async_maintenance = off
max_worker_processes = 16
max_parallel_workers = 8
max_parallel_maintenance_workers = 2"
node_start cs || bail "start failed"
sql cs "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"

problems() { sql cs "SELECT count(*) FROM sift.check('$1')"; }

sql cs "SET sift.test_create_format1 = off" "CREATE TABLE ev (id int8, ts timestamptz NOT NULL, pad text)" \
	"INSERT INTO ev SELECT i, CASE WHEN i % 2 = 0 THEN now() - interval '400 days' ELSE now() END, repeat(md5(i::text) || md5(i || 'b') || md5(i || 'c') || md5(i || 'd'), 16)
		FROM generate_series(1, 102400) i" \
	"CREATE INDEX ev_col ON ev USING sift_col (id, ts, pad)" "DELETE FROM ev WHERE id % 5 = 0" >/dev/null 2>&1 || bail "the hot and cold load failed"
out=$(sql cs "LOAD 'pg_sift'" "SET statement_timeout = '30s'" "SET maintenance_work_mem = '32MB'" "VACUUM (PARALLEL 2) ev" 2>&1)
check "the VACUUM (PARALLEL 2) of a partition cut at the hot cut succeeds" test -z "$out"
check "the index is clean and holds the rows of the heap" test "$(sql cs "SELECT count(*) FROM sift.check('ev_col', true)")" = 0

D=$(dir cs)
G=4300
waiting() { sql cs "SELECT count(*) FROM pg_stat_activity WHERE query LIKE 'VACUUM (PARALLEL 2)%' AND wait_event_type = 'Lock' AND wait_event = 'advisory'"; }
# the blocks of the index in use outside its metadata slabs, whose pages may be given back by any change of the manifest
used() { sql cs "SELECT used_blocks - split_part(sift._test_f2_fsm('$1'), '/', 3)::int8 FROM sift.space('$1')"; }
# cancelled VACUUM (PARALLEL 2) of table $1 while its participants wait at the gate after their first share, $2 the backends cancelled: the blocks it leaves in use
cancelled() {
	local before
	sql cs "SET sift.test_create_format1 = off" "CREATE TABLE $1 (id int8, a int8, pad text)" "INSERT INTO $1 SELECT g, g, repeat('z', 40) FROM generate_series(1, 100000) g" \
		"CREATE INDEX $1_col ON $1 USING sift_col (id, a, pad)" "DELETE FROM $1 WHERE id % 5 <> 0" >/dev/null || bail "the load of $1 failed"
	before=$(used "$1_col")
	sess_open cs gate
	sess_say gate "SELECT pg_advisory_lock($((G + 16))); \\echo @@held"
	wait_for "grep -q '@@held' '$D/gate.out'" 30 || bail "the gate was not taken"
	rm -f "$D/vac.rc"
	(sql cs "LOAD 'pg_sift'" "SET statement_timeout = '30s'" "SET sift.test_phase_gate = $G" "VACUUM (PARALLEL 2) $1" >"$D/vac.out" 2>&1; echo $? >"$D/vac.rc") &
	wait_for "[[ \$(waiting) == 2 ]]" 60 || bail "the participants of $1 did not reach the gate"
	sql cs "SELECT count(pg_cancel_backend(pid)) FROM pg_stat_activity WHERE query LIKE 'VACUUM (PARALLEL 2)%' AND wait_event = 'advisory' AND $2" >/dev/null
	sess_say gate "SELECT pg_advisory_unlock($((G + 16)));"
	sess_close gate
	wait_for "[[ -f '$D/vac.rc' ]]" 60 || bail "the cancelled VACUUM of $1 did not end"
	grep -q 'canceling statement due to user request' "$D/vac.out" || bail "the VACUUM of $1 was not cancelled: $(cat "$D/vac.out")"
	echo $(($(used "$1_col") - before))
}
two_failures() {
	local one two
	sql cs "CREATE EXTENSION pg_sift_test" >/dev/null || bail "the test extension failed"
	one=$(cancelled t1 "backend_type = 'client backend'")
	two=$(cancelled t2 true)
	out="leader alone: $one blocks, leader and worker: $two blocks"
	check "a leader that fails with its worker gives back the parts of the shares that were done, as a leader that fails alone does" test "$two" = "$one"
	check "the index is clean" test "$(problems t2_col)" = 0
	out=$(sql cs "LOAD 'pg_sift'" "VACUUM (PARALLEL 2) t2" 2>&1)
	check "the next VACUUM (PARALLEL 2) succeeds" test -z "$out"
	check "its rows are those of the DELETE" test "$(sql cs "SELECT count(*) FROM t2 WHERE a >= 0")" = 20000
}
if [[ $(sql cs "SELECT count(*) FROM pg_settings WHERE name = 'sift.test_phase_gate'") == 1 ]]; then
	two_failures
else
	skip 4 "the release library has no gate"
fi
finish
