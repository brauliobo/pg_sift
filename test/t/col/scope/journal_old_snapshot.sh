#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The validation pass of a concurrent scoped build writes no journal record for the pairs it adds, which were written before the journal began. A stamp is not what makes that safe: a prepared
# transaction (and an idle READ COMMITTED one) is not waited for, so the xmin of every later snapshot, the stamp's too, stays at or below its xid, and a hot standby waits for nobody.
# What makes it safe is that a read reads the log only when its snapshot sees the transaction that made the journal as committed (sift_col_track_ok_log), which every writer of those pairs
# finished before. A REPEATABLE READ snapshot on the standby taken before those writers committed does not see it: its read takes the join, and its rows are the join's in that snapshot.
# The index is of Format 2.0 on both runs: a Format 1 index has no scope read of its own.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 5
node_init pm 32 "wal_level = replica
max_wal_senders = 4
max_prepared_transactions = 8
autovacuum = off
sift.test_create_format1 = off"
node_start pm || bail "primary start failed"
sql pm "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id))" \
	"INSERT INTO tenant_scope SELECT m, 1 + (m * 31 + j * 13) % 12 FROM generate_series(1, 300) m, generate_series(0, 2) j ON CONFLICT DO NOTHING" \
	"CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int)" \
	"INSERT INTO events SELECT i, 1 + i % 300, timestamptz '2026-01-01' + i * interval '1 minute', i % 10 FROM generate_series(1, 9000) i" \
	"CREATE TABLE other (x int)" \
	"VACUUM (FREEZE, ANALYZE) events, tenant_scope" >/dev/null || bail "setup failed"
node_clone pm sb 33 "hot_standby_feedback = on
wal_receiver_status_interval = 1s"
node_start sb || bail "standby start failed"
D=$(dir pm)
SCOPE="scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id) max_multiplicity(8)'"
READ="SELECT count(*), sum(v) FROM events e JOIN tenant_scope p ON p.dim_a_id = e.dim_a_id WHERE p.tenant_id = 12"

# a prepared transaction with an old xid, a standby reader whose snapshot (xmin = that xid) predates the writers of the pairs
sql pm "BEGIN" "INSERT INTO other VALUES (1)" "PREPARE TRANSACTION 'old'" >/dev/null || bail "prepare failed"
catchup pm sb || bail "the standby did not catch up"
mkfifo "$D/r.in"
psql -X -At -q -h "$(dir sb)/sock" -p "$(port sb)" -U postgres -d postgres <"$D/r.in" >"$D/r.out" 2>&1 &
exec {FDR}>"$D/r.in"
echo "BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 'snap', pg_current_snapshot();" >&$FDR
wait_for "grep -q '^snap' '$D/r.out'" 30 || bail "the reader did not start"

# pairs inserted and deleted and committed after that snapshot and before the build
sql pm "INSERT INTO tenant_scope VALUES (1, 12), (2, 12), (3, 12) ON CONFLICT DO NOTHING" \
	"DELETE FROM tenant_scope WHERE (dim_a_id, tenant_id) IN (SELECT dim_a_id, min(tenant_id) FROM tenant_scope WHERE dim_a_id IN (10, 11, 12) GROUP BY 1)" >/dev/null
sql pm "SET statement_timeout = 120000" "CREATE INDEX CONCURRENTLY events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH ($SCOPE)" >"$D/build.out" 2>&1
check "the scoped build completes with a prepared transaction holding an old xid" test $? == 0
catchup pm sb

ask() { echo "$1" >&$FDR; echo "\\echo end-$2" >&$FDR; wait_for "grep -q '^end-$2\$' '$D/r.out'" 30; }
: >"$D/r.out"
ask "SELECT 'scope', count(*), sum(v) FROM events e JOIN tenant_scope p ON p.dim_a_id = e.dim_a_id WHERE p.tenant_id = 12;" 1
ask "SET sift.enable_agg = off; SELECT 'join', count(*), sum(v) FROM events e JOIN tenant_scope p ON p.dim_a_id = e.dim_a_id WHERE p.tenant_id = 12;" 2
ask "SET sift.enable_agg = on; EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) $READ;" 3
tr -d '\000' <"$D/r.out" >"$D/r.txt"
S=$(grep -a '^scope' "$D/r.txt")
J=$(grep -a '^join' "$D/r.txt")
FALLBACK=$(grep -a -o 'Scope Read: .*' "$D/r.txt" | head -1)
diag "the old snapshot: scope $S, join $J, plan: ${FALLBACK:-no scope read}"
check "the old snapshot of the standby reads the join's rows through the scope index" test -n "$S" -a "${S#scope|}" == "${J#join|}"
check "... because it does not read the log: it takes the join, as its snapshot does not see the transaction that made the journal" test "$FALLBACK" == "Scope Read: join (the summary is not exact for the snapshot)"
echo "\\q" >&$FDR
exec {FDR}>&-
check "a new snapshot on the standby reads the join's rows" test "$(sql sb "$READ")" == "$(sql sb "SET sift.enable_agg = off" "$READ")"
check "the primary reads the join's rows" test "$(sql pm "$READ")" == "$(sql pm "SET sift.enable_agg = off" "$READ")"
sql pm "COMMIT PREPARED 'old'" >/dev/null
finish
