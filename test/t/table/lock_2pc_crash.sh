#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The row locks of a prepared transaction of a sift_table are the lock manager's after a crash, as a heap's tuple locks are: four prepared transactions hold a row each in one of the four modes
# (FOR KEY SHARE, FOR SHARE, FOR NO KEY UPDATE, FOR UPDATE) while a session that holds another row in the partition keeps them from taking the partition, so that each one's lock is a row entry in shared
# memory, which the crash empties. After the restart a request of each mode conflicts with each prepared lock as the matrix of the heap says, a wait ends with ROLLBACK PREPARED or COMMIT PREPARED,
# and a SKIP LOCKED request that skips a row holds no lock on it.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 12
node_init lc 52 "autovacuum = off
sift.async_maintenance = off
${SIFT_TESTING:+sift.test_create_format1 = off}
max_prepared_transactions = 8
deadlock_timeout = 200ms"
node_start lc || bail "start failed"
D=$(dir lc)
EXT=pg_sift
[[ -z ${SIFT_TESTING:-} ]] || EXT="pg_sift_test CASCADE"
sql lc "CREATE EXTENSION $EXT" \
	"CREATE TABLE orders (id int PRIMARY KEY, st text) USING sift_table" \
	"INSERT INTO orders SELECT g, 'new' FROM generate_series(1, 20) g" \
	"SELECT busy FROM sift.table_maintain('orders')" >/dev/null || bail "setup failed"

MODES=("KEY SHARE" "SHARE" "NO KEY UPDATE" "UPDATE")
WAITS=(0001 0011 0111 1111)

start_session() {
	rm -f "$D/locked" "$D/release"
	{ echo "BEGIN;"; echo "$1"; echo "\\! touch '$D/locked'"; echo "\\! for i in \$(seq 1200); do [ -e '$D/release' ] && break; sleep 0.05; done"; echo "ROLLBACK;"; } | sql lc >"$D/$2.out" 2>&1 &
	session=$!
	wait_for "[[ -e $D/locked ]]" 60 || bail "the session $2 did not take its lock"
}
end_session() { touch "$D/release"; wait "$session"; }
verdict() { if out=$(sql lc "SELECT id FROM orders WHERE id = $1 FOR $2 NOWAIT" 2>&1); then echo ok; else echo busy; fi; }

start_session "SELECT id FROM orders WHERE id = 20 FOR UPDATE;" holder
for i in 0 1 2 3; do
	sql lc "BEGIN" "SELECT id FROM orders WHERE id = $((i + 1)) FOR ${MODES[i]}" "PREPARE TRANSACTION 'p$((i + 1))'" >/dev/null || bail "prepare p$((i + 1)) failed"
done
sql lc "BEGIN" "UPDATE orders SET st = 'prepared' WHERE id = 5" "PREPARE TRANSACTION 'p5'" >/dev/null || bail "prepare p5 failed"
end_session
out=$(sql lc "SELECT count(*) FROM pg_prepared_xacts")
check "five transactions are prepared" test "$out" -eq 5

crash lc
node_start lc || bail "restart failed"
died=$(grep -ac 'terminated by signal' "$D/postgres.log")

for i in 0 1 2 3; do
	got=""
	want=""
	for r in 0 1 2 3; do
		got+="$(verdict $((i + 1)) "${MODES[r]}") "
		want+="$([[ ${WAITS[i]:r:1} == 1 ]] && echo busy || echo ok) "
	done
	out=$got
	check "after the restart a request is refused by the prepared FOR ${MODES[i]} as the heap's tuple lock refuses it" test "$got" == "$want"
done

out=$(verdict 10 UPDATE)
check "a row no prepared transaction holds is free" test "$out" == ok
out=$(sql lc "SET lock_timeout = '300ms'" "UPDATE orders SET st = 'lost' WHERE id = 4" 2>&1)	# timeout-ok: the lock_timeout is the product's bound on the wait for the prepared transaction
check "an UPDATE of a row a prepared transaction holds FOR UPDATE waits for it" grep -q 'lock timeout' <<<"$out"
out=$(sql lc "SELECT count(*) FROM (SELECT id FROM orders WHERE id = 5 FOR UPDATE SKIP LOCKED) s")
check "SKIP LOCKED skips a row a prepared transaction changed" test "$out" -eq 0

start_session "SELECT id FROM orders WHERE id = 5 FOR UPDATE SKIP LOCKED;" skipper
sql lc "ROLLBACK PREPARED 'p5'" >/dev/null
out=$(verdict 5 UPDATE)
end_session
check "the session that skipped the row holds no lock on it" test "$out" == ok

sql lc "ROLLBACK PREPARED 'p4'" >/dev/null
out=$(sql lc "SET lock_timeout = '300ms'" "UPDATE orders SET st = 'free' WHERE id = 4" 2>&1)	# timeout-ok: the lock_timeout is the product's bound on the wait, which a freed row does not use
check "ROLLBACK PREPARED frees the row for the waiting UPDATE" test -z "$out"
sql lc "COMMIT PREPARED 'p3'" >/dev/null
out=$(verdict 3 UPDATE)
check "COMMIT PREPARED frees the row" test "$out" == ok
check "no backend died after the restart" test "$(grep -ac 'terminated by signal' "$D/postgres.log")" -eq "$died"
sql lc "ROLLBACK PREPARED 'p1'" "ROLLBACK PREPARED 'p2'" >/dev/null
finish
