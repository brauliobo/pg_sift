#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The first scope index of a relation makes the journal of its relation of pairs before it reads anything and stamps its segments with it, so the first build is filled: its reads are
# the units, not the entity rows of the members ("unfilled"). The writers of the relation of pairs wait for the statement (the journal is made under a lock that holds them out), and a pair
# written the moment the build ends is in the journal: the stamp of the build vouches for every change after the rows it read, whatever the build waited for. A concurrent build starts the journal at the end
# of its build as before: its segments carry no stamp until the next REINDEX, and its reads are exact meanwhile.
. "$(dirname "$0")/lib.bash"

plan 7
node_init fs 62 "sift.scope_max_multiplicity = 8
autovacuum = off"
node_start fs || bail "start failed"
D=$(dir fs)
sql fs "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id))" \
	"INSERT INTO tenant_scope SELECT m, 1 + (m * 31 + j * 13) % 12 FROM generate_series(1, 400) m, generate_series(0, 3) j ON CONFLICT DO NOTHING" \
	"CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid')" \
	"CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int)" \
	"INSERT INTO events SELECT i, 1 + i % 400, timestamptz '2026-01-01' + i * interval '1 minute', i % 10 FROM generate_series(1, 20000) i" \
	"VACUUM (FREEZE, ANALYZE) events, tenant_scope" >/dev/null || bail "setup failed"
SCOPE="scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)'"
READ="SELECT count(*), sum(v) FROM events e WHERE sift.in_scope('events_col'::regclass, 'tenant_id', e.dim_a_id, 12)"
PLAIN="SELECT count(*), sum(v) FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 12)"
fallback() { # the verdict of the scope read: its fallback, "units" when it read the units
	local p
	p=$(sql fs "SET sift.agg_force = on" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) $READ")
	grep -o 'Scope Fallback: .*' <<<"$p" || { grep -q 'Scope Units' <<<"$p" && echo units || echo "no scope read"; }
}
same() { [[ $(sql fs "SET sift.agg_force = on" "$READ") == "$(sql fs "SET sift.enable_agg = off" "$PLAIN")" ]]; }
changed() { sql fs "SELECT count(c.key) FROM (SELECT s.lineage, s.horizon::text::xid AS horizon FROM sift._test_col_scope_stamps('events_col') s
	WHERE s.family = 'scope' AND s.scope = 0 ORDER BY s.horizon LIMIT 1) t, LATERAL sift._test_scope_journal_changes('events_col', 0, t.lineage, t.horizon) c"; }

# the build waits after its scan (sift.test_phase_gate + the write phase) for an advisory lock this session holds; a writer of the pairs starts meanwhile
GATE=7100
rm -f "$D/held"
{ echo "SELECT pg_advisory_lock($(( GATE + 3 )));"; echo "\\! touch '$D/held'"; echo "SELECT pg_sleep(600);"; } | sql fs >/dev/null 2>&1 &
HOLDER=$!
until [[ -e $D/held ]]; do sleep 0.05; done
sql fs "SET sift.test_phase_gate = $GATE" "CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH ($SCOPE)" >"$D/build.out" 2>&1 &
BUILDER=$!
building() { sql fs "SELECT count(*) FROM pg_stat_activity WHERE query LIKE 'CREATE INDEX events_col%' AND wait_event_type = 'Lock'" | grep -qx 1; }
wait_for building 60 || bail "the build did not reach its write phase"
sql fs "INSERT INTO tenant_scope VALUES (1, 12)" >"$D/writer.out" 2>&1 &
WRITER=$!
waiting() { sql fs "SELECT count(*) FROM pg_stat_activity WHERE query LIKE 'INSERT INTO tenant_scope%' AND wait_event_type = 'Lock'" | grep -qx 1; }
wait_for waiting 60
check "a writer of the relation of pairs waits for the build" waiting
sql fs "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE query LIKE '%pg_sleep(600)%' AND pid <> pg_backend_pid()" >/dev/null
kill "$HOLDER" 2>/dev/null
wait "$BUILDER"; built=$?
wait "$WRITER"; wrote=$?
check "the build ends and the writer follows it" test "$built" == 0 -a "$wrote" == 0
check "the first build is filled: the read is the units, no fallback ($(fallback))" test "$(fallback)" == units
check "the pair written after the rows the build read is a changed member of the stamp" test "$(changed)" == 1
check "the read patches it and is the join" same
check "the index verifies" test -z "$(sql fs "SELECT sift.col_verify('events_col', true)" 2>&1)"

# a concurrent build stamps nothing at the end of its build, and reads exactly meanwhile
sql fs "DROP INDEX events_col" "DELETE FROM tenant_scope WHERE dim_a_id = 1 AND tenant_id = 12" "DROP INDEX tenant_scope_journal" \
	"CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid')" \
	"CREATE INDEX CONCURRENTLY events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH ($SCOPE)" >/dev/null
unfilled_exact() { [[ $(fallback) == "Scope Fallback: unfilled" ]] && same; }
check "a concurrent build: the read is the join, unfilled until a REINDEX ($(fallback))" unfilled_exact
finish
