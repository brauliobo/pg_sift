#!/usr/bin/env bash
# Metadata pages released while a REPEATABLE READ snapshot pins the horizon cannot mature, so the slabs that hold them only grow: past the slabs the root (block 1) has room for, the
# allocator keeps the rest on a chain of SIFT_PT_FSM_SLABS pages (core/f2/fsm/chain.c) instead of refusing every write with "has the most metadata slabs". The chain is whole for
# sift.check while it is live and after a crash in the middle of its growth, and once the snapshot ends a VACUUM gives the idle slabs and the pages of the chain back and the file shrinks to its size before.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "${SIFT_REPO:?}/test/t/lib/common.bash"

PAGES=110000
ROOT_SLABS=200
POOL_SPAN=32
CHAIN_SLABS=203
plan 14

p=fsc
node_init "$p" 87 "autovacuum = off
max_parallel_maintenance_workers = 0
sift.async_maintenance = off
sift.on_corruption = error"
node_start "$p" || bail "start failed"
sql "$p" "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" "SET sift.test_create_format1 = off" \
	"CREATE TABLE h (id int, a bigint)" "INSERT INTO h SELECT g, g FROM generate_series(1, 20000) g" \
	"CREATE INDEX h_col ON h USING sift_col (id, a)" >/dev/null || bail "setup failed"

meta_total() { sql "$p" "SELECT split_part(sift._test_f2_fsm('h_col'), '/', 3)"; }
problems() { sql "$p" "SELECT problem FROM sift.check('h_col', true)" 2>&1; }
file_blocks() { sql "$p" "SELECT pg_relation_size('h_col') / current_setting('block_size')::int"; }
churn() { # metadata pages allocated and released at once, by a transaction a snapshot older than it keeps from maturing
	sql "$p" "SELECT count(sift._test_f2_release('h_col', b, 1, 'now')) FROM (SELECT sift._test_f2_meta_alloc('h_col') b FROM generate_series(1, $1)) x" 2>&1
}

hold() { # a REPEATABLE READ snapshot that pins the horizon until the session ends
	coproc HOLD { psql -X -At -q -v ON_ERROR_STOP=1 -h "$(node_sock "$p")" -p "$(port "$p")" -U postgres -d "$(node_db "$p")"; }
	HOLDER=$HOLD_PID
	printf '%s\n' "BEGIN ISOLATION LEVEL REPEATABLE READ;" "SELECT count(*) FROM h;" >&"${HOLD[1]}"
	read -r -u "${HOLD[0]}" _
}
crash_restart() {
	crash "$p"
	eval "exec ${HOLD[1]}>&-"
	wait "$HOLDER" "$@" 2>/dev/null
	node_start "$p" || bail "the restart failed"
}
rows_agree() { test "$(sql "$p" "SELECT count(*), sum(a) FROM h")" == "$(sql "$p" "SET sift.enable_agg = off" "SELECT count(*), sum(a) FROM h")"; }

sql "$p" "VACUUM h" >/dev/null
size0=$(file_blocks)

# 1. a crash in the middle of a churn, while the chain grows: the replay leaves it whole, the sweep frees what the crash left in flight and finds every block owned once
hold
churn $((PAGES * 2)) >/dev/null 2>&1 &
churner=$!
wait_for "(( \$(file_blocks) > ROOT_SLABS * 256 + 30000 ))" 600 || bail "the churn did not pass the root"
crash_restart "$churner"
out=$(sql "$p" "SELECT sift._test_f2_sweep('h_col')" 2>&1)
check "the sweep after the crash frees what the churn held in flight ($out blocks)" test "$out" -le $((POOL_SPAN * 4))
out=$(sql "$p" "SELECT sift._test_f2_sweep('h_col')" 2>&1)
check "a second sweep finds no orphan" test "$out" == 0
out=$(problems)
check "sift.check is clean after the crash" test -z "$out"
sql "$p" "VACUUM h" >/dev/null

# 2. the pinned horizon: the slabs pass the room of the root by more than a page of the chain and the writes go on; a crash after it replays the chain whole
hold
out=$(churn "$PAGES")
check "$PAGES metadata pages released under a pinned horizon are allocated without an error" test "$out" == "$PAGES"
total=$(meta_total)
check "the slabs hold more than the root and a page of the chain have room for ($total metadata blocks)" test "$total" -gt $(((ROOT_SLABS + CHAIN_SLABS) * 256))
out=$(sql "$p" "INSERT INTO h SELECT g, g FROM generate_series(20001, 21000) g" "UPDATE h SET a = a + 1 WHERE id % 10 = 0" "DELETE FROM h WHERE id % 13 = 0" 2>&1)
check "INSERT, UPDATE and DELETE still commit" test -z "$out"
out=$(problems)
check "sift.check is clean while the chain is live" test -z "$out"
crash_restart
out=$(problems)
check "sift.check is clean after a crash that replays the chain" test -z "$out"
check "the rows are the ones that were committed" rows_agree

# 3. the snapshot is gone: VACUUM gives the slabs and the chain back and the file shrinks
sql "$p" "VACUUM h" "VACUUM h" >/dev/null
out=$(sql "$p" "SELECT sift._test_f2_tail_relocate('h_col')" "VACUUM h" 2>&1 >/dev/null)
check "the move of the slabs down runs" test -z "$out"
total=$(meta_total)
check "the slabs fit the root again ($total metadata blocks)" test "$total" -le $((ROOT_SLABS * 256))
size=$(file_blocks)
check "the file shrinks back near its size before the churn ($size0 -> $size blocks)" test "$size" -le $((size0 * 2 + 1024))
out=$(problems)
check "sift.check is clean after the shrink" test -z "$out"
out=$(churn 2000)
check "the allocator serves metadata pages after the shrink" test "$out" == 2000

finish
