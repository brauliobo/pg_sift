#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# A row another transaction staged and has not written, or deleted since a statement read it, is what its writer left:
#   1 the cleanup of a full btree page by another writer keeps the entry of a row that a running update staged: after its commit the index finds the row
#   2 the check of a unique index that missed a staged row on disk and finds its inserter gone looks on disk again: the row the inserter wrote is a duplicate
# 2 stops a backend at an injection point (PostgreSQL built with them only).
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 4
node_init sr 2 "autovacuum = off
sift.async_maintenance = off
sift.test_create_format1 = off"
node_start sr || bail "start failed"
D=$(dir sr)
said() { wait_for "grep -q '^$2\$' $D/$1.out" 30 || bail "the session $1 did not reach $2"; }
waits() { wait_for "[[ \$(sql sr \"SELECT count(*) FROM pg_stat_activity WHERE wait_event = '$1'\") == 1 ]]" 30; }
wake() { sql sr "SELECT injection_points_wakeup('$1')" >/dev/null; }
arm() { sess_say "$1" "SELECT injection_points_set_local(); SELECT injection_points_attach('$2', 'wait'); \\echo armed"; said "$1" armed; }
sql sr "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE p (id int PRIMARY KEY, n int NOT NULL) USING sift_table" \
	"CREATE TABLE c (pid int REFERENCES p)" \
	"INSERT INTO p SELECT g, 0 FROM generate_series(1, 400) g" \
	"CREATE TABLE u (id int PRIMARY KEY, v int NOT NULL) USING sift_table" >/dev/null || bail "setup failed"

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

if pg_config --configure | grep -q -- --enable-injection-points; then
	sql sr "CREATE EXTENSION injection_points" >/dev/null || bail "injection_points failed"
	sess_open sr b

	sess_say a "BEGIN; INSERT INTO u VALUES (1, 1); \\echo inserted"
	said a inserted
	arm b sift-staged-unwritten
	sess_say b "INSERT INTO u VALUES (1, 2); SELECT injection_points_detach('sift-staged-unwritten'); \\echo checked"
	waits sift-staged-unwritten || bail "the unique check did not stop at its probe"
	sess_say a "COMMIT; \\echo committed"
	said a committed
	wake sift-staged-unwritten
	said b checked
	out=$(sql sr "SELECT count(*) FROM u WHERE id = 1")
	check "a unique check whose inserter wrote its row and ended before the probe refuses the duplicate" test "$out" == 1
	check "with a unique violation" grep -q 'duplicate key value violates unique constraint "u_pkey"' "$D/b.out"
	sess_close b
else
	skip 2 "PostgreSQL without injection points (scripts/pg-assert.sh --inject)"
fi
sess_close a
finish
