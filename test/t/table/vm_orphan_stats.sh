#!/usr/bin/env bash
# The sweep of the index entries of rows an insert never wrote (table/f2/vm/vm.c, sift_tbl_vm_sweep_orphans) is the first bulk delete of an index whose VACUUM found no dead row: the
# result of the index is made by its bulk delete and outlives the sweep, so the cleanup of the index that follows reads a live result, not memory the sweep has freed (a BM25 index
# reads its counts there; a sanitizer build reports a use after free).
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 3
node_init vo 1 "shared_preload_libraries = 'pg_sift'
autovacuum = off
sift.async_maintenance = off
${SIFT_TESTING:+sift.test_create_format1 = off}"
node_start vo || bail "start failed"
sql vo "CREATE EXTENSION pg_sift" "CREATE TABLE t (id int, k int, body text) USING sift_table" "CREATE INDEX t_k ON t (k)" "CREATE INDEX t_body ON t USING sift (body)" \
	"BEGIN" "INSERT INTO t SELECT g, g, 'word w' || g % 7 FROM generate_series(1, 2000) g" "SAVEPOINT s" \
	"INSERT INTO t SELECT g, g, 'word w' || g % 7 FROM generate_series(2001, 2100) g" "ROLLBACK TO s" "INSERT INTO t VALUES (0, 0, 'word w0')" "COMMIT" >/dev/null || bail "setup failed"

out=$(sql vo "VACUUM t" 2>&1)
check "VACUUM of a table whose rolled back rows left index entries" test -z "$out"
out=$(sql vo "SET enable_seqscan = off" "SET enable_bitmapscan = off" "SELECT count(*), sum(k) FROM t WHERE k >= 0" 2>&1)
check "the btree answers the rows of the table" test "$out" == "2001|2001000"
out=$(sql vo "SELECT count(*) FROM t WHERE body @@> 'w3'" 2>&1)
check "the BM25 index answers the rows of the table" test "$out" == 286

finish
