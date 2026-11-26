#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# A modify of a row whose column delta chain is too long for the chain cache asks the foreign log of the partition (table/f2/col_delta/foreign.c), which follows the chain from one statement
# to the next: a session that updates rows of two such tables in turn reads the pages each chain gained (sift._test_chain_cache_pages), not both chains again at every turn, and the rows are exact;
# so it does while a snapshot older than its updates is open, the records of its own transactions joining the log once each one ended instead of the chains being read again.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 5
node_init fl 0 "autovacuum = off
sift.async_maintenance = off"
node_start fl || bail "start failed"
sql fl "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE p (k int PRIMARY KEY, v int) USING sift_table" \
	"CREATE TABLE q (k int PRIMARY KEY, v int) USING sift_table" \
	"INSERT INTO p SELECT g, 0 FROM generate_series(1, 20000) g" \
	"INSERT INTO q SELECT g, 0 FROM generate_series(1, 20000) g" >/dev/null || bail "setup failed"
for i in 1 2 3 4 5 6; do
	sql fl "UPDATE p SET v = v + 1" "UPDATE q SET v = v + 1" >/dev/null || bail "the churn failed"
done

TURNS=20
stmts=("SELECT sift._test_chain_cache_pages()" "UPDATE p SET v = v + 1 WHERE k = 1" "UPDATE q SET v = v + 1 WHERE k = 1" "SELECT sift._test_chain_cache_pages()")
for ((i = 0; i < TURNS; i++)); do stmts+=("UPDATE p SET v = v + 1 WHERE k = 1" "UPDATE q SET v = v + 1 WHERE k = 1"); done
stmts+=("SELECT sift._test_chain_cache_pages()")
read -r p0 p1 p2 < <(sql fl "${stmts[@]}" | paste -sd' ' -)
first=$((p1 - p0))
turns=$((p2 - p1))
out="the first update of each table reads $first pages, $TURNS turns of both read $turns"
diag "$out"
check "the chains are longer than the chain cache keeps (29 pages of the default budget): the first updates read them" test "$first" -ge 60
check "a turn from one table to the other reads the pages its chain gained, not the chain" test "$turns" -le $((4 * TURNS))

D=$(dir fl)
sess_open fl rd
sess_say rd "BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) > 0 FROM p; \\echo snap"
wait_for "grep -q '^snap\$' $D/rd.out" 30 || bail "the snapshot did not open"
read -r p0 p1 p2 < <(sql fl "${stmts[@]}" | paste -sd' ' -)
turns=$((p2 - p1))
out="with a snapshot open, $TURNS turns of both read $turns pages"
diag "$out"
check "with an older snapshot open, the records of the updates that ended join the log: no chain is read again" test "$turns" -le $((4 * TURNS))
sess_say rd "COMMIT;"
sess_close rd
out=$(sql fl "SELECT sum(v) FROM p" "SELECT sum(v) FROM q" | paste -sd' ' -)
check "the rows are exact" test "$out" == "$((6 * 20000 + 2 * (TURNS + 1))) $((6 * 20000 + 2 * (TURNS + 1)))"
check "no backend died" test "$(grep -ac 'terminated by signal' "$(dir fl)/postgres.log")" -eq 0
finish
