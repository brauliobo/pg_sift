#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The vblock leases of a backend (table/f2/f2_vm_lease.c) with the registry full: the lease that a backend gives back to take another is a lease of its own, so it takes no lock of the table it
# belonged to (a lock held to the end of the transaction, or waited for with a concurrent TRUNCATE holding the table), and a writer that has no lease at all holds back every bit of the visibility
# map of every table until it ends (VACUUM sets a bit below the least vblock of the leases, and an insert with no lease has none to put there).
. "${SIFT_REPO:?}/test/t/lib/common.bash"

out=
plan 4
node_init vl 2 "autovacuum = off
shared_preload_libraries = 'pg_sift'
sift.async_maintenance = off
sift.test_create_format1 = off"
node_start vl || bail "start failed"
D=$(dir vl)
sql vl "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE FUNCTION p98_fill(int) RETURNS int AS 'pg_sift', 'sift_test_vm_lease_fill' LANGUAGE C STRICT" \
	"CREATE FUNCTION p98_floor(regclass, int8) RETURNS int8 AS 'pg_sift', 'sift_test_vm_lease_floor' LANGUAGE C STRICT" \
	"CREATE TABLE le1 (k int) USING sift_table" \
	"CREATE TABLE le2 (k int) USING sift_table" \
	"CREATE TABLE le3 (k int) USING sift_table" >/dev/null || bail "setup failed"

out=$(sql vl "INSERT INTO le1 VALUES (1)" "SELECT p98_fill(5000) > 0" "BEGIN" "INSERT INTO le2 VALUES (1)" \
	"SELECT count(*) FROM pg_locks WHERE pid = pg_backend_pid() AND relation = 'le1'::regclass" "COMMIT" | tail -n 1)
check "a lease given back leaves no lock on its table" test "$out" == 0

rm -f "$D/ready" "$D/locked" "$D/release"
{ echo "INSERT INTO le1 VALUES (1);"; echo "SELECT p98_fill(5000) > 0;"; echo "\\! touch '$D/ready'"; echo "\\! while [ ! -e '$D/locked' ]; do sleep 0.05; done"
  echo "SET lock_timeout = '5s';"; echo "INSERT INTO le2 VALUES (1);"; echo "SELECT 'inserted';"; } | sql vl >"$D/evict.out" 2>&1 &  # timeout-ok: the product waits for no lock, the timeout is the failure
evictor=$!
wait_for "[[ -e $D/ready ]]" 60 || bail "the writer did not fill the registry"
{ echo "BEGIN;"; echo "LOCK TABLE le1 IN ACCESS EXCLUSIVE MODE;"; echo "\\! touch '$D/locked'"; echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"; } | sql vl >/dev/null &
locker=$!
wait "$evictor"
touch "$D/release"
wait "$locker"
out=$(tail -n 1 "$D/evict.out")
check "a lease given back does not wait for a lock another session holds on its table" test "$out" == inserted

rm -f "$D/ready" "$D/release"
{ echo "SELECT p98_fill(5000) > 0;"; echo "INSERT INTO le3 SELECT g FROM generate_series(1, 10) g;"; echo "\\! touch '$D/ready'"; echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"; } | sql vl >/dev/null &
writer=$!
wait_for "[[ -e $D/ready ]]" 60 || bail "the writer did not insert"
out=$(sql vl "SELECT p98_floor('le3', 1000)")
check "a writer with no lease slot holds back every bit of the map while it runs" test "$out" == 0
touch "$D/release"
wait "$writer"
out=$(sql vl "SELECT p98_floor('le3', 1000)")
check "and lets go of them when it ends" test "$out" == 1000
finish
