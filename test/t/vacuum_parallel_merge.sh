#!/usr/bin/env bash
# A VACUUM that vacuums the b-trees of a table with parallel workers is in parallel mode for the cleanup of its other indexes, where no transaction id can be
# assigned: the merge of a BM25 index there is made by one process (its merge by workers takes an id before it enters a parallel mode of its own), in that
# VACUUM, with no failure counted and no WARNING, and leaves the answers as they were.
. "$(dirname "$0")/lib.bash"

plan 7
node_init pm 21 "autovacuum = off
max_worker_processes = 16
max_parallel_workers = 8"
node_start pm || bail "start failed"
sql pm "CREATE EXTENSION pg_sift" "CREATE TABLE d (id int, body text) WITH (autovacuum_enabled = off)" \
	"INSERT INTO d SELECT i, 'k' || (i % 7) || ' g' || (i % 1000) || ' common word' || (i % 50) FROM generate_series(1, 30000) i" \
	"CREATE INDEX d_text ON d USING sift (body) WITH (build_segments = 60)" "CREATE INDEX d_a ON d (id)" "CREATE INDEX d_b ON d (body)" \
	"DELETE FROM d WHERE id % 3 = 0" >/dev/null || bail "setup failed"
ANSWER="SELECT count(*), sum(id) FROM d WHERE body @@> 'common'"
segments() { sql pm "SELECT count(*) FROM sift.segments('d_text')"; }
BEFORE=$(sql pm "$ANSWER")
SEG0=$(segments)
out=$(sql pm "SET client_min_messages = info" "SET sift.test_range_bytes = 4000" "SET min_parallel_index_scan_size = 0" \
	"SET max_parallel_maintenance_workers = 2" "VACUUM (VERBOSE) d" 2>&1)

check "the index starts in many segments" test "$SEG0" -ge 60
check "the VACUUM vacuumed the b-trees with a parallel worker" grep -q 'launched 1 parallel vacuum worker' <<<"$out"
check "it gave no warning" test -z "$(grep WARNING <<<"$out")"
check "no failure was counted" test "$(sql pm "SELECT failures FROM sift.workers WHERE index = 'd_text'::regclass")" == 0
check "the segments were merged by that VACUUM" test "$(segments)" -lt "$SEG0"
check "the answers are the same" test "$(sql pm "$ANSWER")" == "$BEFORE"
check "the index verifies" test "$(sql pm "SELECT sift.verify('d_text', true)")" == ""
finish
