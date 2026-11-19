#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The validation pass of a concurrent scoped build writes no journal record for the pairs it adds, which were written before the journal began. A stamp is not what makes that safe: a prepared
# transaction (and an idle READ COMMITTED one) is not waited for, so the xmin of every later snapshot, the stamp's too, stays at or below its xid, and a hot standby waits for nobody.
# What makes it safe is that a read reads the log only when its snapshot sees the transaction that made the journal as committed (sift_col_track_ok_log), which every writer of those pairs
# finished before. A REPEATABLE READ snapshot on the standby taken before those writers committed does not see it: its read takes the heap, and its rows are the join's in that snapshot.
. "$(dirname "$0")/lib.bash"
. "$(dirname "$0")/lib_side.bash"

plan 5
node_init pm 32 "wal_level = replica
max_wal_senders = 4
max_prepared_transactions = 8
sift.col_auto_merge = off
autovacuum = off
$SIDE_CONF"
node_start pm || bail "primary start failed"
side_setup pm nojournal && sql pm "CREATE TABLE other (x int)" >/dev/null || bail "setup failed"
side_facts pm 1 24000
sql pm "VACUUM (FREEZE, ANALYZE) events, tenant_scope" >/dev/null || bail "vacuum failed"
node_clone pm sb 33 "hot_standby_feedback = on
wal_receiver_status_interval = 1s"
node_start sb || bail "standby start failed"
D=$(dir pm)
READ=$(side_read 1) JOIN=$(side_join 1) SCOPE_TAG="'scope'" JOIN_TAG="'join'"

# a prepared transaction with an old xid, a standby reader whose snapshot (xmin = that xid) predates the writers of the pairs
sql pm "BEGIN" "INSERT INTO other VALUES (1)" "PREPARE TRANSACTION 'old'" >/dev/null || bail "prepare failed"
catchup pm sb || bail "the standby did not catch up"
mkfifo "$D/r.in"
psql -X -At -q -h "$(dir sb)/sock" -p "$(port sb)" -U postgres -d postgres <"$D/r.in" >"$D/r.out" 2>&1 &
exec {FDR}>"$D/r.in"
echo "BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 'snap', pg_current_snapshot();" >&$FDR
wait_for "grep -q '^snap' '$D/r.out'" 30 || bail "the reader did not start"

# pairs inserted and deleted and committed after that snapshot and before the build
sql pm "INSERT INTO tenant_scope VALUES (5, 1), (11, 1), (17, 1) ON CONFLICT DO NOTHING" \
	"DELETE FROM tenant_scope WHERE (dim_a_id, tenant_id) IN (SELECT dim_a_id, min(tenant_id) FROM tenant_scope WHERE dim_a_id IN (10, 12, 13) GROUP BY 1)" >/dev/null
sql pm "SET statement_timeout = 120000" "CREATE INDEX CONCURRENTLY events_col ON events USING sift_col (id, dim_a_id, created_at, kind, v) WITH ($SIDE_WITH)" >"$D/build.out" 2>&1
check "the scoped build completes with a prepared transaction holding an old xid" test $? == 0
sql pm "VACUUM events" "SELECT sift.col_merge('events_col')" >/dev/null
catchup pm sb

ask() { echo "$1" >&$FDR; echo "\\echo end-$2" >&$FDR; wait_for "grep -q '^end-$2\$' '$D/r.out'" 30; }
: >"$D/r.out"
ask "SET sift.agg_force = on; SET sift.test_scope_entity = on; ${READ/SELECT kind/SELECT $SCOPE_TAG, kind};" 1
ask "SET sift.enable_agg = off; ${JOIN/SELECT kind/SELECT $JOIN_TAG, kind};" 2
ask "SET sift.enable_agg = on; EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) $READ;" 3
tr -d '\000' <"$D/r.out" >"$D/r.txt"
S=$(grep -a '^scope|' "$D/r.txt")
J=$(grep -a '^join|' "$D/r.txt")
RUNS=$(grep -a -o 'Scope Sidecar Runs: .*' "$D/r.txt" | head -1)
diag "the old snapshot: scope $S, join $J, plan: ${RUNS:-no runs}"
check "the old snapshot of the standby reads the join's rows through the scope index" test -n "$S" -a "${S//scope|/}" == "${J//join|/}"
check "... and takes no run: it does not read the log, as its snapshot does not see the transaction that made the journal" test "$RUNS" == "Scope Sidecar Runs: 0"
echo "\\q" >&$FDR
exec {FDR}>&-
check "a new snapshot on the standby reads the join's rows" side_same sb "$READ" "$JOIN"
check "the primary reads the join's rows" side_same pm "$READ" "$JOIN"
sql pm "COMMIT PREPARED 'old'" >/dev/null
finish
