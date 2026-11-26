#!/usr/bin/env bash
# DROP TABLE gives the queued maintenance of the table back at once, in the transaction that drops it: the slots do not wait for a worker to find the table gone. The table is
# held by the session that drops it, so the worker that claims one of its requests waits on it and the other requests stay queued until the DROP.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 3
node_init dp 2 "autovacuum = off
sift.test_create_format1 = off
sift.async_maintenance = on
sift.max_maint_workers = 1"
node_start dp || bail "start failed"
sql dp "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" "CREATE TABLE t (id int) USING sift_table" \
	"INSERT INTO t SELECT g FROM generate_series(1, 1000) g" >/dev/null || bail "setup failed"
pending() { sql dp "SELECT pending FROM sift._test_maint_stats()"; }
wait_for "[[ \$(pending) == 0 ]]" 30

sess_open dp drop
sess_say drop "BEGIN; LOCK TABLE t IN ACCESS EXCLUSIVE MODE;"
sess_say drop "SELECT sift._test_maint_enqueue('t'::regclass::oid, k) FROM generate_series(1, 3) k;"
check "three requests of the table are queued" wait_for "[[ \$(pending) == 3 ]]" 10
sess_say drop "DROP TABLE t; SELECT 'dropped';"
wait_for "grep -q dropped '$(dir dp)/drop.out'" 10
out=$(pending)
check "the DROP gives back the requests no worker holds" test "$out" -le 1
sess_say drop "COMMIT;"
sess_close drop
check "the queue is empty once the DROP commits" wait_for "[[ \$(pending) == 0 ]]" 10
