#!/usr/bin/env bash
# What the reads of a scope index paid for the pairs that changed since its floor is kept in the metapage of the index (col/v1/scope_paid.c): a restart, a crash and a backend that ends keep it, a server with
# more scope indexes than the table of the shared memory had slots counts each of them, and the rule says `stale scope` after a burst of reads that a restart split in two, none of the halves enough alone; no worker rewrites for it.
. "$(dirname "$0")/lib.bash"
plan 8

node_init pd 57 "autovacuum = off
sift.async_maintenance = on
sift.maintenance_naptime = 200
sift.test_scope_patch = off"
node_start pd || bail "start failed"
sql pd "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE cp (m int NOT NULL, t int NOT NULL)" "CREATE UNIQUE INDEX cp_u ON cp (m, t)" \
	"INSERT INTO cp SELECT m, 1 + (m * 7 + j * 5) % 16 FROM generate_series(1, 2000) m, generate_series(0, 2) j WHERE j < m % 4 ON CONFLICT DO NOTHING" \
	"CREATE INDEX cp_j ON cp USING sift_col (m, t) WITH (track_changes = 'ctid')" \
	"CREATE TABLE ce (id int PRIMARY KEY, m int, d timestamptz, v int, pad text)" \
	"INSERT INTO ce SELECT i, 1 + (i * 7919) % 2100, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, 'x' FROM generate_series(1, 200000) i" \
	>/dev/null || bail "setup failed"
sql pd "VACUUM (FREEZE, ANALYZE) ce, cp" >/dev/null || bail "vacuum failed"
sql pd "CREATE INDEX ce_col ON ce USING sift_col (id, m, d, v) WITH (key_order = 'm, d', scope = 't: m -> cp(m, t) max_multiplicity(8)')" >/dev/null || bail "build failed"

JOIN="SELECT count(*), sum(v) FROM ce e WHERE e.m IN (SELECT m FROM cp WHERE t = 6)"
reads() { sql pd "SET sift.agg_force = on" "SET max_parallel_workers_per_gather = 0" "DO \$\$ BEGIN FOR i IN 1..$1 LOOP PERFORM ${JOIN#SELECT }; END LOOP; END \$\$" >/dev/null; }
# reads, then the sum is written as the look does; the session ends, so that nothing but the metapage has it
reads_flush() { sql pd "SET sift.agg_force = on" "SET max_parallel_workers_per_gather = 0" "DO \$\$ BEGIN FOR i IN 1..$1 LOOP PERFORM ${JOIN#SELECT }; END LOOP; END \$\$" "SELECT sift._test_scope_paid_flush('ce_col')" >/dev/null; }
gens() { sql pd "SELECT string_agg(gen::text, ',' ORDER BY gen) FROM sift.col_segments('ce_col') WHERE kind = 'immutable'" | tail -n 1; }
due() { sql pd "SELECT coalesce(sift.col_maintenance_due('ce_col'), '-')" | tail -n 1; }
paid() { sql pd "SELECT paid FROM sift._test_scope_balance('ce_col')" | tail -n 1; }
half() { sql pd "SELECT half::int8 FROM sift._test_scope_balance('ce_col')" | tail -n 1; }
# the WAL writer has the record in the log within wal_writer_delay (200 ms): a crash after that keeps it
settle() { wait_for "wal_flushed pd" 30; }

G0=$(gens)
sql pd "WITH d AS (DELETE FROM cp WHERE abs(hashtext(m || '/' || t || '/b')) % 100 < 2 RETURNING m, t) INSERT INTO cp SELECT 1 + (m + 37) % 2000, t FROM d ON CONFLICT DO NOTHING" >/dev/null

reads_flush 20
P1=$(paid)
check "20 reads in a session that ended: their sum is in the metapage, seen by another session" test "$P1" -gt 0
HALF=$(half)
settle
node_stop pd; node_start pd || bail "restart failed"
check "a clean restart keeps the sum" test "$(paid)" -eq "$P1"
settle
crash pd; node_start pd || bail "restart after the crash failed"
check "a crash keeps the sum" test "$(paid)" -eq "$P1"

# a burst that a restart splits: a part of the reads of the rewrite's half before it, a part after, each under the half and together over it
PER=$((P1 / 20))
N=$((HALF * 6 / 10 / PER))
check "the part of the burst that comes before and the one that comes after are each under half the price and together over it" test "$N" -gt 20 -a $((N * PER)) -lt "$HALF" -a $((2 * N * PER)) -gt "$HALF"
reads_flush "$N"
check "the first part is not enough: nothing is due and the segments are as they were" test "$(due)" == - -a "$(gens)" == "$G0"
settle
crash pd; node_start pd || bail "restart after the second crash failed"
reads "$N"
# the look weighs the rule once for each naptime in a backend: a burst that ended inside one is weighed by the next read
sleep 0.3
reads 1
check "the second part, after the crash, makes the rewrite due and starts none: stale scope, the segments as they were" test "$(due)" == "stale scope" -a "$(gens)" == "$G0"
sql pd "SELECT sift.col_maintain('ce_col')" >/dev/null
check "the explicit rewrite writes the scope segments again, and what the reads paid is not owed to the new segments" test "$(gens)" != "$G0" -a "$(paid)" -eq 0

# more scope indexes than the 64 slots of the table of the shared memory: each one counts (a churn under the refresh threshold: the rewrite is not due by the pairs alone)
for i in $(seq 1 70); do
	sql pd "CREATE TABLE p$i (m int NOT NULL, t int NOT NULL)" "CREATE UNIQUE INDEX p${i}_u ON p$i (m, t)" \
		"INSERT INTO p$i SELECT m, 1 + m % 4 FROM generate_series(1, 200) m" \
		"CREATE INDEX p${i}_j ON p$i USING sift_col (m, t) WITH (track_changes = 'ctid')" \
		"CREATE TABLE e$i (id int PRIMARY KEY, m int, v int)" "INSERT INTO e$i SELECT i, 1 + i % 200, i % 10 FROM generate_series(1, 2000) i" \
		"VACUUM (FREEZE, ANALYZE) p$i, e$i" \
		"CREATE INDEX e${i}_col ON e$i USING sift_col (id, m, v) WITH (scope = 't: m -> p$i(m, t) max_multiplicity(8)')" \
		"DELETE FROM p$i WHERE m % 40 = 0" "INSERT INTO p$i SELECT m, 1 + (m + 1) % 4 FROM generate_series(1, 200) m WHERE m % 40 = 0" >/dev/null || bail "index $i failed"
done
sql pd "SET sift.test_scope_patch = on" "SET sift.agg_force = on" "SET max_parallel_workers_per_gather = 0" \
	"$(for i in $(seq 1 70); do echo "SELECT count(*) FROM e$i e WHERE e.m IN (SELECT m FROM p$i WHERE t = 1);"; done)" >/dev/null
sql pd "$(for i in $(seq 1 70); do echo "SELECT sift._test_scope_paid_flush('e${i}_col');"; done)" >/dev/null
out=$(sql pd "$(for i in $(seq 1 70); do echo "SELECT $i, sift._test_scope_paid('e${i}_col', 0);"; done)" | awk -F'|' '$2 == 0 {print $1}' | tr '\n' ' ')
check "70 scope indexes were read once each: every one has its sum" test -z "$out"
finish
