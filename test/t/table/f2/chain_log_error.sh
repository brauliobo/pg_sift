#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# A modify that errors (a cancel, out of memory) while it reads the column delta chain of a partition leaves no log a later statement of the backend takes for the chain:
#   1 the foreign log of a chain too long for the chain cache (table/f2/col_delta/foreign.c) stopped after its first page (sift-col-delta-foreign-visit): the next update of the backend
#     finds the writers of its row, so a repeatable read update of a row changed since its snapshot fails to serialize instead of losing the other update
#   2 the log of the chain cache (table/f2/chain_cache.c) stopped before its pages are read (sift-col-delta-log-load): the next update of the backend reads the chain again
# Both stop at an injection point (PostgreSQL built with them only: scripts/pg-assert.sh --inject).
. "${SIFT_REPO:?}/test/t/lib/common.bash"

pg_config --configure | grep -q -- --enable-injection-points || { echo "1..0 # SKIP PostgreSQL without injection points (scripts/pg-assert.sh --inject)"; exit 0; }

plan 6
node_init cl 1 "autovacuum = off
sift.async_maintenance = off"
node_start cl || bail "start failed"
D=$(dir cl)
said() { wait_for "grep -q '^$2\$' $D/$1.out" 30 || bail "the session $1 did not reach $2"; }
fail_at() { echo "SELECT injection_points_set_local(); SELECT injection_points_attach('$1', 'error');"; }
sql cl "CREATE EXTENSION pg_sift" "CREATE EXTENSION injection_points" \
	"CREATE TABLE s (k int PRIMARY KEY, v int) USING sift_table" \
	"CREATE TABLE p (k int PRIMARY KEY, v int) USING sift_table" \
	"INSERT INTO s SELECT g, 0 FROM generate_series(1, 20000) g" \
	"INSERT INTO p SELECT g, 0 FROM generate_series(1, 20000) g" \
	"UPDATE s SET v = v + 1" "UPDATE s SET v = v + 1 WHERE k <= 10" >/dev/null || bail "setup failed"

sess_open cl rd
sess_say rd "BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM p; \\echo snap"
said rd snap
sql cl "UPDATE p SET v = v + 1 WHERE k = 1000" "UPDATE p SET v = v + 1 WHERE k = 10" >/dev/null || bail "the updates failed"
for i in 1 2 3 4 5 6; do sql cl "UPDATE p SET v = v + 1 WHERE k > 10000" >/dev/null || bail "the churn failed"; done
sess_say rd "SAVEPOINT s; $(fail_at sift-col-delta-foreign-visit) UPDATE p SET v = v + 100 WHERE k = 20000;" \
	"ROLLBACK TO s; SELECT injection_points_detach('sift-col-delta-foreign-visit'); UPDATE p SET v = v + 100 WHERE k = 10; COMMIT;"
sess_close rd
check "the update stops at the first page of the foreign log" grep -q "error triggered for injection point sift-col-delta-foreign-visit" "$D/rd.out"
check "the next update of the backend finds the writer of its row" grep -q "could not serialize access due to concurrent update" "$D/rd.out"
out=$(sql cl "SELECT v FROM p WHERE k = 10")
check "the other update stays" test "$out" == 1

sess_open cl a
sess_say a "$(fail_at sift-col-delta-log-load) UPDATE s SET v = v + 1 WHERE k = 1; SELECT injection_points_detach('sift-col-delta-log-load');" \
	"UPDATE s SET v = v + 1 WHERE k = 2; UPDATE s SET v = v + 1 WHERE k = 2;"
sess_close a
check "the update stops at the load of the log" grep -q "error triggered for injection point sift-col-delta-log-load" "$D/a.out"
out=$(sql cl "SELECT sum(v) FROM s")
check "the next updates of the backend read the chain again" test "$out" == $((20000 + 10 + 2))
check "no backend died" test "$(grep -ac 'terminated by signal' "$D/postgres.log")" -eq 0
finish
