#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# A modify that errors (a cancel, out of memory) while it reads the column delta chain of a partition leaves no log a later statement of the backend takes for the chain:
#   the log of the chain cache (table/f2/chain_cache.c) stopped before its pages are read (sift-col-delta-log-load): the next update of the backend reads the chain again
# It stops at an injection point (PostgreSQL built with them only: scripts/pg-assert.sh --inject).
. "${SIFT_REPO:?}/test/t/lib/common.bash"

pg_config --configure | grep -q -- --enable-injection-points || { echo "1..0 # SKIP PostgreSQL without injection points (scripts/pg-assert.sh --inject)"; exit 0; }

plan 3
node_init cl 1 "autovacuum = off
sift.async_maintenance = off"
node_start cl || bail "start failed"
D=$(dir cl)
fail_at() { echo "SELECT injection_points_set_local(); SELECT injection_points_attach('$1', 'error');"; }
sql cl "CREATE EXTENSION pg_sift" "CREATE EXTENSION injection_points" \
	"CREATE TABLE s (k int PRIMARY KEY, v int) USING sift_table" \
	"INSERT INTO s SELECT g, 0 FROM generate_series(1, 20000) g" \
	"UPDATE s SET v = v + 1" "UPDATE s SET v = v + 1 WHERE k <= 10" >/dev/null || bail "setup failed"

sess_open cl a
sess_say a "$(fail_at sift-col-delta-log-load) UPDATE s SET v = v + 1 WHERE k = 1; SELECT injection_points_detach('sift-col-delta-log-load');" \
	"UPDATE s SET v = v + 1 WHERE k = 2; UPDATE s SET v = v + 1 WHERE k = 2;"
sess_close a
check "the update stops at the load of the log" grep -q "error triggered for injection point sift-col-delta-log-load" "$D/a.out"
out=$(sql cl "SELECT sum(v) FROM s")
check "the next updates of the backend read the chain again" test "$out" == $((20000 + 10 + 2))
check "no backend died" test "$(grep -ac 'terminated by signal' "$D/postgres.log")" -eq 0
finish
