#!/usr/bin/env bash
# The logs of 4.0.0 to 4.1.1 on a primary and its physical standby: the library of 4.1.0 writes row versions that a statement whose command id moved left without a tombstone (a function that
# runs SQL inside an UPDATE), the heap-free read counts them on both nodes; both nodes then load this tree. Until the log is re-baselined the reads of both nodes check the heap and answer as
# the heap (the standby cannot re-baseline, and says so by reading the heap); the next VACUUM of the primary re-baselines it, the standby gets the result by replication and its heap-free
# reads answer as the heap again; the standby that loads the library of 4.1.0 again reads the marked log (flags 3, which that library takes for incomplete) as the heap.
# The release is built from its tag by scripts/old-lib-check.sh (PostgreSQL 18).
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
if ((PG_MAJOR != 18)); then echo '1..0 # SKIP the released libraries build on PostgreSQL 18 only (two library roots)'; exit 0; fi
newroot=$ROOT
build_old v4.1.0; oldroot=$OLDROOT
plan 12

ROWS=$(knob SIFT_RB_ROWS 20000 8000)
Q="SELECT string_agg(tenant || ':' || n || ':' || s, ' ' ORDER BY tenant) FROM (SELECT tenant, count(*) n, sum(amount) s FROM t GROUP BY tenant) q"
# ans NODE MODE: the grouped answer by the heap-free scan (free), by the heap (heap); the older library has no sift.test_no_heapfree
ans() { sql "$1" "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = $([[ $2 == free ]] && echo on || echo off)" "$Q" | tail -1; }
right() { [[ $(ans "$1" free) == "$(ans "$1" heap)" ]]; }
hf() { sql "$1" "SELECT heap_free FROM sift.col_tracking('t_c')"; }

node_init pm 36 "wal_level = replica
max_wal_senders = 4
autovacuum = off
sift.async_maintenance = off"
use_root pm "$oldroot"
node_start pm || bail "primary start failed"
sql pm "CREATE EXTENSION pg_sift" "CREATE TABLE t (code text PRIMARY KEY, tenant int NOT NULL, amount int NOT NULL) WITH (autovacuum_enabled = false, fillfactor = 100)" \
	"INSERT INTO t SELECT 'c' || i, i % 10, i FROM generate_series(1, $ROWS) i" "CREATE INDEX t_c ON t USING sift_col (code, tenant, amount)" "VACUUM (FREEZE, ANALYZE) t" \
	"CREATE FUNCTION one() RETURNS int LANGUAGE plpgsql AS \$\$ BEGIN PERFORM 1; RETURN 0; END \$\$" >/dev/null || bail "setup failed"
# a new session whose UPDATEs call a function that runs SQL: the command id moves under the statement, and the versions it ends keep their entries
sql pm "BEGIN" "UPDATE t SET amount = amount + 1 + one() WHERE code IN (SELECT 'c' || i FROM generate_series(7, $ROWS, 7) i)" \
	"UPDATE t SET amount = amount + 5 + one() WHERE code IN (SELECT 'c' || i FROM generate_series(5, $ROWS, 5) i)" "COMMIT" >/dev/null || bail "the updates failed"
check "the older library counts the old versions on the primary" test "$(ans pm free)" != "$(ans pm heap)"
node_clone pm ps 37 "hot_standby_feedback = on"
use_root ps "$oldroot"
node_start ps || bail "standby start failed"
catchup pm ps || bail "standby catchup failed"
check "... and on its standby" test "$(ans ps free)" != "$(ans ps heap)"
node_stop ps
node_stop pm

use_root pm "$newroot"
use_root ps "$newroot"
node_start pm || bail "primary start on this tree failed"
node_start ps || bail "standby start on this tree failed"
sql pm "CREATE EXTENSION pg_sift_test" >/dev/null || bail "pg_sift_test failed"
catchup pm ps || bail "standby catchup failed"
check "this tree: the log of the older library has the flags 1" test "$(sql pm "SELECT cur_flags FROM sift._test_log_chan('t_c')")" == 1
check "the primary checks the heap for it" test "$(hf pm)" == f
check "... and answers as the heap" right pm
check "the standby checks the heap for it" test "$(hf ps)" == f
check "... and answers as the heap" right ps
# the VACUUM of the primary (autovacuum and the worker are off in this cluster) re-baselines the log
sql pm "VACUUM t" >/dev/null
check "the next VACUUM of the primary re-baselines it (flags 3), heap-free reads answer as the heap" test "$(sql pm "SELECT cur_flags FROM sift._test_log_chan('t_c')")" == 3 -a "$(hf pm)" == t
check "... as the heap" right pm
catchup pm ps || bail "standby catchup failed"
check "the standby has the result by replication, and its heap-free reads are the heap's" test "$(hf ps)" == t
check "... as the heap" right ps
node_stop ps
use_root ps "$oldroot"
node_start ps || bail "standby start on the older library failed"
catchup pm ps || bail "standby catchup failed"
check "the older library reads the log with the flags 3 as the heap" right ps
# the leak check of finish asks a running node for the test functions of this tree, which the older library has not
node_stop ps
finish
