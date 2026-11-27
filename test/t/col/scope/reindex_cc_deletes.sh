#!/usr/bin/env bash
# tap: pooled
# REINDEX CONCURRENTLY of a scope index waits, after its validation pass, for the snapshots older than it, then swaps the copy in: until the swap the track_changes trigger serves the old
# index. A version a writer ends in that window must reach the copy too, or the summary of the copy counts the row as live until the next VACUUM. A REPEATABLE READ snapshot held from before
# the REINDEX keeps it in that window while facts are deleted and updated; once it is swapped in, every tenant reads from the summary as the join does.
# The index is of Format 2.0 on both runs: a Format 1 index has no scope read of its own.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 4
node_init rd 58 "autovacuum = off
sift.test_create_format1 = off"
node_start rd || bail "start failed"
D=$(dir rd)
sql rd "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id))" \
	"INSERT INTO tenant_scope SELECT m, 1 + (m * 31 + j * 13) % 12 FROM generate_series(1, 300) m, generate_series(0, 2) j ON CONFLICT DO NOTHING" \
	"CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int)" \
	"INSERT INTO events SELECT i, 1 + i % 300, timestamptz '2026-01-01' + i * interval '1 minute', i % 10 FROM generate_series(1, 9000) i" \
	"VACUUM (FREEZE, ANALYZE) events, tenant_scope" \
	"CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id) max_multiplicity(8)')" \
	>/dev/null || bail "setup failed"
READ() { echo "SELECT count(*), sum(v) FROM events e JOIN tenant_scope p ON p.dim_a_id = e.dim_a_id WHERE p.tenant_id = $1"; }
verdict() { sql rd "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) $(READ 5)" | sed -n 's/^ *Scope Read: //p' | grep . || echo "no scope read"; }
every_tenant() {
	local t
	for t in $(seq 1 12); do
		[[ $(sql rd "$(READ "$t")") == "$(sql rd "SET sift.enable_agg = off" "$(READ "$t")")" ]] || { diag "tenant $t: $(sql rd "$(READ "$t")") join $(sql rd "SET sift.enable_agg = off" "$(READ "$t")")"; return 1; }
	done
}

mkfifo "$D/s.in"
psql -X -At -q -h "$(node_sock rd)" -p "$(port rd)" -U postgres -d "$(node_db rd)" <"$D/s.in" >"$D/s.out" 2>&1 &
exec {FDS}>"$D/s.in"
echo "BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 'snap', count(*) FROM tenant_scope;" >&$FDS
wait_for "grep -q '^snap' '$D/s.out'" 30 || bail "the snapshot holder did not start"
sql rd "REINDEX INDEX CONCURRENTLY events_col" >"$D/reindex.out" 2>&1 &
REINDEX=$!
waiting() { sql rd "SELECT count(*) FROM pg_stat_activity WHERE query LIKE 'REINDEX INDEX CONCURRENTLY%' AND wait_event_type = 'Lock' AND pid <> pg_backend_pid()" | grep -qx 1; }
ready_copy() { sql rd "SELECT count(*) FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid WHERE c.relname = 'events_col_ccnew' AND i.indisready" | grep -qx 1; }
wait_for "ready_copy && waiting" 60 || bail "the REINDEX did not wait with its copy ready"
sql rd "DELETE FROM events WHERE id % 7 = 0" "UPDATE events SET dim_a_id = 1 + (dim_a_id + 1) % 300 WHERE id % 11 = 0" >/dev/null || bail "the writers failed"
echo "COMMIT;" >&$FDS
wait "$REINDEX"
check "the REINDEX ends once the old snapshot does" test $? == 0
exec {FDS}>&-
check "the read is from the summary of the copy ($(verdict))" test "$(verdict)" == summary
check "every tenant reads as the join does, the rows ended while the copy waited taken out" every_tenant
check "the index verifies" test -z "$(sql rd "SELECT sift.col_verify('events_col', true)" 2>&1 | grep -v '^$')"
finish
