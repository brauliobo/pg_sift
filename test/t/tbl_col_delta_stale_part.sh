#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# A transaction updates a column no index holds, which stages a column delta for a row of a part; another session rewrites that part (VACUUM of rows deleted before the
# transaction began) before the transaction commits. The commit finds the staged delta on a part that is gone and looks the row up in the part that replaced it: it must
# neither take the stack of the backend (the lookup flushed the stage it was flushing) nor lose the update.
. "$(dirname "$0")/lib.bash"

plan 5
node_init sp 97 "autovacuum = off
sift.maintenance_naptime = 60000
sift.test_create_format1 = off"
node_start sp || bail "start failed"
sql sp "CREATE EXTENSION pg_sift" \
	"CREATE TABLE cd (id bigint, a bigint, b text, c text, d text, e text) USING sift_table" \
	"INSERT INTO cd SELECT g, g, repeat('b', 40), repeat('c', 40), repeat('d', 40), repeat('e', 40) FROM generate_series(1, 6000) g" \
	"DELETE FROM cd WHERE id % 10 >= 3" >/dev/null || bail "setup failed"

sql sp "SET application_name = 'stale_writer'" "BEGIN" "UPDATE cd SET b = b || 'x' WHERE id IN (10, 2500, 5000)" "SELECT pg_sleep(4)" "COMMIT" >"$(dir sp)/writer.out" 2>&1 &
WRITER=$!
writer_staged() { [[ $(sql sp "SELECT count(*) FROM pg_stat_activity WHERE application_name = 'stale_writer' AND query LIKE 'SELECT pg_sleep(4)%'") == 1 ]]; }
wait_for writer_staged 30 || bail "the writer did not stage its update"
sql sp "VACUUM cd" >/dev/null
wait "$WRITER"
check "the commit of the writer ends" test "$?" -eq 0
check "the server stayed up" test "$(sql sp "SELECT 1")" == 1
check "the three updated rows are in the table" test "$(sql sp "SELECT count(*) FROM cd WHERE right(b, 1) = 'x'")" == 3
check "no other row changed" test "$(sql sp "SELECT count(*) FROM cd")" == 1800
check "sift.check finds nothing" test "$(sql sp "SELECT count(*) FROM sift.check('cd'::regclass)")" == 0
finish
