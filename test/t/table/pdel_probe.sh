#!/usr/bin/env bash
# The uniqueness probe of a sift_table asks whether the version it meets was deleted by a pending delete: the deletes the transaction holds in memory and the ones written to the chain of the
# partition. It asks the row by its address of the log of the partition, so a bulk UPDATE of a table with a primary key reads the chain a few times (when the transaction appends to it) and not once
# for each row: sift._test_pend_del_reads counts the reads, and 4 UPDATEs of 10,000 rows each in one transaction, which probe 40,000 times, read the chain fewer than 300 times; so does one UPDATE
# of 60,000 rows, whose deletes are written twice while it runs and whose probes and row locks name the chain by two heads (the log of each head is kept). The answers are the same as ever: the
# key of a row the transaction deleted (held in memory, or written) is free again for the transaction, a key that is alive is a duplicate, and the rows are exact.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 11
node_init pp 60 "autovacuum = off
sift.async_maintenance = off
sift.test_create_format1 = off"
node_start pp || bail "start failed"
sql pp "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE u (k int PRIMARY KEY, v int) USING sift_table" \
	"CREATE TABLE t (k int PRIMARY KEY, v int) USING sift_table" \
	"CREATE TABLE w (k int PRIMARY KEY, v int) USING sift_table" \
	"CREATE TABLE z (k int PRIMARY KEY, v int) USING sift_table" \
	"INSERT INTO u SELECT g, 0 FROM generate_series(1, 20000) g" \
	"INSERT INTO t SELECT g, 0 FROM generate_series(1, 6000) g" \
	"INSERT INTO w SELECT g, 0 FROM generate_series(1, 6000) g" \
	"INSERT INTO z SELECT g, 0 FROM generate_series(1, 80000) g" >/dev/null || bail "setup failed"

bump() { echo "UPDATE u SET v = v + 1 WHERE k <= 10000"; }
out=$(sql pp "BEGIN" "CREATE TEMP TABLE r AS SELECT sift._test_pend_del_reads() AS n" "$(bump)" "$(bump)" "$(bump)" "$(bump)" \
	"SELECT sift._test_pend_del_reads() - n < 300 FROM r" "COMMIT")
check "4 UPDATEs of 10,000 rows read the chain of pending deletes fewer than 300 times" test "$out" == "t"
out=$(sql pp "SELECT count(*) || ' ' || sum(v) FROM u")
check "the rows after the UPDATEs are exact" test "$out" == "20000 40000"
out=$(sql pp "BEGIN" "CREATE TEMP TABLE r AS SELECT sift._test_pend_del_reads() AS n" "UPDATE z SET v = v + 1 WHERE k <= 60000" "SELECT sift._test_pend_del_reads() - n < 300 FROM r" "COMMIT")
check "an UPDATE of 60,000 rows reads the chain of pending deletes fewer than 300 times" test "$out" == "t"
out=$(sql pp "SELECT count(*) || ' ' || sum(v) FROM z")
check "and changes each of them once" test "$out" == "80000 60000"

out=$(sql pp "BEGIN" "DELETE FROM t WHERE k = 1" "INSERT INTO t VALUES (1, 7)" "SELECT v FROM t WHERE k = 1" "COMMIT")
check "a key deleted by the transaction and held in memory is free for it" test "$out" == "7"
out=$(sql pp "BEGIN" "DELETE FROM w WHERE k <= 4000" "INSERT INTO w SELECT g, 5 FROM generate_series(1, 4000) g" "SELECT count(*) || ' ' || sum(v) FROM w" "COMMIT")
check "4,000 keys deleted by the transaction and written are free for it" test "$out" == "6000 20000"
out=$(sql pp "INSERT INTO t VALUES (5000, 1)" 2>&1)
check "a key that is alive is a duplicate" bash -c "[[ \$1 == *'duplicate key value violates unique constraint'* ]]" _ "$out"
out=$(sql pp "BEGIN" "DELETE FROM t WHERE k = 5000" "SAVEPOINT s" "INSERT INTO t VALUES (5000, 2)" "ROLLBACK TO s" "INSERT INTO t VALUES (5000, 3)" "SELECT v FROM t WHERE k = 5000" "COMMIT" 2>&1)
check "a key deleted by the transaction is free again after a savepoint rolled back" test "$out" == "3"
out=$(sql pp "BEGIN" "SAVEPOINT s" "DELETE FROM t WHERE k = 10" "ROLLBACK TO s" "INSERT INTO t VALUES (10, 1)" "COMMIT" 2>&1)
check "a delete a savepoint rolled back leaves the key a duplicate" bash -c "[[ \$1 == *'duplicate key value violates unique constraint'* ]]" _ "$out"
out=$(sql pp "SELECT (SELECT count(*) FROM t) || ' ' || (SELECT count(*) FROM w)")
check "the tables hold the rows that committed" test "$out" == "6000 6000"
check "no backend died" test "$(grep -ac 'terminated by signal' "$(dir pp)/postgres.log")" -eq 0
finish
