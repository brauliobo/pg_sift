#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The journal that CREATE INDEX CONCURRENTLY makes for a relation of pairs that has none is made empty and filled by the validation pass (validate_index adds the pairs the journal does not
# have). Those pairs were written before the journal began, by transactions that finished before the lock that began it, so a read whose snapshot sees the transaction that made the journal does not
# need a record of them (col_scope_journal_standby holds the read that does not): the pass writes the pairs into the index and no journal record. Every pair written after the journal began is recorded by its own writer, whatever the
# pass is doing meanwhile: this holds the pass at its gate (sift.test_phase_gate + 6, after the journal is ready for writes and before it reads), changes the pairs and the facts, and checks that
# the log holds exactly the records of those changes, that the sidecar the merge step writes after the pass (its stamp is taken after it) sees them, and that the read of the scope is the join's.
. "$(dirname "$0")/lib.bash"
. "$(dirname "$0")/lib_side.bash"

plan 15
node_init jv 30 "autovacuum = off
sift.col_auto_merge = off
$SIDE_CONF"
node_start jv || bail "start failed"
D=$(dir jv)
side_setup jv nojournal || bail "setup failed"
side_facts jv 1 24000
sql jv "VACUUM (FREEZE, ANALYZE) events, tenant_scope" >/dev/null || bail "vacuum failed"
PAIRS=$(sql jv "SELECT count(*) FROM tenant_scope")
CIC="CREATE INDEX CONCURRENTLY events_col ON events USING sift_col (id, dim_a_id, created_at, kind, v) WITH ($SIDE_WITH)"
READ=$(side_read 1) JOIN=$(side_join 1)
journal() { sql jv "SELECT i.indexrelid::regclass FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_am a ON a.oid = c.relam WHERE i.indrelid = 'tenant_scope'::regclass AND a.amname = 'sift_col'"; }
records() { sql jv "SELECT records FROM sift._test_conn_gen('$(journal)'::regclass)"; }
stale_runs() { sql jv "SELECT count(*) FROM sift._test_side_valid('events_col', 0, 1) WHERE NOT valid" | tail -n 1; }

GATE=7300
rm -f "$D/held"
{ echo "SELECT pg_advisory_lock($((GATE + 6)));"; echo "\\! touch '$D/held'"; echo "SELECT pg_sleep(600);"; } | sql jv >/dev/null 2>&1 &
HOLDER=$!
until [[ -e $D/held ]]; do sleep 0.05; done
sql jv "SET sift.test_phase_gate = $GATE" "$CIC" >"$D/build.out" 2>&1 &
BUILDER=$!
validating() { sql jv "SELECT count(*) FROM pg_stat_activity WHERE query LIKE 'CREATE INDEX CONCURRENTLY events_col%' AND wait_event_type = 'Lock'" | grep -qx 1; }
wait_for validating 60 || bail "the build did not reach the validation of the journal"
check "the journal is made by the statement, ready for writes and not valid while the pass waits" \
	test "$(sql jv "SELECT count(*) FROM pg_index i WHERE i.indrelid = 'tenant_scope'::regclass AND i.indisready AND NOT i.indisvalid")" == 1
# 3 pairs inserted, 2 deleted, 1 moved to another tenant (a record for the version it ended and one for the version it made): 7 journal records, and the 3 rows the log holds as ended
sql jv "INSERT INTO tenant_scope VALUES (1, 13), (2, 13), (3, 13)" \
	"DELETE FROM tenant_scope WHERE (dim_a_id, tenant_id) IN (SELECT dim_a_id, min(tenant_id) FROM tenant_scope WHERE dim_a_id IN (10, 11) GROUP BY 1)" \
	"UPDATE tenant_scope SET tenant_id = 14 WHERE (dim_a_id, tenant_id) IN (SELECT dim_a_id, min(tenant_id) FROM tenant_scope WHERE dim_a_id = 20 GROUP BY 1)" \
	"INSERT INTO events SELECT 24000 + i, 1 + (i * 31) % 60, timestamptz '2026-02-01', i % 7, i % 10 FROM generate_series(1, 500) i" >/dev/null
