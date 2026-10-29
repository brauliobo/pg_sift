#!/usr/bin/env bash
# An index over sift.vacuum_rewrite_limit is rewritten by partitions of that size, one step each, and not skipped: with the limit at 1 MB the rewrite of a
# 2 MB index leaves several segments (a whole-index rewrite leaves one), the index verifies, the answers are the heap's, and it is not due again. The VACUUM of
# a table with a dead row in every unit rewrites it (the scans of the dirty units cost more than the rewrite); with dead_rewrite_pct 50 it leaves the dead rows
# and the server's own rule finds the index due, for col_maintain.
. "$(dirname "$0")/lib.bash"

plan 7
node_init sl 24 "autovacuum = off
sift.maintenance_naptime = 200
sift.vacuum_rewrite_limit = 1"
node_start sl || bail "start failed"
sql sl "CREATE EXTENSION pg_sift" "CREATE TABLE facts (id int, key int, kind text, amount numeric(10,2), pad text)" \
	"INSERT INTO facts SELECT g, (hashint8(g) & 1048575)::int, 'cat_' || (g % 8), (hashint8(g + 1) & 65535) / 100.0, repeat('x', 100) FROM generate_series(1, 400000) g" \
	"CREATE INDEX facts_idx ON facts USING sift_col (id, key, kind, amount)" "VACUUM facts" >/dev/null || bail "setup failed"

segments() { sql sl "SELECT count(*) FROM sift.col_segments('facts_idx') WHERE kind = 'immutable'"; }
groups() { sql sl "SET sift.enable_agg = $1" "SET max_parallel_workers_per_gather = 0" "SELECT string_agg(x::text, ',' ORDER BY x::text) FROM (SELECT kind, count(*), sum(amount) FROM facts GROUP BY kind) x" | tail -1; }

dead() { sql sl "SELECT coalesce(sum(dead_rows), 0) FROM sift.col_segments('facts_idx')"; }
verified() { test "$(sql sl "SELECT sift.col_verify('facts_idx', true)" 2>&1 | grep -c ERROR)" == 0 -a "$(groups on)" == "$(groups off)"; }
over() { sql sl "SELECT bytes > 1048576 FROM (SELECT sum(bytes) AS bytes FROM sift.col_segments('facts_idx') WHERE kind = 'immutable') s"; }

# the default rule: the VACUUM rewrites the index by partitions
sql sl "UPDATE facts SET amount = amount + 1 WHERE id % 10 = 3" "VACUUM facts" >/dev/null
check "the VACUUM rewrote the index over the limit by partitions" test "$(over)" == t -a "$(dead)" == 0 -a "$(segments)" -ge 2
check "the index verifies and answers as the heap does" verified

# a VACUUM that leaves the dead rows: the rule finds the index due and col_maintain rewrites it by partitions
sql sl "ALTER INDEX facts_idx SET (dead_rewrite_pct = 50)" "UPDATE facts SET amount = amount + 1 WHERE id % 10 = 7" "VACUUM facts" "ALTER INDEX facts_idx RESET (dead_rewrite_pct)" >/dev/null
check "the index is over the limit with its dead rows" test "$(over)" == t -a "$(dead)" -gt 0
check "the server's own rule finds it due" test "$(sql sl "SELECT coalesce(sift.col_maintenance_due('facts_idx', true), '-')")" != "-"
sql sl "SELECT sift.col_maintain('facts_idx')" >/dev/null
check "its rewrite leaves partitions and no dead row" test "$(segments)" -ge 2 -a "$(dead)" == 0
check "the index verifies and answers as the heap does" verified
check "it is not due again" test "$(sql sl "SELECT coalesce(sift.col_maintenance_due('facts_idx', true), '-')")" == "-"

finish
