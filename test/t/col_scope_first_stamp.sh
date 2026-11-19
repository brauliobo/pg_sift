#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The first scope index of a relation makes the journal of its relation of pairs before it reads anything and stamps its sidecars with it, so the first build is filled: its reads take the runs. The writers of
# the relation of pairs wait for the statement (the journal is made under a lock that holds them out), and a pair written the moment the build ends is in the journal: the stamp of the sidecar vouches for every
# change before it and the runs whose members the pair touches are read by reference. A concurrent build starts the journal at the end of its build as before and writes the sidecars in its validation pass
# (col_scope_cic_fill.sh), so its reads take the runs as soon as it ends.
. "$(dirname "$0")/lib.bash"
. "$(dirname "$0")/lib_side.bash"

plan 8
node_init fs 62 "autovacuum = off
sift.col_auto_merge = off
$SIDE_CONF"
node_start fs || bail "start failed"
D=$(dir fs)
side_setup fs || bail "setup failed"
side_facts fs 1 24000
sql fs "VACUUM (FREEZE, ANALYZE) events, tenant_scope" >/dev/null || bail "vacuum failed"
READ=$(side_read 1) JOIN=$(side_join 1)
stale_runs() { sql fs "SELECT count(*) FROM sift._test_side_valid('events_col', 0, 1) WHERE NOT valid" | tail -n 1; }

# the build waits after its scan (sift.test_phase_gate + the write phase) for an advisory lock this session holds; a writer of the pairs starts meanwhile
GATE=7100
rm -f "$D/held"
{ echo "SELECT pg_advisory_lock($(( GATE + 3 )));"; echo "\\! touch '$D/held'"; echo "SELECT pg_sleep(600);"; } | sql fs >/dev/null 2>&1 &
HOLDER=$!
until [[ -e $D/held ]]; do sleep 0.05; done
sql fs "SET sift.test_phase_gate = $GATE" "CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, v) WITH ($SIDE_WITH)" >"$D/build.out" 2>&1 &
BUILDER=$!
building() { sql fs "SELECT count(*) FROM pg_stat_activity WHERE query LIKE 'CREATE INDEX events_col%' AND wait_event_type = 'Lock'" | grep -qx 1; }
wait_for building 60 || bail "the build did not reach its write phase"
sql fs "INSERT INTO tenant_scope VALUES (5, 1)" >"$D/writer.out" 2>&1 &
WRITER=$!
waiting() { sql fs "SELECT count(*) FROM pg_stat_activity WHERE query LIKE 'INSERT INTO tenant_scope%' AND wait_event_type = 'Lock'" | grep -qx 1; }
wait_for waiting 60
check "a writer of the relation of pairs waits for the build" waiting
sql fs "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE query LIKE '%pg_sleep(600)%' AND pid <> pg_backend_pid()" >/dev/null
kill "$HOLDER" 2>/dev/null
wait "$BUILDER"; built=$?
wait "$WRITER"; wrote=$?
check "the build ends and the writer follows it" test "$built" == 0 -a "$wrote" == 0
check "the first build is filled: the read takes the runs of the sidecar" test "$(side_runs fs "$READ")" -gt 0
check "the pair written after the rows the build read makes the runs of its member stale" test "$(stale_runs)" -ge 1
check "the read takes the others and reads those by reference, and is the join" side_same fs "$READ" "$JOIN"
check "the index verifies" side_clean fs events_col

# a concurrent build writes the sidecars in its validation pass
sql fs "DROP INDEX events_col" "DELETE FROM tenant_scope WHERE dim_a_id = 5 AND tenant_id = 1" "DROP INDEX tenant_scope_journal" \
	"CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid')" \
	"CREATE INDEX CONCURRENTLY events_col ON events USING sift_col (id, dim_a_id, created_at, kind, v) WITH ($SIDE_WITH)" >/dev/null
check "a concurrent build: the read is the join's, with no REINDEX" side_same fs "$READ" "$JOIN"
check "... the sidecar is written and its runs are taken, with no merge step" test "$(side_runs fs "$READ")" -gt 0 -a "$(side_format fs events_col)" == 53
finish
