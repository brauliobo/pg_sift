#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# A column delta staged for a partition that a merge of neighbouring partitions took away before the commit goes to the partition that holds its row now, and the commit ends: no error and no
# crash, the rows are updated and sift.check finds nothing. A compaction of a sift_col index with a scope refreshes the summary in the commit that publishes its parts, so the scoped statement
# right after reads the summary (at most twice the buffers of the clean index, never the heap join) and answers what the join does.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 9
node_init cs 3 "autovacuum = off
sift.async_maintenance = off
shared_buffers = 64MB"
node_start cs || bail "start failed"
LOG=$(dir cs)/postgres.log
sql cs "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" "SET sift.test_create_format1 = off" \
	"CREATE TABLE cd (id int8, v int4, pad text) USING sift_table" "SELECT sift.table_set_option('cd', 'key_order', 'id')" \
	"INSERT INTO cd SELECT g, g, md5(g::text) FROM generate_series(1, 20000) g" "VACUUM cd" \
	"SELECT sift._test_f2_split('cd', 1, NULL, 20001)" \
	"INSERT INTO cd SELECT g, g, md5(g::text) FROM generate_series(20001, 40000) g" "VACUUM cd" >/dev/null || bail "setup failed"
check "the table has two partitions" test "$(sql cs "SELECT count(*) FROM sift._test_f2_partitions('cd')")" == 2

# a transaction stages its column deltas for the upper partition, the partitions merge, the transaction commits
sess_open cs writer
sess_say writer "BEGIN; UPDATE cd SET v = v + 1000000 WHERE id > 30000 AND id <= 30100; SELECT 'staged';"
wait_for "grep -q staged $(dir cs)/writer.out" 30
sql cs "SELECT sift._test_f2_merge('cd', 1, NULL, 2, 20001)" >/dev/null
check "the partitions are one" test "$(sql cs "SELECT count(*) FROM sift._test_f2_partitions('cd')")" == 1
sess_say writer "COMMIT; SELECT 'done';"
wait_for "grep -q done $(dir cs)/writer.out" 30
sess_close writer
check "the commit after the merge ends without an error" test "$(grep -c 'ERROR\|server closed\|lost' "$(dir cs)/writer.out")" == 0
check "the rows are updated" test "$(sql cs "SELECT count(*) FROM cd WHERE v = id + 1000000")" == 100
check "sift.check finds nothing" test "$(sql cs "SELECT count(*) FROM sift.check('cd'::regclass, true)")" == 0
check "no backend failed an invariant or died" test "$(grep -c 'INVARIANT\|terminated by signal' "$LOG")" == 0

sql cs "CREATE TABLE pairs (member int4 NOT NULL, scope_key int4 NOT NULL)" "INSERT INTO pairs SELECT m, m % 3 + 1 FROM generate_series(1, 4) m" \
	"CREATE UNIQUE INDEX pairs_pk ON pairs (member, scope_key)" \
	"CREATE TABLE ev (id int8, grp int4, ts timestamptz, val int4, pad text)" \
	"INSERT INTO ev SELECT g, g % 4 + 1, timestamptz '2025-01-01' + g * interval '1 second', hashtext(g::text), md5(g::text) FROM generate_series(1, 30000) g" \
	"CREATE INDEX ev_idx ON ev USING sift_col (grp, ts, val, pad) WITH (key_order = 'grp, ts', scope = 'sc: grp -> pairs(member, scope_key)')" "VACUUM ev" >/dev/null || bail "scope setup failed"
SCOPED="SELECT count(*), sum(e.val) FROM ev e JOIN pairs p ON p.member = e.grp WHERE p.scope_key = 2"
sql cs "CREATE FUNCTION buffers_of(q text) RETURNS bigint LANGUAGE plpgsql AS \$f\$ DECLARE j jsonb; BEGIN EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) ' || q INTO j;
	RETURN coalesce((j->0->'Plan'->>'Shared Hit Blocks')::bigint, 0) + coalesce((j->0->'Plan'->>'Shared Read Blocks')::bigint, 0); END \$f\$" >/dev/null
buffers() { sql cs "SET max_parallel_workers_per_gather = 0" "SELECT buffers_of('$SCOPED')"; }
answer() { sql cs "SET sift.enable_agg = $1" "SET max_parallel_workers_per_gather = 0" "$SCOPED"; }
scope_read() { sql cs "SET max_parallel_workers_per_gather = 0" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) $SCOPED" | sed -n 's/^ *Scope Read: //p'; }
CLEAN=$(buffers)

sql cs "DELETE FROM ev WHERE id <= 7500" "INSERT INTO ev SELECT g + 30000, 1, timestamptz '2025-02-01', g, md5(g::text) FROM generate_series(1, 500) g" "VACUUM ev" >/dev/null
sql cs "SELECT sift.merge('ev_idx'::regclass, true)" >/dev/null
check "the compaction left no error" test "$(grep -c 'INVARIANT\|terminated by signal' "$LOG")" == 0
check "the scoped statement right after the compaction reads at most twice the buffers of the clean index" test "$(buffers)" -le $((2 * CLEAN))
check "it reads the summary and answers what the join does" test "$(scope_read)|$(answer on)" == "summary|$(answer off)"
