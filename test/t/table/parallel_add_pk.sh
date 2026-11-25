#!/usr/bin/env bash
# A parallel build of a unique index on a sift_table: the participants scan the table with the snapshot PostgreSQL gives them, SnapshotAny for a build that holds off the writers (ADD PRIMARY KEY,
# CREATE UNIQUE INDEX), which sees every version a commit has deleted until a merge drops it. The index gets the rows a serial build reads, so a key an UPDATE rewrote is once in it, and each
# participant ends the scan it was given. Twelve rounds rewrite a third of the rows and add the primary key in parallel again; the log says each build was parallel and shows no leak.
. "$(dirname "$0")/lib.bash"

plan 7
node_init pk 80 "autovacuum = off
sift.async_maintenance = off
sift.test_create_format1 = off
max_worker_processes = 16
max_parallel_workers = 8
max_parallel_maintenance_workers = 3
min_parallel_table_scan_size = 0
log_min_messages = debug1"
node_start pk || bail "start failed"
sql pk "CREATE EXTENSION pg_sift" "CREATE TABLE q (k int, v int) USING sift_table" "ALTER TABLE q SET (parallel_workers = 3)" \
	"INSERT INTO q SELECT g, 0 FROM generate_series(1, 60000) g" >/dev/null || bail "setup failed"

rounds() {
	local i
	for i in $(seq 1 12); do
		sql pk "UPDATE q SET v = v + 1 WHERE k % 3 = $((i % 3))" "ALTER TABLE q ADD PRIMARY KEY (k)" \
			"SELECT reltuples::int FROM pg_class WHERE oid = 'q_pkey'::regclass" "ALTER TABLE q DROP CONSTRAINT q_pkey" 2>&1 | grep -v '^60000$' || true
	done
}
out=$(rounds)
check "12 rounds of UPDATE and ADD PRIMARY KEY: every build succeeds and its index has one entry for each row" test -z "$out"
out=$(grep -ac 'building index "q_pkey" on table "q" with request for [1-9] parallel workers' "$(dir pk)/postgres.log")
check "each of the 12 builds was parallel" test "$out" -eq 12
out=$(sql pk "SELECT count(*) || ' ' || count(DISTINCT k) || ' ' || sum(v) FROM q")
check "the rows are exact" test "$out" == "60000 60000 240000"

sql pk "UPDATE q SET v = v + 1 WHERE k % 2 = 0" >/dev/null
out=$(sql pk "CREATE UNIQUE INDEX q_uk ON q (k)" "SELECT reltuples::int FROM pg_class WHERE oid = 'q_uk'::regclass" 2>&1)
check "a parallel CREATE UNIQUE INDEX after more UPDATEs has one entry for each row" test "$out" == "60000"
sql pk "DROP INDEX q_uk" >/dev/null
sql pk "UPDATE q SET v = v + 1 WHERE k % 2 = 1" >/dev/null
out=$(sql pk "CREATE UNIQUE INDEX CONCURRENTLY q_uc ON q (k)" "SELECT indisvalid FROM pg_index WHERE indexrelid = 'q_uc'::regclass" 2>&1)
check "a parallel CREATE UNIQUE INDEX CONCURRENTLY is valid" test "$out" == "t"
check "no scan or reference was left open" test "$(grep -ac 'was not closed' "$(dir pk)/postgres.log")" -eq 0
check "no backend died" test "$(grep -ac 'terminated by signal\|TRAP:' "$(dir pk)/postgres.log")" -eq 0
finish
