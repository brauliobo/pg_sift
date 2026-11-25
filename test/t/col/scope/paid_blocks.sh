#!/usr/bin/env bash
# A read that takes its blocks from the file instead of the buffers pays for them as well: MAINT_BLOCK_READ (20,000 instructions, the weight of a block in the price of a unit of the rewrite) for each block past
# the one of each unit it read. The same read, warm, pays the instructions of its arm alone and the same every time; after a restart (the buffers empty) its first run pays the blocks on top.
. "$(dirname "$0")/lib.bash"
plan 3

node_init pb 60 "autovacuum = off
sift.async_maintenance = on
sift.maintenance_naptime = 60000
sift.test_scope_patch = off
shared_buffers = 128MB"
node_start pb || bail "start failed"
sql pb "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE cp (m int NOT NULL, t int NOT NULL)" "CREATE UNIQUE INDEX cp_u ON cp (m, t)" \
	"INSERT INTO cp SELECT m, 1 + (m * 7 + j * 5) % 16 FROM generate_series(1, 2000) m, generate_series(0, 2) j WHERE j < m % 4 ON CONFLICT DO NOTHING" \
	"CREATE INDEX cp_j ON cp USING sift_col (m, t) WITH (track_changes = 'ctid')" \
	"CREATE TABLE ce (id int PRIMARY KEY, m int, d timestamptz, v int, pad text)" \
	"INSERT INTO ce SELECT i, 1 + (i * 7919) % 2100, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, 'x' FROM generate_series(1, 200000) i" \
	>/dev/null || bail "setup failed"
sql pb "VACUUM (FREEZE, ANALYZE) ce, cp" >/dev/null || bail "vacuum failed"
sql pb "CREATE INDEX ce_col ON ce USING sift_col (id, m, d, v) WITH (key_order = 'm, d', scope = 't: m -> cp(m, t) max_multiplicity(8)')" >/dev/null || bail "build failed"
sql pb "WITH d AS (DELETE FROM cp WHERE abs(hashtext(m || '/' || t || '/b')) % 100 < 2 RETURNING m, t) INSERT INTO cp SELECT 1 + (m + 37) % 2000, t FROM d ON CONFLICT DO NOTHING" "CHECKPOINT" >/dev/null

JOIN="SELECT count(*), sum(v) FROM ce e WHERE e.m IN (SELECT m FROM cp WHERE t = 6)"
# one session: three reads, and the sum after each (the naptime of this node is the longest: nothing else asks)
out=$(sql pb "SET sift.agg_force = on" "SET max_parallel_workers_per_gather = 0" "SELECT paid FROM sift._test_scope_balance('ce_col')" \
	"$JOIN" "SELECT paid FROM sift._test_scope_balance('ce_col')" "$JOIN" "SELECT paid FROM sift._test_scope_balance('ce_col')" | awk -F'|' 'NF == 1 && $1 ~ /^[0-9]+$/')
read -r W0 W1 W2 <<<"$(tr '\n' ' ' <<<"$out")"
check "two reads of the warm index pay the same (the instructions of the arm)" test "$((W2 - W1))" -eq "$((W1 - W0))" -a "$((W1 - W0))" -gt 0
node_stop pb; node_start pb || bail "restart failed"
out=$(sql pb "SET sift.agg_force = on" "SET max_parallel_workers_per_gather = 0" "SELECT paid FROM sift._test_scope_balance('ce_col')" \
	"$JOIN" "SELECT paid FROM sift._test_scope_balance('ce_col')" "$JOIN" "SELECT paid FROM sift._test_scope_balance('ce_col')" | awk -F'|' 'NF == 1 && $1 ~ /^[0-9]+$/')
read -r C0 C1 C2 <<<"$(tr '\n' ' ' <<<"$out")"
check "the first read after a restart pays the blocks it took from the file on top: at least 10 blocks of 20,000" test "$((C1 - C0 - (W1 - W0)))" -ge 200000
check "the second one, warm again, pays the warm sum" test "$((C2 - C1))" -eq "$((W1 - W0))"
finish
