#!/usr/bin/env bash
# The dictionaries that backends read in place from the shared ring (col/dictshm.c): a record that a scan reads is not overwritten while the scan lives, however
# many dictionaries the ring takes in meanwhile (a ring of 8 MB, 12 tables of about 2 MB of dictionary: it wraps twice); a backend gives the records back when its
# transaction ends, when a statement fails and when the backend is terminated holding them.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
REPO=$(cd "$(dirname "$0")/../.." && pwd)
. "$(dirname "$0")/lib.bash"

plan 8
node_init primary 2 "sift.test_col_dict_shared = 4"
node_start primary || bail "primary start failed"
TABLES=12
{
	echo "CREATE EXTENSION pg_sift; CREATE EXTENSION pg_sift_test;"
	for t in $(seq $TABLES); do
		echo "CREATE TABLE t$t AS SELECT g AS id, 'k$t' || md5(g::text) AS s FROM generate_series(1, 40000) g;"
		echo "CREATE INDEX t${t}_col ON t$t USING sift_col (id, s) WITH (rollup_dims = 'off');"
		echo "VACUUM (FREEZE, ANALYZE) t$t;"
	done
} | sql primary >/dev/null || bail "setup failed"
node_stop primary
node_start primary || bail "restart failed"

Q1="SELECT s, count(*) FROM t1 WHERE s LIKE '%ab%' GROUP BY s"
WANT=$(sql primary "SET sift.enable_agg = off" "SET sift.enable_col_scan = off" "$Q1" | sort | "$REPO/scripts/sha256.sh")
pinned() { echo "SELECT 'P:' || records FROM sift._test_col_dict_pinned()"; }
churn() { for t in $(seq 2 $TABLES); do echo "SELECT 'C:' || count(*) FROM (SELECT s FROM t$t WHERE s LIKE '%ab%' GROUP BY s) x;"; done; }

# the cursor reads t1's dictionary in place; the ring takes in 11 others and wraps; the cursor still returns the rows of the stock plan
OUT=$(sql primary 2>&1 <<SQL
LOAD 'pg_sift';
SET max_parallel_workers_per_gather = 0; SET sift.agg_force = on;
BEGIN;
DECLARE c CURSOR FOR $Q1;
FETCH 1 FROM c;
$(pinned);
$(churn)
$(pinned);
SELECT 'M:' || (value >= $TABLES) FROM sift.stats() WHERE name = 'dict_shared_misses';
FETCH ALL FROM c;
COMMIT;
$(pinned);
SQL
)
PINS=($(grep '^P:' <<<"$OUT" | cut -c3-))
check "the cursor holds the record it reads" test "${PINS[0]}" -ge 1
check "and the ring took in the other dictionaries" grep -q '^M:true' <<<"$OUT"
check "the record is still held when the ring wrapped" test "${PINS[1]}" -ge 1
check "the rows after the wrap are those of the stock plan" test "$(grep -v '^[PMC]:' <<<"$OUT" | sort | "$REPO/scripts/sha256.sh")" == "$WANT"
check "a transaction that ended gave the record back" test "${PINS[2]}" == 0

# a statement that fails while a cursor holds the record
sql primary >/dev/null 2>&1 <<SQL
LOAD 'pg_sift'; SET max_parallel_workers_per_gather = 0; SET sift.agg_force = on; BEGIN; DECLARE c CURSOR FOR $Q1; FETCH 1 FROM c; SELECT 1 / (count(*) - count(*)) FROM t2;
SQL
check "a failed transaction gave the records back" test "$(sql primary "$(pinned)")" == P:0

# a backend that is terminated holding records
sql primary >/dev/null 2>&1 <<SQL &
LOAD 'pg_sift'; SET max_parallel_workers_per_gather = 0; SET sift.agg_force = on; BEGIN; DECLARE c CURSOR FOR $Q1; FETCH 1 FROM c; SELECT pg_sleep(600);
SQL
victim() { sql primary "SELECT pid FROM pg_stat_activity WHERE query LIKE 'SELECT pg_sleep(600)%' AND pid <> pg_backend_pid()"; }
for _ in $(seq 300); do [[ -n $(victim) ]] && break; sleep 0.1; done
HELD=$(sql primary "$(pinned)"); HELD=${HELD#P:}
sql primary "SELECT pg_terminate_backend($(victim))" >/dev/null
wait
for _ in $(seq 100); do [[ $(sql primary "$(pinned)") == P:0 ]] && break; sleep 0.1; done
check "a backend that was terminated while it held records had them" test "$HELD" -ge 1
check "and gave them back when it left" test "$(sql primary "$(pinned)")" == P:0
finish
