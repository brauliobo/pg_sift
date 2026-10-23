#!/usr/bin/env bash
# A crash in the middle of the rewrite of a sift_col index by range (sift.col_auto_merge_partition_kb): the one segment is cut into partitions, and the PANIC comes
# once the first partition is written and before the directory switch that replaces the old segment with all of them. Crash recovery leaves the index consistent:
# the segment is as it was, the answers are the heap's, the rewrite is still due, no block stays unaccounted for after the VACUUMs that sweep the orphan partition,
# and the rewrite asked again completes into partitions. Then a second PANIC, in the rewrite of one dirty partition of those, leaves the others as they were.
. "$(dirname "$0")/lib.bash"

plan 13
node_init pc 25 "autovacuum = off"
node_start pc || bail "start failed"
sql pc "CREATE EXTENSION pg_sift_test CASCADE" "CREATE TABLE facts (id int, key int, kind text, amount numeric(10,2), pad text) WITH (autovacuum_enabled = false)" \
	"INSERT INTO facts SELECT g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 150) FROM generate_series(1, 80000) g" \
	"CREATE INDEX facts_idx ON facts USING sift_col (id, key, kind, amount)" "VACUUM facts" >/dev/null || bail "setup failed"
# a quarter of the index
PART=$(sql pc "SELECT (sum(bytes) / 4096)::int FROM sift.col_segments('facts_idx')")
sql pc "ALTER DATABASE postgres SET sift.col_auto_merge_partition_kb = $PART" "ALTER DATABASE postgres SET sift.async_maintenance = off" >/dev/null
churn() { sql pc "UPDATE facts SET amount = amount + 1 WHERE id >= $1 AND id % 100 = $2" "DELETE FROM facts WHERE id >= $1 AND id % 100 = $2 + 50" \
	"INSERT INTO facts SELECT 1000000 * $2 + g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 150) FROM generate_series(1, 300) g" "VACUUM facts" >/dev/null; }
churn 0 1

# the log of a VACUUM is flushed by the WAL writer, not at its commit (no transaction id), so a crash within 200 ms would take the whole VACUUM with it
sql pc "CHECKPOINT" >/dev/null
answer() { sql pc "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = $1" "SELECT string_agg(x::text, ',' ORDER BY x::text) FROM (SELECT kind, count(*), sum(amount) FROM facts GROUP BY kind) x" | tail -1; }
due() { sql pc "SELECT coalesce(sift.col_maintenance_due('facts_idx'), 'not due')"; }
# the blocks the index holds that are neither a segment's nor on the free list (the metapage, the directory and the like: 3 here)
unaccounted() { sql pc "SELECT relation_blocks - free_blocks - (SELECT sum(bytes) / 8192 FROM sift.col_segments('facts_idx'))::bigint FROM sift.col_index_info('facts_idx')"; }
segments() { sql pc "SELECT count(*) FILTER (WHERE kind = 'immutable') || ' segments, ' || count(*) FILTER (WHERE dirty) || ' dirty' FROM sift.col_segments('facts_idx')"; }
maintain_panics() { ! sql pc "SET sift.test_col_merge_panic = on" "SELECT sift.col_maintain('facts_idx')" >/dev/null 2>&1; }
restarted() { for _ in $(seq 300); do [[ -f $(dir pc)/data/postmaster.pid ]] || break; sleep 0.1; done; node_start pc; }
# a mismatch puts both sides in the TAP diagnostics
same() { [[ $1 == "$2" ]] || diag "got:"$'\n'"$1"$'\n'"want:"$'\n'"$2"; [[ $1 == "$2" ]]; }
OLD="SELECT gen, rows, dirty, dead_rows FROM sift.col_segments('facts_idx') WHERE kind = 'immutable' ORDER BY gen"
HEAP=$(answer off)
BEFORE=$(sql pc "$OLD")
check "scattered: the rewrite is due and the answers are the heap's" test "$(due)|$(answer on)" == "dirty units|$HEAP"

SIFT_TMP=$(dir pc) leak_expect 2 'PANIC:'
SIFT_TMP=$(dir pc) leak_expect 2 'terminated by signal 6'
check "the rewrite PANICs once the first partition is written" maintain_panics
check "the server restarts through crash recovery" restarted
check "the index verifies after the crash" test -z "$(sql pc "SELECT sift.col_verify('facts_idx', true)" 2>&1)"
check "the segment the rewrite was to replace is untouched" same "$(sql pc "$OLD")" "$BEFORE"
check "the answers are the heap's and the rewrite is still due" test "$(answer on)|$(due)" == "$HEAP|dirty units"

# the partition the first step wrote is an orphan the log may or may not have kept: two VACUUMs leave no block unaccounted for
sql pc "VACUUM facts" "VACUUM facts" >/dev/null
check "after VACUUM every block is a segment's or free" test "$(unaccounted)" -le 4
check "the rewrite, asked again, completes: partitions, none dirty" test "$(sql pc "SELECT sift.col_maintain('facts_idx')")|$(segments)" == "t|5 segments, 0 dirty"
sql pc "VACUUM facts" "VACUUM facts" >/dev/null
check "the index verifies, the answers are the heap's and every block is accounted for" test "$(sql pc "SELECT sift.col_verify('facts_idx', true)" 2>&1)|$(answer on)|$([[ $(unaccounted) -le 4 ]] && echo ok)" == "|$HEAP|ok"

# the churn of the newest rows dirties the last partition: its rewrite PANICs after it wrote its partition, the other three are as they were
churn 76000 2
sql pc "CHECKPOINT" >/dev/null
HEAP=$(answer off)
BEFORE=$(sql pc "$OLD")
check "the window of churn makes the last partition due" test "$(due)" == "dirty units"
check "the rewrite of that partition PANICs too" maintain_panics
check "the server restarts" restarted
check "the partitions are as they were, the answers the heap's, the index verifies" same "$(sql pc "$OLD")|$(answer on)|$(sql pc "SELECT sift.col_verify('facts_idx', true)" 2>&1)" "$BEFORE|$HEAP|"
finish
