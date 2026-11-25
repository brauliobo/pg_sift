#!/usr/bin/env bash
# tap: pooled
# DROP TABLE beside a maintenance worker (the race between the lock order of the statements, table first and then its indexes, and a worker that opens an index
# for the table): an injection point stops the worker without a sleep (PostgreSQL built with --enable-injection-points: scripts/pg-assert.sh --inject), under
# sift.async_maintenance = on, the one switch of the background maintenance of 6.0 (sift.col_auto_merge is gone):
#   Format 2.0 (sift_col): the unified worker of src/core/maint_sched_pg.c, points sift-maint-f2-{before,after}-table-lock of sift_maint_open
#   Format 1.7 (BM25 sift): the worker of src/core/maint.c, points sift-maint-{before,after}-table-lock of open_index_table_first
# for each: (a) after it locked the table: DROP TABLE waits for the worker, it goes on and ends, DROP TABLE ends: no deadlock, no error;
# (b) before it locked the table: DROP TABLE ends first, the worker finds the relation gone and ends without an error.
# Both beside the assertions of the lock order (Assert in sift_try_open_index and sift_maint_open).
. "$(dirname "$0")/lib.bash"

pg_config --configure | grep -q -- --enable-injection-points || { echo "1..0 # SKIP PostgreSQL without injection points (scripts/pg-assert.sh --inject)"; exit 0; }
# 17 names the wait of a point differently
((PG_MAJOR >= 18)) || { echo "1..0 # SKIP the wait of an injection point is named from PostgreSQL 18"; exit 0; }

plan 17
node_init lo 26 "autovacuum = off
sift.async_maintenance = on
sift.max_maint_workers = 2
sift.maintenance_naptime = 1000"
node_start lo || bail "start failed"
D=$(dir lo)
sql lo "CREATE EXTENSION pg_sift" "CREATE EXTENSION injection_points" >/dev/null || bail "extensions failed"

retired_guc_gone() { [[ $(sql lo "SELECT current_setting('sift.col_auto_merge', true) IS NULL") == t ]]; }
check "sift.async_maintenance is on" test "$(sql lo "SHOW sift.async_maintenance")" == on
check "sift.col_auto_merge is not a setting any more" retired_guc_gone

setup_f2() {
	sql lo "SET sift.test_create_format1 = off" "CREATE TABLE facts (id int, key int, kind text, amount numeric(10,2), pad text)" \
		"INSERT INTO facts SELECT g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 100) FROM generate_series(1, 100000) g" \
		"CREATE INDEX facts_idx ON facts USING sift_col (id, key, kind, amount)" "VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) facts" >/dev/null
}
setup_bm25() {
	sql lo "CREATE TABLE facts (id serial, body text)" "CREATE INDEX facts_idx ON facts USING sift (body) WITH (write_buffer_limit = 64)" >/dev/null
}
# reads of the churned table queue the maintenance of its sift_col, and inserts past the write buffer of a BM25 index ask for its worker
drive_f2() {
	sql lo "UPDATE facts SET amount = amount + 1 WHERE id % 20 = 1" >/dev/null
	sql lo "SET max_parallel_workers_per_gather = 0" "SELECT kind, count(*), sum(amount) FROM facts GROUP BY kind" >/dev/null 2>&1
}
drive_bm25() {
	sql lo "INSERT INTO facts (body) SELECT string_agg(md5(g::text || j::text), ' ') FROM generate_series(1, 40) g, generate_series(1, 30) j GROUP BY g" >/dev/null
}

waiting() { sql lo "SELECT count(*) FROM pg_stat_activity WHERE wait_event_type = 'InjectionPoint' AND wait_event = '$1'"; }
await_worker() {
	local n
	for n in $(seq 600); do
		"$2"
		[[ $(waiting "$1") -gt 0 ]] && return 0
		sleep 0.2
	done
	return 1
}
errors() { grep -cE 'deadlock detected|ERROR|TRAP|cache lookup failed|could not open relation' "$D/postgres.log"; }
# injection_points_wakeup frees one waiter of a point, and a worker may have asked for another one at the same point
release() {
	local n
	sql lo "SELECT injection_points_detach('$1')" >/dev/null
	while n=$(waiting "$1"); ((n > 0)); do
		sql lo "SELECT injection_points_wakeup('$1')" >/dev/null
		wait_for "((\$(waiting $1) < $n))" 60
	done
}
idle() { for _ in $(seq 300); do [[ $(sql lo "SELECT count(*) FROM pg_stat_activity WHERE wait_event_type = 'InjectionPoint'") == 0 ]] && return 0; sleep 0.1; done; return 1; }
unified_worker_waits() { [[ $(sql lo "SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'sift maintenance' AND wait_event = '$1'") -ge 1 ]]; }

# race LABEL FORMAT BEFORE-POINT AFTER-POINT
race() {
	local label=$1 fmt=$2 before=$3 after=$4 dp

	"setup_$fmt" || bail "$label: setup failed"
	sql lo "SELECT injection_points_attach('$after', 'wait')" >/dev/null
	"drive_$fmt"
	check "$label: a worker stops after it locked the table" await_worker "$after" "drive_$fmt"
	[[ $fmt != f2 ]] || check "$label: it is a worker of the unified queue" unified_worker_waits "$after"
	sql lo "DROP TABLE facts" >"$D/drop.out" 2>&1 & dp=$!
	for _ in $(seq 100); do [[ $(sql lo "SELECT count(*) FROM pg_stat_activity WHERE query = 'DROP TABLE facts' AND wait_event_type = 'Lock'") == 1 ]] && break; sleep 0.1; done
	check "$label: the DROP TABLE is in the lock queue of the worker's table lock" test "$(sql lo "SELECT count(*) FROM pg_stat_activity WHERE query = 'DROP TABLE facts' AND wait_event_type = 'Lock'")" == 1
	release "$after"
	wait "$dp"
	check "$label: the worker goes on and DROP TABLE ends" test $? == 0
	check "$label: no error, no deadlock" test "$(errors)" == 0
	idle

	"setup_$fmt" || bail "$label: second setup failed"
	sql lo "SELECT injection_points_attach('$before', 'wait')" >/dev/null
	"drive_$fmt"
	check "$label: a worker stops before it locks the table" await_worker "$before" "drive_$fmt"
	sql lo "DROP TABLE facts" >/dev/null
	release "$before"
	check "$label: the worker finds the relation gone and ends" idle
	sleep 1
	check "$label: no error, no deadlock" test "$(errors)" == 0
}

race "Format 2.0" f2 sift-maint-f2-before-table-lock sift-maint-f2-after-table-lock
race "Format 1.7 BM25" bm25 sift-maint-before-table-lock sift-maint-after-table-lock
finish
