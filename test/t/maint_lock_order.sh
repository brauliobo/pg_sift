#!/usr/bin/env bash
# DROP TABLE beside a maintenance worker (the race between the lock order of the statements, table first and then its indexes, and a worker that opens an index
# for the table): an injection point (src/bm25/maint.c, PostgreSQL built with --enable-injection-points: scripts/pg-assert.sh --inject) stops the worker without a sleep,
# (a) after it locked the table: DROP TABLE waits for the worker, the worker goes on and ends, DROP TABLE ends: no deadlock, no error; (b) before it locked the
# table: DROP TABLE ends first, the worker finds the index gone and ends without an error. Both beside the assertions of the lock order (Assert in sift_try_open_index).
. "$(dirname "$0")/lib.bash"

pg_config --configure | grep -q -- --enable-injection-points || { echo "1..0 # SKIP PostgreSQL without injection points (scripts/pg-assert.sh --inject)"; exit 0; }
# 17 names the wait of a point differently
((PG_MAJOR >= 18)) || { echo "1..0 # SKIP the wait of an injection point is named from PostgreSQL 18"; exit 0; }

plan 7
node_init lo 26 "autovacuum = off
sift.maintenance_naptime = 1000
sift.col_auto_merge = on"
node_start lo || bail "start failed"
D=$(dir lo)
sql lo "CREATE EXTENSION pg_sift" "CREATE EXTENSION injection_points" >/dev/null || bail "extensions failed"

setup() {
	sql lo "CREATE TABLE facts (id int, key int, kind text, amount numeric(10,2), pad text)" \
		"INSERT INTO facts SELECT g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 100) FROM generate_series(1, 100000) g" \
		"CREATE INDEX facts_idx ON facts USING sift_col (id, key, kind, amount)" "VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) facts" >/dev/null
}
# reads of the churned table make the worker VACUUM it, and the VACUUM asks for the rewrite worker
waiting() { sql lo "SELECT count(*) FROM pg_stat_activity WHERE wait_event_type = 'InjectionPoint' AND wait_event = '$1'"; }
await_worker() {
	local n
	for n in $(seq 600); do
		sql lo "SET max_parallel_workers_per_gather = 0" "SELECT kind, count(*), sum(amount) FROM facts GROUP BY kind" >/dev/null 2>&1
		[[ $(waiting "$1") -gt 0 ]] && return 0
		sleep 0.2
	done
	return 1
}
errors() { grep -cE 'deadlock detected|ERROR|TRAP|cache lookup failed|could not open relation' "$D/postgres.log"; }
release() { sql lo "SELECT injection_points_detach('$1')" "SELECT injection_points_wakeup('$1')" >/dev/null; }
idle() { for _ in $(seq 300); do [[ $(sql lo "SELECT count(*) FROM pg_stat_activity WHERE wait_event_type = 'InjectionPoint'") == 0 ]] && return 0; sleep 0.1; done; return 1; }

# (a)
setup || bail "setup failed"
sql lo "SELECT injection_points_attach('sift-maint-after-table-lock', 'wait')" >/dev/null
sql lo "UPDATE facts SET amount = amount + 1 WHERE id % 20 = 1" >/dev/null
check "a worker stops after it locked the table" await_worker sift-maint-after-table-lock
sql lo "DROP TABLE facts" >"$D/drop.out" 2>&1 & dp=$!
for _ in $(seq 100); do [[ $(sql lo "SELECT count(*) FROM pg_stat_activity WHERE query = 'DROP TABLE facts' AND wait_event_type = 'Lock'") == 1 ]] && break; sleep 0.1; done
check "the DROP TABLE is in the lock queue of the worker's table lock" test "$(sql lo "SELECT count(*) FROM pg_stat_activity WHERE query = 'DROP TABLE facts' AND wait_event_type = 'Lock'")" == 1
release sift-maint-after-table-lock
wait "$dp"
check "the worker goes on and DROP TABLE ends" test $? == 0
check "no error, no deadlock" test "$(errors)" == 0
idle

# (b)
setup || bail "second setup failed"
sql lo "SELECT injection_points_attach('sift-maint-before-table-lock', 'wait')" >/dev/null
sql lo "UPDATE facts SET amount = amount + 1 WHERE id % 20 = 1" >/dev/null
check "a worker stops before it locks the table" await_worker sift-maint-before-table-lock
sql lo "DROP TABLE facts" >/dev/null
release sift-maint-before-table-lock
check "the worker finds the index gone and ends" idle
sleep 1
check "no error, no deadlock" test "$(errors)" == 0
finish
