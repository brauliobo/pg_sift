#!/usr/bin/env bash
# A maintenance operation holds no lock that keeps writers or readers out of the table while it works, and no path releases a lock it does not own. Each case stops an operation inside its
# work at a test gate (an advisory lock a test holds, so no timing is involved) and looks at pg_locks: the operation holds no relation or extension lock of a mode that conflicts with
# RowExclusiveLock (ShareLock, ShareRowExclusiveLock, ExclusiveLock, AccessExclusiveLock) on the table or its indexes; then an INSERT, an UPDATE, a DELETE and a SELECT run on the table under
# lock_timeout and none waits. The operations run again in a savepoint, an exception block and a cursor, whose resource owners are not the transaction's: the leak gate of finish fails
# the test when a log has "you don't own a lock". A case is a row of CASES: a lock held in a transaction by the operation, or one that waits out a gate, is a defect to fix, not a row to loosen.
. "$(dirname "$0")/lib.bash"

# name | table | gate setting | lock the holder takes | operation
CASES=(
	"bm25 VACUUM inside the bulk delete|b|SET sift.test_bulkdelete_gate = 7|SELECT pg_advisory_lock(7)|VACUUM b"
	"bm25 VACUUM before the cleanup|b|SET sift.test_cleanup_gate = on|SELECT pg_advisory_lock(20983, 2)|VACUUM b"
	"bm25 merge releasing extents|b|SET sift.test_release_gate = on|SELECT pg_advisory_lock(20983, 6)|SELECT sift.merge('b_idx', true)"
	"col merge releasing extents|c|SET sift.test_release_gate = on|SELECT pg_advisory_lock(20983, 6)|SELECT sift.col_merge('c_idx', true)"
	"sift_table VACUUM releasing extents|t|SET sift.test_release_gate = on|SELECT pg_advisory_lock(20983, 6)|VACUUM t"
)
plan $(( ${#CASES[@]} * 4 + 6 ))
node_init lm 61 "autovacuum = off
sift.async_maintenance = off"
node_start lm || bail "start failed"
D=$(dir lm)
sql lm "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE b (id int, body text)" \
	"CREATE TABLE c (id int, grp int, amount numeric(10,2), pad text)" \
	"CREATE TABLE t (id int, grp int, pad text) USING sift_table" >/dev/null || bail "setup failed"

fill() { # the segments an operation has to work on: several sealed buffers, then rows deleted so that VACUUM and the merges have something to do
	sql lm "SET sift.async_maintenance = off" \
		"DROP INDEX IF EXISTS b_idx" "DROP INDEX IF EXISTS c_idx" "TRUNCATE b, c, t" \
		"CREATE INDEX b_idx ON b USING sift (body)" \
		"CREATE INDEX c_idx ON c USING sift_col (id, grp, amount, pad) WITH (write_buffer_limit = 1024)" >/dev/null || bail "indexes failed"
	for k in 1 2 3; do
		sql lm "SET sift.async_maintenance = off" \
			"INSERT INTO b SELECT $k * 100000 + g, 'word w' || g || ' x' || (g % 31) FROM generate_series(1, 4000) g" \
			"INSERT INTO c SELECT $k * 100000 + g, g % 50, g % 997, md5(g::text) FROM generate_series(1, 4000) g" \
			"INSERT INTO t SELECT $k * 100000 + g, g % 50, md5(g::text) FROM generate_series(1, 4000) g" \
			"SELECT sift.promote('b_idx')" "SELECT sift.col_promote('c_idx')" >/dev/null || bail "fill failed"
	done
	sql lm "DELETE FROM b WHERE id % 5 = 0" "DELETE FROM c WHERE id % 5 = 0" "DELETE FROM t WHERE id % 5 = 0" >/dev/null || bail "delete failed"
}
new_rows() {
	case $1 in
		b) echo "INSERT INTO b SELECT 900000000 + g, 'w' || g FROM generate_series(1, 10) g" ;;
		c) echo "INSERT INTO c SELECT 900000000 + g, g, 1, 'x' FROM generate_series(1, 10) g" ;;
		t) echo "INSERT INTO t SELECT 900000000 + g, g, 'x' FROM generate_series(1, 10) g" ;;
	esac
}
waiting() { sql lm "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted"; }
await() {
	for _ in $(seq 600); do [[ $(waiting) -ge 1 ]] && return 0; sleep 0.05; done
	return 1
}
conflicting() { # the relation and extension locks of the backend that waits at the gate, of a mode that conflicts with RowExclusiveLock
	sql lm "SELECT l.locktype || ' ' || l.mode || ' ' || coalesce(l.relation::regclass::text, '')
		FROM pg_locks l WHERE l.granted AND l.locktype IN ('relation', 'extend')
		AND l.mode IN ('ShareLock', 'ShareRowExclusiveLock', 'ExclusiveLock', 'AccessExclusiveLock')
		AND l.pid = (SELECT pid FROM pg_locks WHERE locktype = 'advisory' AND NOT granted LIMIT 1)"
}

