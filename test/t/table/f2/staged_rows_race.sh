#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# A row another transaction staged and has not written, or deleted since a statement read it, is what its writer left:
#   1 the cleanup of a full btree page by another writer keeps the entry of a row that a running update staged: after its commit the index finds the row
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 2
node_init sr 2 "autovacuum = off
sift.async_maintenance = off
sift.test_create_format1 = off"
node_start sr || bail "start failed"
D=$(dir sr)
said() { wait_for "grep -q '^$2\$' $D/$1.out" 30 || bail "the session $1 did not reach $2"; }
sql sr "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE p (id int PRIMARY KEY, n int NOT NULL) USING sift_table" \
	"CREATE TABLE c (pid int REFERENCES p)" \
	"INSERT INTO p SELECT g, 0 FROM generate_series(1, 400) g" >/dev/null || bail "setup failed"

sess_open sr a
sess_say a "BEGIN; UPDATE p SET n = -1 WHERE id = 200; \\echo staged"
said a staged
sql sr "DO \$\$ BEGIN FOR i IN 1..60 LOOP UPDATE p SET n = i WHERE id BETWEEN 190 AND 210 AND id <> 200; COMMIT; END LOOP; END \$\$" >/dev/null || bail "the churn failed"
sess_say a "COMMIT; \\echo done"
said a done
out=$(sql sr "SET enable_seqscan = off" "SET enable_bitmapscan = off" "SELECT count(*) FROM p WHERE id = 200 AND n = -1")
check "the index finds the row a running update staged while another writer cleaned its page" test "$out" == 1
out=$(sql sr "SET enable_indexscan = off" "SET enable_bitmapscan = off" "SELECT count(*) FROM p WHERE id = 200")
check "the table holds the row once" test "$out" == 1
sess_close a
finish
