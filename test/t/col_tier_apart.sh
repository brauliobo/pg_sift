#!/usr/bin/env bash
# The tier merge leaves clean segments that hold no superblock in common alone when they total more than sift.vacuum_rewrite_limit: the segments of a
# parallel build and of the promotions of appended rows, whose merge gains a reader nothing and writes all their bytes. Eight such segments stay eight
# after a VACUUM; a dead row in one of them makes the merge worth its bytes and they become one.
. "$(dirname "$0")/lib.bash"

plan 4
node_init ta 25 "autovacuum = off
sift.maintenance_naptime = 200
sift.vacuum_rewrite_limit = 1"
node_start ta || bail "start failed"
sql ta "CREATE EXTENSION pg_sift" "CREATE TABLE facts (id int, key int, kind text, amount numeric(10,2))" \
	"INSERT INTO facts SELECT g, (hashint8(g) & 1048575)::int, 'cat_' || (g % 8), (hashint8(g + 1) & 65535) / 100.0 FROM generate_series(1, 100000) g" \
	"CREATE INDEX facts_idx ON facts USING sift_col (id, key, kind, amount)" "VACUUM facts" >/dev/null || bail "setup failed"
# eight appended batches, each promoted to a segment of its own over superblocks no other segment holds
for b in 1 2 3 4 5 6 7 8; do
	sql ta "INSERT INTO facts SELECT g, (hashint8(g) & 1048575)::int, 'cat_' || (g % 8), (hashint8(g + 1) & 65535) / 100.0 FROM generate_series($b * 100000 + 1, $b * 100000 + 100000) g" \
		"SELECT sift.col_promote('facts_idx')" >/dev/null || bail "batch $b failed"
done
segments() { sql ta "SELECT count(*) FROM sift.col_segments('facts_idx') WHERE kind = 'immutable'"; }
groups() { sql ta "SET sift.enable_agg = $1" "SET max_parallel_workers_per_gather = 0" "SELECT string_agg(x::text, ',' ORDER BY x::text) FROM (SELECT kind, count(*), sum(amount) FROM facts GROUP BY kind) x" | tail -1; }

before=$(segments)
sql ta "VACUUM facts" >/dev/null
check "clean segments that meet at an edge: the VACUUM leaves the tier alone" test "$(segments)" == "$before"
check "the index verifies and answers as the heap does" test "$(sql ta "SELECT sift.col_verify('facts_idx', true)" 2>&1 | grep -c ERROR)" == 0 -a "$(groups on)" == "$(groups off)"
sql ta "DELETE FROM facts WHERE id % 1000 = 7" "VACUUM facts" >/dev/null
check "a dead row in them: the VACUUM merges the tier" test "$(segments)" -lt "$before"
check "and the index still verifies" test "$(sql ta "SELECT sift.col_verify('facts_idx', true)" 2>&1 | grep -c ERROR)" == 0 -a "$(groups on)" == "$(groups off)"

finish
