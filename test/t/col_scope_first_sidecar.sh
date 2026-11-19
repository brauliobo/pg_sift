#!/usr/bin/env bash
# The first scope index of a relation of pairs that has no journal makes the journal in its own statement, and the sidecars of its entity segments are written at the end of that statement, under the
# journal it started: CREATE INDEX with a journal made beforehand, with a journal the statement makes, and CREATE INDEX CONCURRENTLY with one it makes, all leave segments with a sidecar (format 53) and
# reads that take the runs with the answer of the join, with no maintenance (the merge step is off).
. "$(dirname "$0")/lib.bash"
. "$(dirname "$0")/lib_side.bash"
plan 10

node_init fb 64 "autovacuum = off
sift.col_auto_merge = off
$SIDE_CONF"
node_start fb || bail "start failed"
READ=$(side_read 1) JOIN=$(side_join 1)
CREATE_SQL="CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, v) WITH ($SIDE_WITH)"

first_build() { # NAME JOURNAL(made|auto) STATEMENT
	local name=$1 journal=$2 stmt=$3
	sql fb "DROP TABLE IF EXISTS events, tenant_scope" >/dev/null
	side_setup fb "$([[ $journal == auto ]] && echo nojournal)" || bail "$name: setup failed"
	side_facts fb 1 24000
	sql fb "VACUUM (FREEZE, ANALYZE) events, tenant_scope" "$stmt" >/dev/null || bail "$name: build failed"
	check "$name: the entity segments have the sidecar (format 53) with no maintenance" test "$(side_format fb events_col)" == 53
	check "$name: the read takes the runs of the sidecar, with the join's answer" test "$(side_runs fb "$READ")" -gt 0 && side_same fb "$READ" "$JOIN"
	check "$name: the index verifies and no key differs from the join" side_clean fb events_col
}
first_build "CREATE INDEX, journal made beforehand" made "$CREATE_SQL"
first_build "CREATE INDEX, journal made by the statement" auto "$CREATE_SQL"
check "REINDEX of it keeps the sidecar" test "$(sql fb "REINDEX INDEX events_col" >/dev/null; side_format fb events_col)" == 53
first_build "CREATE INDEX CONCURRENTLY, journal made by the statement" auto "${CREATE_SQL/CREATE INDEX/CREATE INDEX CONCURRENTLY}"
finish
