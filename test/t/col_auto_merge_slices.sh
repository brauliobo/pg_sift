#!/usr/bin/env bash
# An index over sift.vacuum_rewrite_limit is rewritten by partitions of that size, one step each, and not skipped: with the limit at 1 MB the rewrite of a
# 2 MB index leaves several segments (a whole-index rewrite leaves one), the index verifies, the answers are the heap's, and it is not due again.
. "$(dirname "$0")/lib.bash"

plan 5
node_init sl 24 "autovacuum = off
sift.maintenance_naptime = 200
sift.vacuum_rewrite_limit = 1"
node_start sl || bail "start failed"
sql sl "CREATE EXTENSION pg_sift" "CREATE TABLE facts (id int, key int, kind text, amount numeric(10,2), pad text)" \
	"INSERT INTO facts SELECT g, (hashint8(g) & 1048575)::int, 'cat_' || (g % 8), (hashint8(g + 1) & 65535) / 100.0, repeat('x', 100) FROM generate_series(1, 400000) g" \
	"CREATE INDEX facts_idx ON facts USING sift_col (id, key, kind, amount)" "VACUUM facts" >/dev/null || bail "setup failed"

segments() { sql sl "SELECT count(*) FROM sift.col_segments('facts_idx') WHERE kind = 'immutable'"; }
groups() { sql sl "SET sift.enable_agg = $1" "SET max_parallel_workers_per_gather = 0" "SELECT string_agg(x::text, ',' ORDER BY x::text) FROM (SELECT kind, count(*), sum(amount) FROM facts GROUP BY kind) x" | tail -1; }

sql sl "UPDATE facts SET amount = amount + 1 WHERE id % 10 = 3" "VACUUM facts" >/dev/null
check "the index is over the limit" test "$(sql sl "SELECT bytes > 1048576 FROM (SELECT sum(bytes) AS bytes FROM sift.col_segments('facts_idx') WHERE kind = 'immutable') s")" == t
check "the server's own rule finds it due" test "$(sql sl "SELECT coalesce(sift.col_maintenance_due('facts_idx', true), '-')")" != "-"
sql sl "SELECT sift.col_maintain('facts_idx')" >/dev/null
check "its rewrite leaves partitions" test "$(segments)" -ge 2
check "the index verifies and answers as the heap does" test "$(sql sl "SELECT sift.col_verify('facts_idx', true)" 2>&1 | grep -c ERROR)" == 0 -a "$(groups on)" == "$(groups off)"
check "it is not due again" test "$(sql sl "SELECT coalesce(sift.col_maintenance_due('facts_idx', true), '-')")" == "-"

finish
