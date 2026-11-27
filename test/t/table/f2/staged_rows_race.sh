#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# A row another transaction staged and has not written, or deleted since a statement read it, is what its writer left:
#   1 the cleanup of a full btree page by another writer keeps the entry of a row that a running update staged: after its commit the index finds the row
#   2 the check of a unique index that missed a staged row on disk and finds its inserter gone looks on disk again: the row the inserter wrote is a duplicate
#   3 a lock taken after the row was read reads it again: a foreign key check and an update do not take a row whose delete committed in between
#   4 the load of the pending deletes of a partition that writers keep appending to gives up its cache after a few tries instead of spinning
# 2-4 stop a backend at an injection point (PostgreSQL built with them only).
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 7
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
	"CREATE TABLE u (id int PRIMARY KEY, v int NOT NULL) USING sift_table" \
	"CREATE TABLE q (id int PRIMARY KEY, n int NOT NULL) USING sift_table" \
	"INSERT INTO q SELECT g, 0 FROM generate_series(1, 1000) g" \
	"DELETE FROM q WHERE id = 1" >/dev/null || bail "setup failed"

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

	arm b sift-lock-row-read
	sess_say b "INSERT INTO c VALUES (7); UPDATE p SET n = n + 1 WHERE id = 8; SELECT injection_points_detach('sift-lock-row-read'); \\echo locked"
	waits sift-lock-row-read || bail "the foreign key check did not stop before its lock"
	sql sr "DELETE FROM p WHERE id = 7" >/dev/null || bail "the delete of the parent failed"
	wake sift-lock-row-read
	waits sift-lock-row-read || bail "the update did not stop before its lock"
	sql sr "DELETE FROM p WHERE id = 8" >/dev/null || bail "the delete of the row failed"
	wake sift-lock-row-read
	said b locked
	out=$(sql sr "SELECT count(*) FROM c WHERE NOT EXISTS (SELECT 1 FROM p WHERE p.id = c.pid)")
	check "a foreign key check that locks a parent deleted since it read it leaves no orphan" test "$out" == 0
	out=$(sql sr "SELECT count(*) FROM p WHERE id IN (7, 8)")
	check "an update that locks a row deleted since it read it does not bring it back" test "$out" == 0

	arm b sift-pend-del-loaded
	sess_say b "UPDATE q SET n = n + 1 WHERE id = 500; SELECT injection_points_detach('sift-pend-del-loaded'); \\echo loaded"
	stopped="SELECT count(*) FROM pg_stat_activity WHERE wait_event = 'sift-pend-del-loaded'"
	for i in $(seq 2 40); do
		wait_for "grep -q '^loaded\$' $D/b.out || [[ \$(sql sr \"$stopped\") == 1 ]]" 30 || bail "the reader neither stopped nor ended"
		grep -q '^loaded$' "$D/b.out" && break
		sql sr "DELETE FROM q WHERE id = $i" >/dev/null || bail "the delete $i failed"
		wake sift-pend-del-loaded
	done
	out=$(grep -c '^loaded$' "$D/b.out")
	check "a reader of pending deletes that grow under every load goes on" test "$out" == 1
	[[ $out == 1 ]] || { sql sr "SELECT injection_points_detach('sift-pend-del-loaded')" >/dev/null; wake sift-pend-del-loaded; }
	sess_close b
else
	skip 5 "PostgreSQL without injection points (scripts/pg-assert.sh --inject)"
fi
sess_close a
finish
