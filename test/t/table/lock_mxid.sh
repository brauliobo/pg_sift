#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The holders of a row past the eight an entry has room for are the members of a MultiXactId. Ten sessions hold one row FOR KEY SHARE: a request that conflicts with them is refused and one that does
# not is granted, and when the last of them ends the entry of the row is gone from shared memory, not kept for a MultiXactId nobody holds. A transaction that locks the same row in fifteen
# savepoints it releases is one holder of it, and allocates no MultiXactId.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 8
node_init lm 53 "autovacuum = off
sift.async_maintenance = off
${SIFT_TESTING:+sift.test_create_format1 = off}"
node_start lm || bail "start failed"
D=$(dir lm)
EXT=pg_sift
[[ -z ${SIFT_TESTING:-} ]] || EXT="pg_sift_test CASCADE"
sql lm "CREATE EXTENSION $EXT" \
	"CREATE TABLE parent (id int PRIMARY KEY, v int) USING sift_table" \
	"INSERT INTO parent SELECT g, 0 FROM generate_series(1, 10) g" >/dev/null || bail "setup failed"

entries() { sql lm "SELECT in_table + in_chunks FROM sift._test_rowlock_stats()"; }
next_mxid() { sql lm "CHECKPOINT" "SELECT next_multixact_id FROM pg_control_checkpoint()" | tail -n 1; }

holders=()
rm -f "$D"/held.* "$D/release"
for k in 1 2 3 4 5 6 7 8 9 10; do
	{ echo "BEGIN;"; echo "SELECT id FROM parent WHERE id = 1 FOR KEY SHARE;"; echo "\\! touch '$D/held.$k'"
	  echo "\\! for i in \$(seq 1200); do [ -e '$D/release' ] && break; sleep 0.05; done"; echo "COMMIT;"; } | sql lm >"$D/holder.$k.out" 2>&1 &
	holders+=($!)
done
all_held() { [[ $(ls "$D"/held.* 2>/dev/null | wc -l) -eq 10 ]]; }
wait_for all_held 60 || bail "the ten holders did not take the row"

out=$(sql lm "SELECT id FROM parent WHERE id = 1 FOR UPDATE NOWAIT" 2>&1)
check "a request that conflicts with the ten holders is refused" grep -q 'could not obtain lock' <<<"$out"
out=$(sql lm "SELECT id FROM parent WHERE id = 1 FOR KEY SHARE NOWAIT" 2>&1)
check "a request that does not conflict with them is granted" test "$out" -eq 1
touch "$D/release"
wait "${holders[@]}"
check "the ten holders end without an error" test -z "$(grep -h ERROR "$D"/holder.*.out)"
if [[ -n ${SIFT_TESTING:-} ]]; then
	out=$(entries)
	check "the entry of the row is gone when its last holder ends" test "$out" -eq 0
else
	skip 1 "the entries of the row locks are counted by the testing flavour (sift._test_rowlock_stats)"
fi

before=$(next_mxid)
sql lm "DO \$\$ BEGIN FOR i IN 1..15 LOOP BEGIN PERFORM id FROM parent WHERE id = 2 FOR UPDATE; EXCEPTION WHEN division_by_zero THEN NULL; END; END LOOP; END \$\$" \
	>/dev/null || bail "the savepoint loop failed"
after=$(next_mxid)
check "fifteen released savepoints on one row allocate no MultiXactId" test "$after" -eq "$before"

# 48 clients take one row FOR KEY SHARE at once, five rounds each: the holders past the room of the entry each grow its MultiXactId once, not again for every one that grew it first
before=$(next_mxid)
sess_open lm gate
sess_say gate "SELECT pg_advisory_lock(42); \\echo r0"
wait_for "grep -q '^r0\$' $D/gate.out" 60 || bail "the gate was not taken"
clients=()
for k in $(seq 48); do
	for r in 1 2 3 4 5; do
		echo "BEGIN;"; echo "SELECT pg_advisory_xact_lock_shared(42);"
		echo "SELECT id FROM parent WHERE id = 3 FOR KEY SHARE;"; echo "SELECT pg_sleep(0.15);"; echo "COMMIT;"
	done | sql lm >"$D/hot.$k.out" 2>&1 &
	clients+=($!)
done
gated() { [[ $(sql lm "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND mode = 'ShareLock' AND NOT granted") -eq 48 ]]; }
start=$(date +%s.%N)
for r in 1 2 3 4 5; do
	wait_for gated 60 || bail "the 48 clients did not reach round $r"
	sess_say gate "SELECT pg_advisory_unlock(42); SELECT pg_advisory_lock(42);"
done
wait "${clients[@]}"
sess_say gate "SELECT pg_advisory_unlock(42);"
sess_close gate
elapsed=$(echo "$(date +%s.%N) - $start" | bc)
after=$(next_mxid)
out="$((after - before)) MultiXactIds in $elapsed s"
diag "$out"
check "48 concurrent holders of one row allocate at most one MultiXactId each a round" test $((after - before)) -le 240
check "and end in 5 s without an error" awk -v t="$elapsed" -v e="$(grep -hc ERROR "$D"/hot.*.out | paste -sd+ - | bc)" 'BEGIN { exit !(t < 5 && e == 0) }'
check "no backend died" test "$(grep -ac 'terminated by signal' "$D/postgres.log")" -eq 0
finish
