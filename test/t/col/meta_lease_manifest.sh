#!/usr/bin/env bash
# Block 0 is asked for as little as a writer can ask: 16 writers of a sift_table of 16 partitions each put 10 bulk inserts of 300 rows in a partition of their own (160 commits of a part) and the exclusive
# locks of Block 0 they take (the generations, the metadata pages, the vblocks and the manifest record of each part) stay under 150, the rows are all there, the index checks, and the allocator has no page of
# metadata that nothing names once it is cut. The registries of shared memory scale: 32 sessions that ask the queue of maintenance for what it already holds, and 32 that read and publish the manifests,
# take no longer than a few times one session, and no reader makes a list of a partition for another reader that is making it.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

WRITERS=16 INSERTS=10 ROWS=300 CLIENTS=32
# the writers that overlap share their locks of Block 0 (fewer than one for each commit of a part); on fewer cores than writers they run mostly one after the other: under two for each
LOCK_LIMIT=$((CORES >= WRITERS ? 150 : 2 * WRITERS * INSERTS))
# the sessions that share the cores take one turn after the other: the time of one session times the turns that the cores need for CLIENTS sessions
TURNS=$(((CLIENTS + CORES - 1) / CORES))
plan 7
node_init ml 4 "autovacuum = off
shared_preload_libraries = 'pg_sift'
sift.async_maintenance = off
sift.shared_cache_kb = 65536
max_connections = 80"
node_start ml || bail "start failed"
TESTING=$(sql ml "CREATE EXTENSION pg_sift" "SELECT count(*) FROM pg_settings WHERE name = 'sift.test_phase_gate'" | tail -1)
[[ $TESTING == 1 ]] || bail "needs the testing library"
sql ml "CREATE EXTENSION pg_sift_test" "CREATE EXTENSION pg_walinspect" \
	"CREATE FUNCTION lease_x(int4) RETURNS int8 AS 'pg_sift', 'sift_test_lease_block0_x' LANGUAGE C" \
	"CREATE FUNCTION lease_maint(oid, int8, int4) RETURNS int8 AS 'pg_sift', 'sift_test_lease_maint' LANGUAGE C" \
	"CREATE FUNCTION lease_mix(oid, int8, int4) RETURNS int8 AS 'pg_sift', 'sift_test_lease_mix' LANGUAGE C" \
	"CREATE FUNCTION lease_stat(text) RETURNS int8 AS 'pg_sift', 'sift_test_lease_stat' LANGUAGE C" \
	"CREATE TABLE t (k int PRIMARY KEY, v int) USING sift_table" "SELECT sift._test_f2_split_many('t', 1, NULL, $((WRITERS - 1)), 10000)" >/dev/null || bail "setup failed"
D=$(dir ml)

# a gate holds the sessions until all of them wait, so that they run their statements at the same time
gate_run() {
	local n=$1 statement=$2 i pids=()
	tap_tx_hold ml gate "SELECT pg_advisory_lock(7)"
	for i in $(seq "$n"); do
		sql ml "SELECT pg_advisory_lock_shared(7)" "$statement" >"$D/gate.$i.out" 2>&1 &
		pids+=($!)
	done
	tap_tx_wait_blocked ml "$n"
	tap_tx_release gate
	wait "${pids[@]}"
}

for w in $(seq 0 $((WRITERS - 1))); do
	cat >"$D/w$w.sql" <<SQL
DO \$\$ BEGIN FOR i IN 1..$INSERTS LOOP INSERT INTO t SELECT $((w * 10000)) + (i - 1) * $ROWS + g, i FROM generate_series(1, $ROWS) g; COMMIT; END LOOP; END \$\$
SQL
done
tap_tx_hold ml gate "SELECT pg_advisory_lock(7)"
for w in $(seq 0 $((WRITERS - 1))); do
	sql ml "SELECT pg_advisory_lock_shared(7)" "\\i $D/w$w.sql" "SELECT lease_x(-1), lease_x(0), lease_x(1), lease_x(2), lease_x(3)" >"$D/w$w.out" 2>&1 &
