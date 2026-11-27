#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The validation of CREATE UNIQUE INDEX CONCURRENTLY inserts the rows its build did not see. One of them that another transaction deleted since the snapshot of the
# validation, and replaced by a row of the same key that the index has, is no duplicate: the index ends valid. The build and the validation stop at injection points
# (PostgreSQL built with them only).
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 3
node_init cv 2 "autovacuum = off
sift.async_maintenance = off
sift.test_create_format1 = off
max_parallel_maintenance_workers = 0"
node_start cv || bail "start failed"
D=$(dir cv)
said() { wait_for "grep -q '^$2\$' $D/$1.out" 30 || bail "the session $1 did not reach $2"; }
waits() { wait_for "[[ \$(sql cv \"SELECT count(*) FROM pg_stat_activity WHERE wait_event = '$1'\") == 1 ]]" 30; }
wake() { sql cv "SELECT injection_points_wakeup('$1')" >/dev/null; }
sql cv "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE u (id int NOT NULL, v int NOT NULL) USING sift_table" \
	"INSERT INTO u SELECT g, 0 FROM generate_series(1, 100) g" >/dev/null || bail "setup failed"

if pg_config --configure | grep -q -- --enable-injection-points; then
	sql cv "CREATE EXTENSION injection_points" >/dev/null || bail "injection_points failed"
	sess_open cv a
	sess_say a "SELECT injection_points_set_local(); SELECT injection_points_attach('sift-tbl-index-build-snapshot', 'wait');" \
		"SELECT injection_points_attach('sift-tbl-validate-insert', 'wait'); CREATE UNIQUE INDEX CONCURRENTLY u_id ON u (id); \\echo built"
	waits sift-tbl-index-build-snapshot || bail "the build did not stop after its snapshot"
	sql cv "INSERT INTO u VALUES (1000, 1)" >/dev/null || bail "the insert the build misses failed"
	wake sift-tbl-index-build-snapshot
	waits sift-tbl-validate-insert || bail "the validation did not stop before it inserts the missed row"
	sql cv "DELETE FROM u WHERE id = 1000" "INSERT INTO u VALUES (1000, 2)" >/dev/null || bail "the replacement of the row failed"
	wake sift-tbl-validate-insert
	said a built
	check "the validation raises no unique violation for a row deleted since its snapshot" bash -c "! grep -q 'duplicate key' $D/a.out"
	out=$(sql cv "SELECT indisvalid FROM pg_index WHERE indexrelid = 'u_id'::regclass")
	check "the index is valid" test "$out" == t
	out=$(sql cv "SET enable_seqscan = off" "SELECT v FROM u WHERE id = 1000")
	check "the index finds the row that replaced it" test "$out" == 2
	sess_close a
else
	skip 3 "PostgreSQL without injection points (scripts/pg-assert.sh --inject)"
fi
finish
