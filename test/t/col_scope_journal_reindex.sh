#!/usr/bin/env bash
# A REINDEX of the journal index of the pairs is another lineage: the stamp of the scope sidecars is of the old one, so no run vouches for its members and the scope reads go on by reference, with the answer
# of the join, exactly. No read or worker starts a rewrite for it; the merge step the user runs (sift.col_merge) writes the sidecars again with no REINDEX of the scope index; the answers are the join's
# before, during and after.
. "$(dirname "$0")/lib.bash"
. "$(dirname "$0")/lib_side.bash"
plan 8

node_init jr 53 "autovacuum = off
sift.col_auto_merge = off
$SIDE_CONF"
node_start jr || bail "start failed"
side_setup jr || bail "setup failed"
side_facts jr 1 24000
sql jr "VACUUM (FREEZE, ANALYZE) events, tenant_scope" >/dev/null || bail "vacuum failed"
sql jr "CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, v) WITH ($SIDE_WITH)" >/dev/null || bail "build failed"
READ=$(side_read 1) JOIN=$(side_join 1)

check "the first build writes the sidecars: the read takes their runs" test "$(side_format jr events_col)" == 53 -a "$(side_runs jr "$READ")" -gt 0
sql jr "REINDEX INDEX tenant_scope_journal" >/dev/null
check "after a REINDEX of the journal no run is taken" test "$(side_runs jr "$READ")" == 0
check "... and the answer is the join's" side_same jr "$READ" "$JOIN"
for _ in $(seq 30); do side_runs jr "$READ" >/dev/null; done
check "the reads start no rewrite: after 30 reads no run is taken yet" test "$(side_runs jr "$READ")" == 0
sql jr "SELECT sift.col_merge('events_col')" >/dev/null
check "the merge step writes the sidecars again, with no REINDEX of the scope index: the runs are taken" test "$(side_runs jr "$READ")" -gt 0
check "... with the same answer as the join" side_same jr "$READ" "$JOIN"
sql jr "INSERT INTO tenant_scope SELECT m, 1 FROM generate_series(5, 59, 6) m" >/dev/null
check "a pair written after it is read by reference, and the answer is the join's" side_same jr "$READ" "$JOIN"
check "... and no key differs from the join, the index verifies" side_clean jr events_col
finish