done
tap_tx_wait_blocked ml "$WRITERS"
tap_tx_release gate
wait
LOCKS=$(tail -qn1 "$D"/w*.out | awk -F'|' '{for (i = 1; i <= NF; i++) s[i] += $i} END {print s[1]; printf "counters %d allocator %d vblocks %d publish %d\n", s[2], s[3], s[4], s[5] > "/dev/stderr"}' 2>"$D/sites")
diag "exclusive locks of Block 0 taken by $((WRITERS * INSERTS)) commits of a part: $LOCKS ($(cat "$D/sites"))"
check "$((WRITERS * INSERTS)) commits of a part take fewer than $LOCK_LIMIT exclusive locks of Block 0" test "$LOCKS" -lt "$LOCK_LIMIT"
check "every row is there" test "$(sql ml "SELECT count(*) FROM t")" -eq $((WRITERS * INSERTS * ROWS))
out=$(sql ml "SELECT * FROM sift.check('t')" 2>&1)
check "the table checks" test -z "$out"
sql ml "VACUUM t" >/dev/null
out=$(sql ml "SELECT * FROM sift.check('t')" 2>&1)
check "the allocator holds no page of metadata that nothing names once the file is cut" test -z "$out"

elapsed_max() { sort -n "$@" | tail -1; }
rate() { echo $(($1 * $2 * 1000000 / ($3 + 1))); }
ITERS=200000
sql ml "SELECT lease_maint(4000001, 1000, 0)" >/dev/null
one=$(sql ml "SELECT lease_maint(4000001, $ITERS, 0)" | tail -1)
gate_run "$CLIENTS" "SELECT lease_maint(4000001, $ITERS, 0)"
many=$(elapsed_max < <(for i in $(seq "$CLIENTS"); do tail -1 "$D/gate.$i.out"; done))
diag "queue: 1 session $(rate 1 $ITERS "$one") requests/s, $CLIENTS sessions $(rate "$CLIENTS" $ITERS "$many") requests/s"
check "$CLIENTS sessions that ask the queue for what it holds take under 8 times (times the $TURNS turns of the cores) what one session takes for its own" test "$((many / (one + 1)))" -lt $((8 * TURNS))
gate_run "$CLIENTS" "SELECT lease_maint(4100000, $ITERS, 1)"
heat=$(elapsed_max < <(for i in $(seq "$CLIENTS"); do tail -1 "$D/gate.$i.out"; done))
diag "heat: $CLIENTS sessions $(rate "$CLIENTS" $ITERS "$heat") notes/s"

MIX=400000
sql ml "SELECT lease_mix(900001, 100, 0)" >/dev/null
one=$(sql ml "SELECT lease_mix(900001, $MIX, 0)" | tail -1)
gate_run "$CLIENTS" "SELECT lease_mix(900002, $MIX, 0)"
many=$(elapsed_max < <(for i in $(seq "$CLIENTS"); do tail -1 "$D/gate.$i.out"; done))
diag "manifests: 1 session $(rate 1 $MIX "$one") rounds/s, $CLIENTS sessions $(rate "$CLIENTS" $MIX "$many") rounds/s"
check "$CLIENTS sessions that read and publish the manifests take under 12 times (times the $TURNS turns of the cores) what one session takes for its own" test "$((many / (one + 1)))" -lt $((12 * TURNS))
copies0=$(sql ml "SELECT lease_stat('list_copies')")
tap_tx_hold ml mix "SELECT lease_mix(900003, 1, 0)"
gate_run "$CLIENTS" "SELECT lease_mix(900003, $MIX, 0)"
tap_tx_release mix
diag "lists copied: $(($(sql ml "SELECT lease_stat('list_copies')") - copies0))"
check "the list of a partition is made once, by one of the readers that ask for it" test "$(($(sql ml "SELECT lease_stat('list_copies')") - copies0))" -eq 32
sql ml "SELECT sift._test_shm_manifest_invalidate(900001, false)" "SELECT sift._test_shm_manifest_invalidate(900002, false)" "SELECT sift._test_shm_manifest_invalidate(900003, false)" >/dev/null
finish
