#!/usr/bin/env bash
# tap: pooled
# What a scope read that found changed members is counted to have paid (col/scope.h sift_scope_ran_price, agg/scope.c read_extra) follows what its entries cost, which depends on the aggregate: a count
# of the largest tenant takes 29 instructions an entry (and a sum 33), a sum and a count by 7 values 62, on 300,000 entries; the price of the entry was 67 for both, so a count was counted twice over and the
# rewrite it asks for came after a quarter of the reads that pay it back. Checked as the ratio of the two, which does not depend on the speed of the host: measured, a count costs 0.53 of the group by (11.3 and 21.2
# million instructions over the filled read); counted, it must be between 0.4 and 0.75 (0.91 before). The same for a read that took the join (the tenant of a tenth of the facts at 5%: 3.1 and 5.5 million over the filled
# read, 0.56; the join was priced the same for both, 1.0 before).
. "${SIFT_REPO:?}/test/t/lib/common.bash"
plan 6

node_init pp 60 "autovacuum = off
sift.async_maintenance = off
sift.test_scope_patch = off"
node_start pp || bail "start failed"
sql pp "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" "SET client_min_messages = warning" \
	"CREATE TABLE ts (m int NOT NULL, t int NOT NULL, PRIMARY KEY (m, t))" \
	"INSERT INTO ts SELECT m, 1 FROM generate_series(1, 3000) m" \
	"INSERT INTO ts SELECT m, 3 + m % 10 FROM generate_series(1, 3000) m" \
	"CREATE INDEX ts_journal ON ts USING sift_col (m, t) WITH (track_changes = 'ctid')" \
	"CREATE TABLE events (id int PRIMARY KEY, m int, d date, kind text, amount int)" \
	"INSERT INTO events SELECT i, 1 + (i::bigint * 7919) % 3000, date '2025-01-01' + i % 365, 'k' || i % 7, i % 100 FROM generate_series(1, 300000) i" \
	"VACUUM (FREEZE, ANALYZE) events, ts" \
	"CREATE INDEX events_col ON events USING sift_col (id, m, d, kind, amount) WITH (scope = 't: m -> ts(m, t) max_multiplicity(8)')" \
	"VACUUM (FREEZE, ANALYZE) events, ts" >/dev/null || bail "setup failed"

COUNT="SELECT count(*) FROM events e WHERE e.m IN (SELECT m FROM ts WHERE t = 1)"
GROUP="SELECT e.kind, sum(e.amount), count(*) FROM events e WHERE e.m IN (SELECT m FROM ts WHERE t = 1) GROUP BY 1"
paid() { sql pp "SELECT sift._test_scope_paid('events_col', 0)" | tail -n 1; }
# the sum of one read of the statement in a session of its own: the difference of the metapage's and this session's sums
read_paid() {
	local before after
	before=$(paid)
	sql pp "SET sift.agg_force = on" "SET sift.agg_scope = on" "SET max_parallel_workers_per_gather = 0" "$1" "SELECT sift._test_scope_paid_flush('events_col')" >/dev/null
	after=$(paid)
	echo $((after - before))
}
sql pp "WITH d AS (DELETE FROM ts WHERE abs(hashtext(m || '/' || t || '/a')) % 10000 < 30 RETURNING m, t) INSERT INTO ts SELECT 1 + abs(hashtext(m || '/' || t || '/g')) % 3000, t FROM d ON CONFLICT DO NOTHING" >/dev/null || bail "the churn failed"
pick() { sql pp "SET sift.agg_force = on" "SET max_parallel_workers_per_gather = 0" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) $1" | grep -c 'Scope Entries Read'; }
check "the read of the largest tenant at 0.3% of the pairs changed decodes the entries of its units" test "$(pick "$COUNT")" -ge 1
C=$(read_paid "$COUNT")
G=$(read_paid "$GROUP")
check "both reads were counted" test "$C" -gt 0 -a "$G" -gt 0
out="count $C, group by $G"
check "a count was counted at 0.4 to 0.75 of the group by (what its entries cost, not the same price for every aggregate)" test $((C * 100)) -ge $((G * 40)) -a $((C * 100)) -le $((G * 75))

sql pp "WITH d AS (DELETE FROM ts WHERE abs(hashtext(m || '/' || t || '/b')) % 10000 < 500 RETURNING m, t) INSERT INTO ts SELECT 1 + abs(hashtext(m || '/' || t || '/h')) % 3000, t FROM d ON CONFLICT DO NOTHING" >/dev/null || bail "the second churn failed"
COUNT="SELECT count(*) FROM events e WHERE e.m IN (SELECT m FROM ts WHERE t = 5)"
GROUP="SELECT e.kind, sum(e.amount), count(*) FROM events e WHERE e.m IN (SELECT m FROM ts WHERE t = 5) GROUP BY 1"
joined() { sql pp "SET sift.agg_force = on" "SET max_parallel_workers_per_gather = 0" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) $1" | grep -c 'Scope Fallback'; }
check "the reads of a tenant of a tenth of the facts at 5% of the pairs changed take the join" test "$(joined "$COUNT")" -ge 1 -a "$(joined "$GROUP")" -ge 1
C=$(read_paid "$COUNT")
G=$(read_paid "$GROUP")
check "both join reads were counted" test "$C" -gt 0 -a "$G" -gt 0
out="count $C, group by $G"
check "a count that took the join was counted at 0.4 to 0.75 of the group by" test $((C * 100)) -ge $((G * 40)) -a $((C * 100)) -le $((G * 75))
finish
