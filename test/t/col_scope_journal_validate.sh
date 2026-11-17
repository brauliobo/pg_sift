#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The journal that CREATE INDEX CONCURRENTLY makes for a relation of pairs that has none is made empty and filled by the validation pass (validate_index adds the pairs the journal does not
# have). Those pairs were written before the journal began, by transactions that finished before the lock that began it, so a read whose snapshot sees the transaction that made the journal does not
# need a record of them (col_scope_journal_standby holds the read that does not): the pass writes the pairs into the index and no journal record. Every pair written after the journal began is recorded by its own writer, whatever the
# pass is doing meanwhile: this holds the pass at its gate (sift.test_phase_gate + 6, after the journal is ready for writes and before it reads), changes the pairs and the facts, and checks that
# the log holds exactly the records of those changes, that the stamp (taken after the pass) sees them, and that the read of the scope is the join's.
. "$(dirname "$0")/lib.bash"

plan 15
node_init jv 30 "sift.scope_max_multiplicity = 8
autovacuum = off"
node_start jv || bail "start failed"
D=$(dir jv)
sql jv "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id))" \
	"INSERT INTO tenant_scope SELECT m, 1 + (m * 31 + j * 13) % 12 FROM generate_series(1, 400) m, generate_series(0, 3) j ON CONFLICT DO NOTHING" \
	"CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int)" \
	"INSERT INTO events SELECT i, 1 + i % 400, timestamptz '2026-01-01' + i * interval '1 minute', i % 10 FROM generate_series(1, 20000) i" \
	"VACUUM (FREEZE, ANALYZE) events, tenant_scope" >/dev/null || bail "setup failed"
PAIRS=$(sql jv "SELECT count(*) FROM tenant_scope")
SCOPE="scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)'"
READ="SELECT count(*), sum(v) FROM events e WHERE sift.in_scope('events_col'::regclass, 'tenant_id', e.dim_a_id, 12)"
PLAIN="SELECT count(*), sum(v) FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 12)"
journal() { sql jv "SELECT i.indexrelid::regclass FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_am a ON a.oid = c.relam WHERE i.indrelid = 'tenant_scope'::regclass AND a.amname = 'sift_col'"; }
records() { sql jv "SELECT records FROM sift._test_conn_gen('$(journal)'::regclass)"; }
verdict() { # the verdict of the scope read: its fallback, "units" when it read the units
	local p
	p=$(sql jv "SET sift.agg_force = on" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) $READ")
	grep -o 'Scope Fallback: .*' <<<"$p" || { grep -q 'Scope Units' <<<"$p" && echo units || echo "no scope read"; }
}
same() { [[ $(sql jv "SET sift.agg_force = on" "$READ") == "$(sql jv "SET sift.enable_agg = off" "$PLAIN")" ]]; }
changed() { sql jv "SELECT count(c.key) FROM (SELECT s.lineage, s.horizon::text::xid AS horizon FROM sift._test_col_scope_stamps('events_col') s
	WHERE s.family = 'scope' AND s.scope = 0 ORDER BY s.horizon LIMIT 1) t, LATERAL sift._test_scope_journal_changes('events_col', 0, t.lineage, t.horizon) c"; }

GATE=7300
rm -f "$D/held"
{ echo "SELECT pg_advisory_lock($((GATE + 6)));"; echo "\\! touch '$D/held'"; echo "SELECT pg_sleep(600);"; } | sql jv >/dev/null 2>&1 &
HOLDER=$!
until [[ -e $D/held ]]; do sleep 0.05; done
sql jv "SET sift.test_phase_gate = $GATE" "CREATE INDEX CONCURRENTLY events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH ($SCOPE)" >"$D/build.out" 2>&1 &
BUILDER=$!
validating() { sql jv "SELECT count(*) FROM pg_stat_activity WHERE query LIKE 'CREATE INDEX CONCURRENTLY events_col%' AND wait_event_type = 'Lock'" | grep -qx 1; }
wait_for validating 60 || bail "the build did not reach the validation of the journal"
check "the journal is made by the statement, ready for writes and not valid while the pass waits" \
	test "$(sql jv "SELECT count(*) FROM pg_index i WHERE i.indrelid = 'tenant_scope'::regclass AND i.indisready AND NOT i.indisvalid")" == 1
# 3 pairs inserted, 2 deleted, 1 moved to another tenant (a record for the version it ended and one for the version it made): 7 journal records, and the 3 rows the log holds as ended
sql jv "INSERT INTO tenant_scope VALUES (1, 13), (2, 13), (3, 13)" \
	"DELETE FROM tenant_scope WHERE (dim_a_id, tenant_id) IN (SELECT dim_a_id, min(tenant_id) FROM tenant_scope WHERE dim_a_id IN (10, 11) GROUP BY 1)" \
	"UPDATE tenant_scope SET tenant_id = 14 WHERE (dim_a_id, tenant_id) IN (SELECT dim_a_id, min(tenant_id) FROM tenant_scope WHERE dim_a_id = 20 GROUP BY 1)" \
	"INSERT INTO events SELECT 20000 + i, 1 + (i * 31) % 400, timestamptz '2026-02-01', i % 10 FROM generate_series(1, 500) i" >/dev/null
