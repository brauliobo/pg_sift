#!/usr/bin/env bash
# Two contracts of the table access method that PostgreSQL states in its assertions, which a server built with them (--enable-cassert) turns into an abort of the backend:
# - a TID range scan is given the ends of the range as the executor makes them: an exclusive end is stepped to the address next to it, so an end may be (block, 0) or (block, 65535), and no row
#   is there. The rows it returns are those of the range, each with its address;
# - the btree asks the table which index entries it may delete (index_delete_tuples): an entry its caller marked deletable stays so, and the array it gets back ends at the last deletable entry,
#   so that a bottom-up deletion that finds nothing gets an empty array and the btree has nothing to write. An UPDATE of a table with a primary key that changes no key leaves duplicates of
#   each key on the index pages, which is what the bottom-up deletion is asked about.
# - a wait for the transaction that holds a row (an UPDATE or a SELECT FOR UPDATE that is blocked) names the row it waits on, as XactLockTableWait asks.
# The checks are the same on a server without assertions; the abort is the failure on one with them.
. "$(dirname "$0")/lib.bash"

plan 13
node_init ac 70 "autovacuum = off
sift.async_maintenance = off
sift.test_create_format1 = off
max_parallel_workers_per_gather = 0"
node_start ac || bail "start failed"
sql ac "CREATE EXTENSION pg_sift" "CREATE TABLE t (k int PRIMARY KEY, v int) USING sift_table" \
	"INSERT INTO t SELECT g, 0 FROM generate_series(1, 6000) g" >/dev/null || bail "setup failed"

oracle() { sql ac "SELECT count(*) FROM (SELECT ctid AS c FROM t OFFSET 0) s WHERE $1"; }
ranged() { sql ac "SELECT count(*) FROM t WHERE $1"; }
agree() {
	local qual=$1 want got
	want=$(oracle "$qual")
	got=$(ranged "${qual//c /ctid }")
	[[ -n $want && $want == "$got" ]] || { out="range [$qual]: the scan found $got, the filter of all rows $want"; return 1; }
}
out=$(sql ac "EXPLAIN SELECT 1 FROM t WHERE ctid >= '(0,1)' AND ctid < '(1,1)'" | grep -c 'Tid Range Scan')
check "the planner scans a range of addresses" test "$out" -eq 1
check "an exclusive end (block, 0)" agree "c >= '(0,1)' AND c < '(1,1)'"
check "an exclusive start (block, 65535)" agree "c > '(0,65535)'"
check "one end only: an upper end" agree "c < '(0,100)'"
check "one end only: a lower end" agree "c >= '(0,100)'"
check "an inclusive range of one block" agree "c >= '(0,1)' AND c <= '(0,100)'"
check "a range with a start after its end" agree "c > '(0,100)' AND c < '(0,50)'"
out=$(sql ac "SELECT count(*) > 0 AND count(DISTINCT ctid) = count(*) AND bool_and(ctid >= '(0,1)' AND ctid < '(1,1)') FROM t WHERE ctid >= '(0,1)' AND ctid < '(1,1)'")
check "the rows of a range are distinct, each at an address of the range" test "$out" == "t"

for _ in 1 2 3; do sql ac "UPDATE t SET v = v + 1 WHERE k <= 2100" >/dev/null; done
out=$(sql ac "SELECT count(*) || ' ' || sum(v) FROM t")
check "UPDATEs of 2,100 rows of a table with a primary key leave the rows exact" test "$out" == "6000 6300"
out=$(sql ac "SELECT count(*) FROM t WHERE k = 1")
check "and one row of a key" test "$out" -eq 1
D=$(dir ac)
holder_of() {   # K: another session holds the row of K until $D/release exists
	rm -f "$D/held" "$D/release"
	{ echo "BEGIN;"; echo "SELECT 1 FROM t WHERE k = $1 FOR UPDATE;"; echo "\\! touch '$D/held'"; echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"; echo "COMMIT;"; } | sql ac >/dev/null &
	holder=$!
	wait_for "[[ -e $D/held ]]" 60 || bail "the holder did not lock its row"
}
waiting() { [[ $(sql ac "SELECT count(*) FROM pg_locks WHERE locktype = 'transactionid' AND NOT granted") -ge 1 ]]; }
waits_for_holder() {   # K STATEMENT: the statement blocks on the row of K, then goes on when its holder commits; the output is in $D/waiter.out
	holder_of "$1"
	sql ac "$2" >"$D/waiter.out" 2>&1 &
	waiter=$!
	wait_for waiting 60 || { touch "$D/release"; wait "$holder" "$waiter"; return 1; }
	touch "$D/release"
	wait "$holder"
	wait "$waiter"
}
waits_for_holder 7 "UPDATE t SET v = 100 WHERE k = 7"
out=$(sql ac "SELECT v FROM t WHERE k = 7")
check "an UPDATE waits for the transaction that holds its row, then changes it" test "$out" -eq 100
waits_for_holder 8 "SELECT v FROM t WHERE k = 8 FOR UPDATE"
check "a SELECT FOR UPDATE waits for the transaction that holds its row, then gets it" test "$(cat "$D/waiter.out")" -eq 3
check "no backend aborted" test "$(grep -ac 'TRAP:\|terminated by signal' "$(dir ac)/postgres.log")" -eq 0
finish
