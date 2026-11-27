#!/usr/bin/env bash
# The passes of VACUUM over the visibility map of a sift_table (table/f2/vm/vm.c): a maintenance_work_mem above 8 GB used to size the counts of a window from the budget alone, past what
# palloc makes ("invalid memory alloc request size"), and failed the VACUUM of every table, even one of one row; the window holds what the table has. The sweep of the index entries of
# rows an insert never wrote lets the writers in once it knows its vblocks: an INSERT does not wait out the cleanup of the indexes (here a btree whose page a cursor pins).
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 6
node_init mm 6 "shared_preload_libraries = 'pg_sift'
autovacuum = off
sift.async_maintenance = off
${SIFT_TESTING:+sift.test_create_format1 = off}"
node_start mm || bail "start failed"
sql mm "CREATE EXTENSION pg_sift" "CREATE TABLE one (id int, k int) USING sift_table" "CREATE INDEX one_k ON one (k)" "INSERT INTO one VALUES (1, 1)" \
	"CREATE TABLE many (id int, k int) USING sift_table" "INSERT INTO many SELECT g, g FROM generate_series(1, 2000) g" >/dev/null || bail "setup failed"

out=$(sql mm "SET maintenance_work_mem = '9GB'" "VACUUM one" 2>&1)
check "VACUUM of a table of one row at a maintenance_work_mem of 9 GB" test -z "$out"
out=$(sql mm "SET maintenance_work_mem = '9GB'" "VACUUM many" "SELECT relallvisible > 0 FROM pg_class WHERE relname = 'many'" 2>&1)
check "and of a table whose part it marks all visible" test "$out" == t

# the rows of a savepoint rolled back leave index entries in two vblocks that also hold committed rows
sql mm "CREATE TABLE t (id int, k int) USING sift_table" "CREATE INDEX t_k ON t (k)" "BEGIN" "INSERT INTO t SELECT g, g FROM generate_series(1, 2000) g" "SAVEPOINT s" \
	"INSERT INTO t SELECT g, g FROM generate_series(2001, 2100) g" "ROLLBACK TO s" "INSERT INTO t VALUES (0, 0)" "COMMIT" "ANALYZE t" >/dev/null || bail "orphans failed"
D=$(dir mm)
sess_open mm pin
sess_say pin "SET enable_seqscan = off; SET enable_bitmapscan = off; BEGIN; DECLARE c CURSOR FOR SELECT k FROM t WHERE k > 1990; FETCH 1 FROM c; \\echo pinned"
wait_for "grep -q '^pinned\$' $D/pin.out" 30 || bail "the cursor did not pin the btree"
sess_open mm vac
sess_say vac "SET application_name = 'vac'; VACUUM t; \\echo vacuumed"
wait_for "[[ \$(sess_count mm vac \"wait_event = 'BufferPin'\") == 1 ]]" 30
check "VACUUM waits for the pin of the btree page" test "$(sess_count mm vac "wait_event = 'BufferPin'")" == 1
out=$(sql mm "SET lock_timeout = '3s'" "INSERT INTO t VALUES (3000, 3000)" 2>&1)  # timeout-ok: the INSERT waits for no lock of VACUUM; one that waits blocks until the cursor ends
check "an INSERT does not wait for the cleanup of the indexes" test -z "$out"
sess_say pin "COMMIT;"
sess_close pin
wait_for "grep -q '^vacuumed\$' $D/vac.out" 30
check "VACUUM ends once the pin is gone" grep -q '^vacuumed$' "$D/vac.out"
sess_close vac
out=$(sql mm "SET enable_seqscan = off" "SET enable_bitmapscan = off" "SELECT count(*), sum(k) FROM t WHERE k >= 0")
check "the index answers the rows of the table" test "$out" == "2002|2004000"

finish
