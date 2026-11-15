#!/usr/bin/env bash
# An append edit (sift_page_edit_*, the delta record the write buffers use) changes the buffer before it logs anything, inside a critical section: a change that reaches the special space of the
# page, or a region of the log record past the page, ends in a PANIC, the server restarts from the log that has none of the change, and the index verifies (core/lane.c edit_check). The change
# inside the bytes the edit logs goes through.
. "$(dirname "$0")/lib.bash"

node_init eg 55
node_start eg || bail "start failed"
D=$(dir eg)
SIFT_TMP=$D leak_expect 4 'terminated by signal 6|PANIC:  sift index'
sql eg "CREATE EXTENSION pg_sift_test CASCADE" "CREATE TABLE t (v int)" "CREATE INDEX t_idx ON t USING sift_store_test (v)" >/dev/null || bail "setup failed"

page() { sql eg "SELECT lower, upper, type, gen FROM sift._test_page('t_idx', 1)"; }
before=$(page)
out=$(sql eg "SELECT sift._test_write('t_idx', 1, 30, 2, true)" 2>&1)
check "an edit inside its head is logged" test $? == 0
for w in "8184 1" "8174 4"; do
	pm=$(head -1 "$D/data/postmaster.pid")
	out=$(sql eg "SELECT sift._test_write('t_idx', 1, ${w% *}, ${w#* }, true)" 2>&1)
	check "an edit of $w (offset, bytes) is refused with a PANIC" grep -q "PANIC:  sift index \"t_idx\": a change of block 1 was refused" <<<"$out"
	for _ in $(seq 600); do kill -0 "$pm" 2>/dev/null || break; sleep 0.1; done
	node_start eg || { not_ok "restart after the edit of $w"; diag "$(tail -20 "$D/postgres.log")"; break; }
	check "after the edit of $w the page is as it was" test "$(page)" == "$before"
	check "after the edit of $w the index verifies" test "$(sql eg "SELECT sift._test_check('t_idx')")" == 0
done
plan "$NTEST"
finish
