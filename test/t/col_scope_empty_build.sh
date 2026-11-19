#!/usr/bin/env bash
# A scope index built on an empty table has no segment and no sidecar: its reads are the join's before and after rows arrive (the rows are the write buffers', which no run summarises), and the first
# promotion of its rows writes entity segments with their sidecars, which the reads then take; the answers are the join's throughout.
. "$(dirname "$0")/lib.bash"
. "$(dirname "$0")/lib_side.bash"
plan 7

node_init eb 55 "autovacuum = off
sift.col_auto_merge = off
$SIDE_CONF"
node_start eb || bail "start failed"
side_setup eb || bail "setup failed"
sql eb "CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, v) WITH ($SIDE_WITH)" >/dev/null || bail "build failed"

READ=$(side_read 1) JOIN=$(side_join 1)

check "on an empty table the read is the join's" side_same eb "$READ" "$JOIN"
side_facts eb 1 24000
check "... and once rows arrive, from the write buffers" side_same eb "$READ" "$JOIN"
check "... the entity read has no run to take" test "$(side_runs eb "$READ")" == 0
sql eb "VACUUM events" "SELECT sift.col_promote('events_col')" >/dev/null
check "the first promotion of its rows writes entity segments with a sidecar" test "$(side_format eb events_col)" == 53
check "... the read takes its runs" test "$(side_runs eb "$READ")" -gt 0
check "... and is the join's" side_same eb "$READ" "$JOIN"
check "... and the index verifies, no key differs from the join" side_clean eb events_col
finish
