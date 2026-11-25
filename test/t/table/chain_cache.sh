#!/usr/bin/env bash
# The column delta chain of a partition is kept by the backend for the writes to its rows (src/table/f2/chain_cache.c). A chain that grows by one transaction after another is read from its
# newest page on, not whole each time: 40 transactions that each update 400 rows of a table whose chain grows by 3 pages read the pages of the chain once (sift._test_chain_cache_pages counts
# them), a few hundred, and not the 2,000 that a whole read of the chain for each transaction costs. The rows are as exact as ever, whatever happens to the chain in between: a writer of
# another session that adds pages to it, a VACUUM that folds it away and the writes that start a new one. The log of the pending deletes of a partition is read once when a transaction finds it
# changed (sift._test_pend_del_reads): 30 transactions that each delete a row and add it again read it fewer than 45 times, not the 60 of a read before and after the stamp of its last page.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 8
node_init cc 0 "autovacuum = off
sift.async_maintenance = off
sift.test_create_format1 = off"
node_start cc || bail "start failed"
sql cc "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE h (id int PRIMARY KEY, v bigint NOT NULL, pad text NOT NULL) USING sift_table" \
	"CREATE TABLE hs (id int PRIMARY KEY, v bigint NOT NULL, pad text NOT NULL)" \
	"CREATE TABLE p (k int PRIMARY KEY, v int NOT NULL) USING sift_table" \
	"INSERT INTO p SELECT g, g FROM generate_series(1, 4000) g" \
	"INSERT INTO h SELECT g, g, repeat('x', 40) FROM generate_series(1, 4000) g" \
	"INSERT INTO hs SELECT g, g, repeat('x', 40) FROM generate_series(1, 4000) g" \
	"VACUUM h" "SELECT sift.maintain('h')" >/dev/null || bail "setup failed"

BUMP="UPDATE %s SET v = v + 1 WHERE id %% 10 = %d;\n"
bump() { printf "$BUMP" "$1" "$2"; }
# other SQL: a statement run by a session of its own, from the session that runs the script
other() { printf '\\! psql -X -q -h %s -p %s -U postgres -d postgres -c "%s"\n' "$(dir cc)/sock" "$(port cc)" "$1"; }
same_rows() { sql cc "SELECT count(*) FROM (SELECT * FROM h EXCEPT SELECT * FROM hs) d, (SELECT 1) one" | tr -d '\n'; }

out=$({
	echo 'SELECT sift._test_chain_cache_pages() AS p0 \gset'
	for _ in $(seq 40); do bump h 0; bump hs 0; done
	echo 'SELECT sift._test_chain_cache_pages() - :p0;'
} | sql cc)
check "40 transactions that each grow the chain by 3 pages read fewer than 600 pages of it, not 2,000" test "$out" -lt 600
check "the rows are those of a heap table that took the same updates" test "$(same_rows)" == 0
out=$(sql cc "SELECT count(*) FROM sift.check('h', true)")
check "and the chain verifies" test "$out" == 0

{
	for _ in $(seq 6); do bump h 1; other "UPDATE h SET v = v + 1 WHERE id % 10 = 2"; bump h 2; done
} | sql cc >/dev/null
sql cc "$(for _ in $(seq 6); do printf 'UPDATE hs SET v = v + 1 WHERE id %% 10 = 1; UPDATE hs SET v = v + 1 WHERE id %% 10 = 2; UPDATE hs SET v = v + 1 WHERE id %% 10 = 2;'; done)" >/dev/null
check "the updates of a writer that adds pages between the statements of a session are all there" test "$(same_rows)" == 0

{
	bump h 3; bump hs 3
	other "VACUUM h"
	other "SELECT sift.maintain('h')"
	bump h 4; bump hs 4; bump h 3; bump hs 3
} | sql cc >/dev/null
check "the updates after a VACUUM folded the chain away are all there" test "$(same_rows)" == 0
out=$(sql cc "SELECT count(*) FROM sift.check('h', true)")
check "and the chain verifies" test "$out" == 0

out=$({
	echo 'SELECT sift._test_pend_del_reads() AS r0 \gset'
	for k in $(seq 30); do printf 'BEGIN;\nDELETE FROM p WHERE k = %d;\nINSERT INTO p VALUES (%d, 0);\nCOMMIT;\n' "$k" "$k"; done
	echo 'SELECT sift._test_pend_del_reads() - :r0;'
} | sql cc)
check "30 transactions that each delete a row and add it again read the pending deletes fewer than 45 times, not 60" test "$out" -lt 45
out=$(sql cc "SELECT count(*) || ' ' || sum(v) FROM p")
check "and the rows are those they left" test "$out" == "4000 8001535"
finish
