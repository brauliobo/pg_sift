#!/usr/bin/env bash
# Two sessions that insert one PRIMARY KEY value of a sift_table on a server that preloads pg_sift (the row lock table lives in shared memory): the second waits for the first, fails with a duplicate
# key when the first commits and goes on when the first rolls back, as on a heap.
. "$(dirname "$0")/lib.bash"

plan 5
node_init pk 1 "shared_preload_libraries = 'pg_sift'"
node_start pk || bail "start failed"
D=$(dir pk)
sql pk "CREATE EXTENSION pg_sift" "CREATE TABLE t (id int PRIMARY KEY, v int) USING sift_table" "INSERT INTO t VALUES (1, 1)" >/dev/null || bail "setup failed"

check "pg_sift is preloaded" test "$(sql pk "SHOW shared_preload_libraries")" == pg_sift

race pk "INSERT INTO t VALUES (500, 1)" "INSERT INTO t VALUES (500, 2)"
check "the second insert of a key waits for the first and fails with a duplicate key when the first commits" test "$RACE_WAITED" == 1 \
	-a "$(grep -c 'duplicate key' "$D/rb.out")" == 1 -a "$(sql pk "SELECT v FROM t WHERE id = 500")" == 1

race pk "INSERT INTO t VALUES (600, 1)" "INSERT INTO t VALUES (600, 2)" ROLLBACK
check "the second insert of a key goes on when the first rolls back" test "$RACE_WAITED" == 1 \
	-a "$(grep -c 'duplicate key' "$D/rb.out")" == 0 -a "$(sql pk "SELECT v FROM t WHERE id = 600")" == 2

race pk "UPDATE t SET id = 700 WHERE id = 1" "INSERT INTO t VALUES (700, 2)"
check "an UPDATE of the key is an insert of the new key: the second insert waits and fails" test "$RACE_WAITED" == 1 \
	-a "$(grep -c 'duplicate key' "$D/rb.out")" == 1 -a "$(sql pk "SELECT v FROM t WHERE id = 700")" == 1

check "the table verifies" test "$(sql pk "SELECT count(*) FROM sift.check('t', true)")" == 0
finish
