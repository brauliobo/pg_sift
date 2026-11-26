#!/usr/bin/env bash
# sift.check reads the segments of a column index one at a time, each under the maintenance lock, and the lock is let go between two of them. The index's VACUUM clears the entry of a dead
# row before the heap reuses its address, so a row that the check read live in a segment before the VACUUM and again, at the address a new row took, in a segment after it is not in the
# index twice. The check stops at its test gate between the segments (an advisory lock a session holds, so no timing is involved), the VACUUM and the INSERT run, and the check goes on:
# it finds nothing wrong. With the mutant that makes the VACUUM of the index clear nothing the same two statements leave a row in the index twice, and the check finds it, whether the
# directory moved while it waited or not.
# Format 1 (col/v1/verify.c): the check walks a directory of segments under the maintenance lock; a Format 2.0 index has no such directory.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 12
node_init cr 63 "autovacuum = off
sift.async_maintenance = off
sift.test_create_format1 = on"
node_start cr || bail "start failed"
D=$(dir cr)
sql cr "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "setup failed"

waiting() { sql cr "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted"; }
await() {
	for _ in $(seq 600); do [[ $(waiting) -ge 1 ]] && return 0; sleep 0.05; done
	return 1
}
start_holder() {
	rm -f "$D/locked" "$D/release"
	{ echo "SELECT pg_advisory_lock(20983, ${1:-18});"; echo "\\! touch '$D/locked'"; echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"; } | sql cr >/dev/null &
	holder=$!
	until [[ -e $D/locked ]]; do sleep 0.05; done
}

# fresh: a table whose rows sit in an immutable segment and a write buffer, with a row of the segment deleted and not yet vacuumed
fresh() {
	sql cr "DROP TABLE IF EXISTS r" "CREATE TABLE r (id int PRIMARY KEY, g int)" "INSERT INTO r SELECT i, i % 7 FROM generate_series(1, 200) i" \
		"CREATE INDEX r_col ON r USING sift_col (id, g)" "SELECT sift.col_promote('r_col')" "INSERT INTO r VALUES (500, 1)" "DELETE FROM r WHERE id = 150" >/dev/null || bail "fresh failed"
}

# gated_check STATEMENT...: sift.check of the index stops after its first segment, the statements run in a session of their own, the check goes on; its problems are in $found
gated_check() {
	start_holder
	sql cr "LOAD 'pg_sift'" "SET sift.test_check_gate = on" "SELECT problem || ' ' || coalesce(detail, '') FROM sift.check('r_col')" >"$D/found" 2>&1 &
	local checker=$!
	await || bail "the check did not reach its gate"
	sql cr "LOAD 'pg_sift'" "$@" >/dev/null || bail "the statements of the gate failed: $*"
	touch "$D/release"
	wait "$holder" "$checker"
	rm -f "$D/release"
	found=$(cat "$D/found")
}

# store_gated_check MUTANT STATEMENT: the check stops in its walk of the blocks of the index, after it read the directory and before it reads the free list; the statement runs in a session
# of its own until it ends or waits for a lock, the check goes on. $blocked is the number of sessions that waited for the maintenance lock, the problems of the check are in $found
store_gated_check() {
	local mutant=$1 checker other
	shift
	start_holder 19
	sql cr "LOAD 'pg_sift'" "SET sift.test_check_gate = on" "SET sift.test_mutant = $mutant" "SELECT problem || ' ' || coalesce(detail, '') FROM sift.check('r_col')" >"$D/found" 2>&1 &
	checker=$!
	await || bail "the check did not reach its gate"
	sql cr "LOAD 'pg_sift'" "$@" >/dev/null &
	other=$!
	until ! kill -0 "$other" 2>/dev/null || [[ $(sql cr "SELECT count(*) FROM pg_locks WHERE locktype = 'page' AND NOT granted") -ge 1 ]]; do sleep 0.05; done
	blocked=$(sql cr "SELECT count(*) FROM pg_locks WHERE locktype = 'page' AND NOT granted")
	touch "$D/release"
	wait "$holder" "$checker" "$other"
	rm -f "$D/release"
	found=$(cat "$D/found")
}

fresh
gated_check "VACUUM r" "INSERT INTO r VALUES (1000, 1)"
check "a row took the address of a dead one the check had read live: nothing is wrong" test -z "$found"
check "the new row is at the address of the dead one" test "$(sql cr "SELECT ctid FROM r WHERE id = 1000")" == "(0,150)"

fresh
gated_check "SELECT 1"
check "control: the check without a change between its segments finds nothing" test -z "$found"

fresh
sql cr "LOAD 'pg_sift'" "SET sift.test_mutant = 3" "VACUUM r" "INSERT INTO r VALUES (1000, 1)" >/dev/null
out=$(sql cr "SELECT problem, detail FROM sift.check('r_col')")
check "mutant: a VACUUM that clears nothing leaves a row in the index twice, and the check finds it" grep -q "is live in the index twice" <<<"$out"
out=$(sql cr "SELECT sift.col_verify('r_col')" 2>&1)
check "mutant: and so does sift.col_verify" grep -q "is live in the index twice" <<<"$out"

fresh
sql cr "LOAD 'pg_sift'" "SET sift.test_mutant = 3" "VACUUM r" "INSERT INTO r VALUES (1000, 1)" >/dev/null
gated_check "SELECT sift.col_promote('r_col')"
out=$found
check "mutant: the check finds it after the directory moved while it waited" grep -q "is live in the index twice" <<<"$found"
out=
# the directory and the free list of the check are one view: a promotion between its two reads would free the blocks of the write buffer that the directory it read still lists
fresh
store_gated_check 0 "SELECT sift.col_promote('r_col')"
out=$found
check "a promotion waits for the check while it reads the blocks of the index, which it finds in order" test "$blocked" -eq 1 -a -z "$found"
out=
check "... and the promotion ran when the check let go" test "$(sql cr "SELECT count(*) FROM sift.col_segments('r_col') WHERE kind = 'immutable'")" -eq 2

fresh
store_gated_check 4 "SELECT sift.col_promote('r_col')"
check "mutant: a check that takes no maintenance lock for its blocks lets the promotion through" test "$blocked" -eq 0
out=$found
check "mutant: and finds a block referenced by a segment and on the free list" grep -q "is referenced twice" <<<"$found"
out=

check "no lock was released that the backend did not own" test "$(grep -c "don't own a lock" "$D/postgres.log")" -eq 0
check "no check left a maintenance lock behind" test "$(sql cr "SELECT count(*) FROM pg_locks WHERE locktype = 'page' AND granted")" == 0
finish
