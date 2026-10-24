#!/usr/bin/env bash
# A crash in the middle of the rewrite of a sift_col index (sift.col_maintain, the work of sift.col_auto_merge): the rewrite promotes the write buffer, then
# merges, and the PANIC comes after the merge wrote its new segment and before it replaced the old ones. Crash recovery leaves the index consistent: every
# step is a physical change the log replays, so the promotion may be there and the merge never is (the segments it was to replace are as they were, the answers
# are the heap's), the rewrite is still due, no block stays unaccounted for after the VACUUMs that sweep the orphan segment, and the rewrite asked again completes.
. "$(dirname "$0")/lib.bash"

plan 10
node_init cr 21 "autovacuum = off"
node_start cr || bail "start failed"
sql cr "CREATE EXTENSION pg_sift_test CASCADE" "CREATE TABLE facts (id int, key int, kind text, amount numeric(10,2), pad text) WITH (autovacuum_enabled = false)" \
	"INSERT INTO facts SELECT g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 150) FROM generate_series(1, 60000) g" \
	"CREATE INDEX facts_idx ON facts USING sift_col (id, key, kind, amount)" "VACUUM facts" \
	"UPDATE facts SET amount = amount + 1 WHERE id % 100 = 1" "DELETE FROM facts WHERE id % 100 = 51" \
	"INSERT INTO facts SELECT 1000000 + g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 150) FROM generate_series(1, 600) g" "VACUUM facts" >/dev/null || bail "setup failed"

# the log of a VACUUM is flushed by the WAL writer, not at its commit (no transaction id), so a crash within 200 ms would take the whole VACUUM with it
sql cr "CHECKPOINT" >/dev/null
answer() { sql cr "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = $1" "SELECT string_agg(x::text, ',' ORDER BY x::text) FROM (SELECT kind, count(*), sum(amount) FROM facts GROUP BY kind) x" | tail -1; }
due() { sql cr "SELECT coalesce(sift.col_maintenance_due('facts_idx'), 'not due')"; }
# the blocks the index holds that are neither a segment's nor on the free list (the metapage, the directory and the like: 3 here)
unaccounted() { sql cr "SELECT relation_blocks - free_blocks - (SELECT sum(bytes) / 8192 FROM sift.col_segments('facts_idx'))::bigint FROM sift.col_index_info('facts_idx')"; }
# the segments of the rewrite, as they were: generation, rows, whether a row is dead
OLD="SELECT gen, rows, dirty, dead_rows FROM sift.col_segments('facts_idx') WHERE kind = 'immutable' ORDER BY gen"
HEAP=$(answer off)
BEFORE=$(sql cr "$OLD")
check "scattered: the rewrite is due and the answers are the heap's" test "$(due)|$(answer on)" == "dirty units|$HEAP"

SIFT_TMP=$(dir cr) leak_expect 1 'PANIC:'
SIFT_TMP=$(dir cr) leak_expect 1 'terminated by signal 6'
maintain_panics() { ! sql cr "SET sift.test_col_merge_panic = on" "SELECT sift.col_maintain('facts_idx')" >/dev/null 2>&1; }
check "the rewrite PANICs once the merge wrote its new segment" maintain_panics
for _ in $(seq 300); do [[ -f $(dir cr)/data/postmaster.pid ]] || break; sleep 0.1; done
check "the server restarts through crash recovery" node_start cr
check "the index verifies after the crash" test -z "$(sql cr "SELECT sift.col_verify('facts_idx', true)" 2>&1)"
check "the segments the merge was to replace are untouched" test "$(sql cr "SELECT gen, rows, dirty, dead_rows FROM sift.col_segments('facts_idx') WHERE kind = 'immutable' AND gen <= $(sed -n '$ s/|.*//p' <<<"$BEFORE") ORDER BY gen")" == "$BEFORE"
check "the answers are the heap's and the rewrite is still due" test "$(answer on)|$(due)" == "$HEAP|dirty units"

# the new segment of the merge is an orphan the log may or may not have kept: two VACUUMs leave no block unaccounted for
sql cr "VACUUM facts" "VACUUM facts" >/dev/null
check "after VACUUM every block is a segment's or free" test "$(unaccounted)" -le 4
check "the rewrite, asked again, completes: one clean segment" test "$(sql cr "SELECT sift.col_maintain('facts_idx')")|$(sql cr "SELECT count(*) FILTER (WHERE kind = 'immutable') || ' ' || count(*) FILTER (WHERE dirty) FROM sift.col_segments('facts_idx')")" == "t|1 0"
sql cr "VACUUM facts" "VACUUM facts" >/dev/null
check "the index verifies, the answers are the heap's and every block is accounted for" test "$(sql cr "SELECT sift.col_verify('facts_idx', true)" 2>&1)|$(answer on)|$([[ $(unaccounted) -le 4 ]] && echo ok)" == "|$HEAP|ok"
check "nothing is due after it" test "$(due)" == "not due"
finish