sql jv "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE query LIKE '%pg_sleep(600)%' AND pid <> pg_backend_pid()" >/dev/null
kill "$HOLDER" 2>/dev/null
wait "$BUILDER"; built=$?
check "the build ends beside the writers" test "$built" == 0
check "the log holds the records of the writers and none of the $PAIRS pairs the pass added" test "$(records)" == 10
check "the index is valid and the journal is valid" test "$(sql jv "SELECT count(*) FROM pg_index WHERE NOT indisvalid")" == 0
check "the read is from the units ($(verdict))" test "$(verdict)" == units
check "... and is the join's, with the pairs the writers changed" same
check "the stamp is of a snapshot that sees what the writers did: no member changed since it" test "$(changed)" == 0
check "the index verifies" test -z "$(sql jv "SELECT sift.col_verify('events_col', true)" 2>&1)"
check "... and no key differs from the join" test "$(sql jv "SELECT count(*) FROM sift.col_scope_verify('events_col', 1000) WHERE problem <> 'unverified'")" == 0

# nothing written during the pass: the journal is empty when the statement ends, and records the next pair
sql jv "DROP INDEX events_col" "VACUUM tenant_scope" >/dev/null
check "a build with no writers leaves the journal with no record" test "$(sql jv "CREATE INDEX CONCURRENTLY events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH ($SCOPE)" >/dev/null; records)" == 0
sql jv "INSERT INTO tenant_scope SELECT min(m), 12 FROM generate_series(1, 400) m WHERE NOT EXISTS (SELECT 1 FROM tenant_scope t WHERE t.dim_a_id = m AND t.tenant_id = 12)" >/dev/null
check "... and the first pair written after it is its first record and the one changed member" test "$(records)|$(changed)" == "1|1"
check "... and the read patches it and is the join's" same

# writers of the pairs and the facts run while builds that make their own journal run one after the other (each DROP INDEX takes the journal with it): every tenant reads as the join does
SECS=$(knob SIFT_JV_SECS 8 4)
sql jv "CREATE SEQUENCE ev_seq START 100000" >/dev/null
cat >"$D/pairs.sql" <<'SQL'
\set m random(1, 400)
\set t random(1, 14)
BEGIN;
INSERT INTO tenant_scope VALUES (:m, :t) ON CONFLICT DO NOTHING;
DELETE FROM tenant_scope WHERE dim_a_id = :m + 1 AND tenant_id = :t;
DELETE FROM tenant_scope WHERE dim_a_id = :m + 2 AND tenant_id = :t;
INSERT INTO tenant_scope VALUES (:m + 2, 1 + :t % 14) ON CONFLICT DO NOTHING;
COMMIT;
SQL
cat >"$D/facts.sql" <<'SQL'
\set m random(1, 400)
\set v random(0, 9)
INSERT INTO events VALUES (nextval('ev_seq'), :m, timestamptz '2026-03-01', :v);
UPDATE events SET dim_a_id = 1 + :m % 400 WHERE id = 1 + :m * 7;
SQL
pgbench_bg jv 4 "$((SECS + 2))" -f "$D/pairs.sql"@3 -f "$D/facts.sql"@2
ROUNDS=0 FAILED=0
end=$((SECONDS + SECS))
while ((SECONDS < end)); do
	if sql jv "DROP INDEX IF EXISTS events_col" "CREATE INDEX CONCURRENTLY events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH ($SCOPE)" >"$D/op.out" 2>&1; then ROUNDS=$((ROUNDS + 1)); else FAILED=$((FAILED + 1)); diag "$(head -n 2 "$D/op.out")"; fi
done
wait "$PGBENCH_PID"; PRC=$?
check "the writers had no failed transaction and $ROUNDS builds ran" test "$PRC" == 0 -a "$FAILED" == 0 -a "$ROUNDS" -ge 1
every_tenant() {
	local t
	for t in $(seq 1 14); do
		[[ $(sql jv "SET sift.agg_force = on" "SELECT count(*), sum(v) FROM events e WHERE sift.in_scope('events_col'::regclass, 'tenant_id', e.dim_a_id, $t)") == \
			"$(sql jv "SET sift.enable_agg = off" "SELECT count(*), sum(v) FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = $t)")" ]] || { diag "tenant $t differs"; return 1; }
	done
}
check "every tenant reads as the join does after the writers ($(verdict))" every_tenant
check "no invalid index is left and the index verifies" test "$(sql jv "SELECT count(*) FROM pg_index WHERE NOT indisvalid")" == 0 -a -z "$(sql jv "SELECT sift.col_verify('events_col', true)" 2>&1)"
finish