held() { # every lock of the backend that waits at the gate, besides the gate: what the operation holds while it is inside its work
	sql lm "SELECT l.locktype || ' ' || l.mode || ' ' || coalesce(l.relation::regclass::text, l.page::text, '')
		FROM pg_locks l WHERE l.granted AND l.locktype IN ('relation', 'extend', 'page', 'tuple', 'object') AND l.relation IS DISTINCT FROM 'pg_locks'::regclass
		AND l.pid = (SELECT pid FROM pg_locks WHERE locktype = 'advisory' AND NOT granted LIMIT 1) ORDER BY 1"
}
release_gate() { touch "$D/release"; wait "$1" "$2"; rm -f "$D/release"; }
start_holder() { # SQL: a session that takes the lock of the gate and keeps it until release
	rm -f "$D/locked" "$D/release"
	{ echo "$1;"; echo "\\! touch '$D/locked'"; echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"; } | sql lm >/dev/null &
	holder=$!
	until [[ -e $D/locked ]]; do sleep 0.05; done
}

start_holder "SELECT pg_advisory_lock(20983, 99)"
sql lm "BEGIN" "LOCK TABLE b IN SHARE ROW EXCLUSIVE MODE" "SELECT pg_advisory_lock(20983, 99)" "COMMIT" >/dev/null 2>&1 &
op_pid=$!
await || bail "the control did not reach its gate"
check "control: the probe sees the ShareRowExclusiveLock of a session at its gate" test "$(conflicting)" == "relation ShareRowExclusiveLock b"
sql lm "SET lock_timeout = '500ms'" "$(new_rows b)" >/dev/null 2>&1
check "control: an INSERT waits for that lock" test $? -ne 0
release_gate "$holder" "$op_pid"

for row in "${CASES[@]}"; do
	IFS='|' read -r name table gate hold op <<<"$row"
	fill
	start_holder "$hold"
	sql lm "LOAD 'pg_sift'" "SET sift.async_maintenance = off" "$gate" "$op" >"$D/op" 2>&1 &
	op_pid=$!
	if await; then
		out=$(conflicting)
		diag "$name: holds $(held | paste -sd, -)"
		check "$name: holds no lock that conflicts with RowExclusiveLock" test -z "$out"
		sql lm "SET lock_timeout = '3s'" "$(new_rows "$table")" >"$D/w1" 2>&1
		check "$name: INSERT does not wait" test $? -eq 0
		sql lm "SET lock_timeout = '3s'" "UPDATE $table SET id = id + 1 WHERE id = 100001" "DELETE FROM $table WHERE id = 100002" >"$D/w2" 2>&1
		check "$name: UPDATE and DELETE do not wait" test $? -eq 0
		sql lm "SET lock_timeout = '3s'" "SELECT count(*) FROM $table" >"$D/w3" 2>&1
		check "$name: SELECT does not wait" test $? -eq 0
	else
		not_ok "$name: the operation reached its gate"; diag "$(cat "$D/op")"
		skip 3 "$name: no gate"
	fi
	release_gate "$holder" "$op_pid"
done

fill
sql lm "SET sift.async_maintenance = off" "BEGIN" "SAVEPOINT s" "SELECT sift.promote('b_idx')" "SELECT sift.merge('b_idx', true)" "SELECT sift.col_promote('c_idx')" \
	"SELECT sift.col_merge('c_idx', true)" "ROLLBACK TO s" "SELECT sift.merge('b_idx', true)" "SELECT sift.col_merge('c_idx', true)" "COMMIT" >"$D/sp" 2>&1
check "maintenance in a savepoint, rolled back, then again" test $? -eq 0
sql lm "SET sift.async_maintenance = off" "DO \$\$ BEGIN
	BEGIN PERFORM sift.merge('b_idx', true); PERFORM sift.col_merge('c_idx', true); RAISE EXCEPTION 'undo'; EXCEPTION WHEN raise_exception THEN NULL; END;
	PERFORM sift.merge('b_idx', true); PERFORM sift.col_merge('c_idx', true);
END \$\$" "VACUUM b, c, t" >"$D/ex" 2>&1
check "maintenance in an exception block, undone, then VACUUM" test $? -eq 0
sql lm "SET sift.async_maintenance = off" "BEGIN" "DECLARE cur CURSOR WITH HOLD FOR SELECT id FROM c WHERE id > 0" "FETCH 5 FROM cur" "SELECT sift.col_merge('c_idx', true)" "COMMIT" \
	"FETCH 5 FROM cur" "CLOSE cur" "VACUUM c" >"$D/cu" 2>&1
check "maintenance around a held cursor" test $? -eq 0
check "no lock was released that the backend did not own" test "$(grep -c "don't own a lock" "$(dir lm)/postgres.log")" -eq 0
finish