sql jv "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE query LIKE '%pg_sleep(600)%' AND pid <> pg_backend_pid()" >/dev/null
kill "$HOLDER" 2>/dev/null
wait "$BUILDER"; built=$?
check "the build ends beside the writers" test "$built" == 0
check "the log holds the records of the writers and none of the $PAIRS pairs the pass added" test "$(records)" == 8
check "the index is valid and the journal is valid" test "$(sql jv "SELECT count(*) FROM pg_index WHERE NOT indisvalid")" == 0
check "the read is the join's, with the pairs the writers changed" side_same jv "$READ" "$JOIN"
check "the index verifies" side_clean jv events_col
sql jv "VACUUM events" "SELECT sift.col_merge('events_col')" >/dev/null
check "the merge step writes the sidecar after the pass: the runs are taken ($(side_arm jv "$READ"))" test "$(side_runs jv "$READ")" -gt 0
check "the stamp of the sidecar is of a snapshot that sees what the writers did: no run is stale" test "$(stale_runs)" == 0
check "... and the read is the join's" side_same jv "$READ" "$JOIN"

# nothing written during the pass: the journal is empty when the statement ends, and records the next pair
sql jv "DROP INDEX events_col" "VACUUM tenant_scope" >/dev/null
check "a build with no writers leaves the journal with no record" test "$(sql jv "$CIC" >/dev/null; records)" == 0
sql jv "INSERT INTO tenant_scope VALUES (5, 1)" >/dev/null
check "... and the first pair written after it is its first record" test "$(records)" == 1
check "... and the read is the join's" side_same jv "$READ" "$JOIN"

# writers of the pairs and the facts run while builds that make their own journal run one after the other (each DROP INDEX takes the journal with it): every tenant reads as the join does
SECS=$(knob SIFT_JV_SECS 8 4)
sql jv "CREATE SEQUENCE ev_seq START 100000" >/dev/null
cat >"$D/pairs.sql" <<'SQL'
\set m random(1, 60)
\set t random(1, 14)
BEGIN;
INSERT INTO tenant_scope VALUES (:m, :t) ON CONFLICT DO NOTHING;
DELETE FROM tenant_scope WHERE dim_a_id = :m + 1 AND tenant_id = :t;
DELETE FROM tenant_scope WHERE dim_a_id = :m + 2 AND tenant_id = :t;
INSERT INTO tenant_scope VALUES (:m + 2, 1 + :t % 14) ON CONFLICT DO NOTHING;
COMMIT;
SQL
cat >"$D/facts.sql" <<'SQL'
\set m random(1, 60)
\set v random(0, 9)
INSERT INTO events VALUES (nextval('ev_seq'), :m, timestamptz '2026-03-01', :v % 7, :v);
UPDATE events SET dim_a_id = 1 + :m % 60 WHERE id = 1 + :m * 7;
SQL
pgbench_bg jv 4 "$((SECS + 2))" -f "$D/pairs.sql"@3 -f "$D/facts.sql"@2
ROUNDS=0 FAILED=0
end=$((SECONDS + SECS))
while ((SECONDS < end)); do
	if sql jv "DROP INDEX IF EXISTS events_col" "$CIC" >"$D/op.out" 2>&1; then ROUNDS=$((ROUNDS + 1)); else FAILED=$((FAILED + 1)); diag "$(head -n 2 "$D/op.out")"; fi
done
wait "$PGBENCH_PID"; PRC=$?
check "the writers had no failed transaction and $ROUNDS builds ran" test "$PRC" == 0 -a "$FAILED" == 0 -a "$ROUNDS" -ge 1
every_tenant() {
	local t
	for t in $(seq 1 14); do
		side_same jv "$(side_read "$t")" "$(side_join "$t")" || { diag "tenant $t differs"; return 1; }
	done
}
check "every tenant reads as the join does after the writers" every_tenant
check "no invalid index is left and the index verifies" test "$(sql jv "SELECT count(*) FROM pg_index WHERE NOT indisvalid")" == 0 -a -z "$(sql jv "SELECT sift.col_verify('events_col', true)" 2>&1)"
finish
