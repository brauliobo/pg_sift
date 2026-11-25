#!/usr/bin/env bash
# Reading the state of an index (sift.space, sift.status, sift.health, sift.indexes) takes no lock above AccessShareLock, not even for a moment: a scrape every few seconds is not a lock that
# VACUUM, sift.maintain() or a DDL statement can wait for. The source of sift.space acquires no relation, page or tuple lock of its own (maintenance_busy is read from the lock manager), and the
# scrapes finish under a lock_timeout while a session holds ShareUpdateExclusiveLock of the table.
. "$(dirname "$0")/lib.bash"

out=
plan 6
SRC=$(dirname "$0")/../../src/core/sql/space.c
taken() { grep -E '\b(Conditional)?Lock(Page|Tuple|RelationOid|Relation)\(' "$SRC" | grep -v 'AccessShareLock'; }
check "sql_space.c takes no heavyweight lock above AccessShareLock" test -z "$(taken)"

node_init ml 85 "autovacuum = off
shared_preload_libraries = 'pg_sift'
sift.async_maintenance = off"
node_start ml || bail "start failed"
D=$(dir ml)
sql ml "CREATE EXTENSION pg_sift" "CREATE TABLE c (id int, grp int, amount numeric(10,2))" \
	"CREATE INDEX c_idx ON c USING sift_col (id, grp, amount)" "CREATE TABLE t (id int, grp int) USING sift_table" \
	"INSERT INTO c SELECT g, g % 7, g FROM generate_series(1, 5000) g" "INSERT INTO t SELECT g, g % 7 FROM generate_series(1, 5000) g" >/dev/null || bail "setup failed"
rm -f "$D/locked" "$D/release"
{ echo "BEGIN;"; echo "LOCK TABLE c IN SHARE UPDATE EXCLUSIVE MODE;"; echo "LOCK TABLE t IN SHARE UPDATE EXCLUSIVE MODE;"; echo "\\! touch '$D/locked'"
	echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"; echo "COMMIT;"; } | sql ml >/dev/null &
holder=$!
wait_for "[[ -e $D/locked ]]" 60 || bail "the holder did not take its locks"
scrape() { sql ml "SET lock_timeout = '500ms'" "$@" 2>&1 | paste -sd' ' -; }  # timeout-ok: the scrapes take no lock that could wait; the timeout is what the test asserts
out=$(scrape "SELECT count(*) FROM sift.space('c_idx')")
check "sift.space of an index under a ShareUpdateExclusiveLock of its table" test "$out" = 1
out=$(scrape "SELECT count(*) FROM sift.space('t')")
check "sift.space of a sift_table" test "$out" = 1
out=$(scrape "SELECT count(*) > 0 FROM sift.status()")
check "sift.status" test "$out" = t
out=$(scrape "SELECT count(*) >= 0 FROM sift.health()" "SELECT count(*) > 0 FROM sift.indexes")
check "sift.health and sift.indexes" test "$out" = "t t"
touch "$D/release"
wait "$holder"
check "the holder ended" test $? -eq 0
finish
