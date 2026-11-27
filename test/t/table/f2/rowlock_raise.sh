#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The row locks of a sift_table where a lock is raised or its intention fails:
#   1 a transaction whose first xid cannot be assigned (the lock table of the server is full) fails while it records the intention of its row lock, and leaves the slot of the
#     intention readable: a coarse lock that reads every slot afterwards (the PREPARE of a transaction that holds a row) ends
#   2 a COPY into a table with a primary key leaves no mark of the rows it staged once it wrote them: the unique checks of others find them in their parts
#   3-4 a KEY SHARE holder whose UPDATE read the row before another transaction, holding the relation (it locked more rows than a partition keeps entries for), updated it and committed, raises its lock and reads the row again: it
#     updates the new version, so no update is lost and the key stays unique
# 3-4 stop a backend at an injection point (PostgreSQL built with them only).
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 4
node_init rr 2 "autovacuum = off
max_prepared_transactions = 2
sift.async_maintenance = off
sift.test_create_format1 = off"
node_start rr || bail "start failed"
D=$(dir rr)
said() { wait_for "grep -q '^$2\$' $D/$1.out" 30 || bail "the session $1 did not reach $2"; }
waits() { wait_for "[[ \$(sql rr \"SELECT count(*) FROM pg_stat_activity WHERE wait_event = '$1'\") == 1 ]]" 30; }
sql rr "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE f (id int PRIMARY KEY, n int NOT NULL) USING sift_table" \
	"INSERT INTO f SELECT g, 0 FROM generate_series(1, 10) g" \
	"CREATE TABLE c (id int PRIMARY KEY, n int) USING sift_table" \
	"CREATE TABLE r (id int PRIMARY KEY, n int NOT NULL) USING sift_table" \
	"INSERT INTO r SELECT g, 0 FROM generate_series(1, 33000) g" "CREATE INDEX ON r (n)" >/dev/null || bail "setup failed"

sess_open rr a
sess_say a "BEGIN; SELECT 1 FROM f WHERE id = 3 FOR KEY SHARE; COMMIT;
DO \$\$ BEGIN FOR i IN 1..1000000 LOOP BEGIN PERFORM pg_advisory_lock(i); EXCEPTION WHEN out_of_memory THEN EXIT; END; END LOOP; END \$\$;
BEGIN; SELECT 1 FROM f WHERE id = 1 FOR KEY SHARE; ROLLBACK; SELECT pg_advisory_unlock_all();
BEGIN; SELECT 1 FROM f WHERE id = 2 FOR KEY SHARE; PREPARE TRANSACTION 'rr'; COMMIT PREPARED 'rr'; \\echo prepared"
check "a coarse lock reads the intentions after a lock that failed to assign its xid" wait_for "grep -q '^prepared\$' $D/a.out" 15
grep -q '^prepared$' "$D/a.out" || bail "the PREPARE spins on a slot left half written (the backend ignores signals: stop the node immediately)"

marks="SELECT in_table + in_chunks FROM sift._test_rowlock_stats()"
out=$(sql rr "BEGIN" "CREATE TEMP TABLE m AS $marks" "COPY c (id) FROM PROGRAM 'seq 1 30000'" "SELECT ($marks) - (SELECT * FROM m)" "COMMIT")
check "a COPY holds no mark of the rows it wrote" test "$out" -le 0

if pg_config --configure | grep -q -- --enable-injection-points; then
	sql rr "CREATE EXTENSION injection_points" >/dev/null || bail "injection_points failed"
	sess_say a "BEGIN; SELECT id FROM r WHERE id = 1 FOR KEY SHARE; SELECT injection_points_set_local(); SELECT injection_points_attach('sift-lock-row-read', 'wait'); \\echo armed"
	said a armed
	sess_say a "UPDATE r SET n = n + 100 WHERE id = 1; SELECT injection_points_detach('sift-lock-row-read'); \\echo updated"
	waits sift-lock-row-read || bail "the update did not stop before its lock"
	sql rr "BEGIN" "SELECT count(*) FROM (SELECT FROM r FOR NO KEY UPDATE) l" "UPDATE r SET n = n + 1 WHERE id = 1" "COMMIT" >/dev/null || bail "the update under the relation lock failed"
	for _ in $(seq 1 5); do
		grep -q '^updated$' "$D/a.out" && break
		sql rr "SELECT injection_points_wakeup('sift-lock-row-read')" >/dev/null
		wait_for "grep -q '^updated\$' $D/a.out || [[ \$(sql rr \"SELECT count(*) FROM pg_stat_activity WHERE wait_event = 'sift-lock-row-read'\") == 1 ]]" 30 || bail "the update neither stopped nor ended"
	done
	said a updated
	sess_say a "COMMIT; \\echo done"
	said a done
	out=$(sql rr "SELECT count(*) || ' ' || sum(n) FROM r WHERE id = 1")
	check "the raised lock updates the version the other transaction committed: one row, both updates" test "$out" == "1 101"
	out=$(sed -n '/^armed$/,$p' "$D/a.out")
	check "with no error" test "${out//ERROR/}" == "$out"
else
	skip 2 "PostgreSQL without injection points (scripts/pg-assert.sh --inject)"
fi
sess_close a
finish
