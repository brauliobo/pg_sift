#!/usr/bin/env bash
# The TTL drops parts without a row delete that logical decoding could carry, so a table in a logical publication gets no TTL: sift.set_ttl refuses it, and a publication made after the TTL
# stops the drops (a warning in the log) instead of leaving the subscribers with rows the table has lost.
. "$(dirname "$0")/lib.bash"

plan 4
node_init tp 4 "autovacuum = off
shared_preload_libraries = 'pg_sift'
wal_level = logical"
node_start tp || bail "start failed"
sql tp "CREATE EXTENSION pg_sift" "CREATE TABLE t (id int8, ts timestamp NOT NULL, v int) USING sift_table" \
	"INSERT INTO t SELECT i, timestamp '2020-01-01' + i * interval '1 hour', i % 100 FROM generate_series(1, 5000) i" "VACUUM (FREEZE) t" \
	"CREATE PUBLICATION p98_pub FOR TABLE t" >/dev/null || bail "setup failed"
out=$(sql tp "SELECT sift.set_ttl('t', 'ts', interval '30 days')" 2>&1)
check "a TTL on a table in a publication is refused" bash -c '[[ $1 == *"logical publication"* ]]' _ "$out"
check "and no policy was written" test "$(sql tp "SELECT count(*) FROM sift.ttl_policies")" == 0
sql tp "DROP PUBLICATION p98_pub" "SELECT sift.set_ttl('t', 'ts', interval '30 days')" "CREATE PUBLICATION p98_pub FOR TABLE t" "VACUUM t" >/dev/null 2>&1
check "a table that was published after its TTL keeps its rows" test "$(sql tp "SELECT count(*) FROM t")" == 5000
check "and the log says why the TTL did not run" grep -q "in a logical publication" "$(dir tp)/postgres.log"

finish
