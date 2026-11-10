#!/usr/bin/env bash
# pg_restore -j builds the indexes of one table at the same time: two sift_col indexes that write their automatic choices into their options both hold ShareLock on the table, and
# the write asked for ShareUpdateExclusiveLock on it, which each waited for the other's ShareLock to give up (a deadlock in five restores of six). Six parallel restores, no error.
. "$(dirname "$0")/lib.bash"

plan 1
node_init r 56 "autovacuum = off"
node_start r || bail "start failed"
D=$(dir r)
S=("-h" "$D/sock" "-p" "$(port r)" "-U" "postgres")
export PGOPTIONS="-c client_min_messages=warning"
dbq() { local db=$1; shift; psql -X -At -q -v ON_ERROR_STOP=1 "${S[@]}" -d "$db" "${@/#/--command=}"; }
dbq postgres "CREATE EXTENSION pg_sift" "CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, kind int, qty int, status text, created_at timestamptz)" \
	"INSERT INTO events SELECT i, i % 40, i % 5, i % 97, (ARRAY['new', 'open', 'held'])[1 + i % 3], timestamptz '2026-01-01' + i * interval '1 minute' FROM generate_series(1, 6000) i" \
	"CREATE INDEX events_a ON events USING sift_col (dim_a_id, created_at, kind, qty) WITH (track_changes = off, key_order = 'dim_a_id, created_at')" \
	"CREATE INDEX events_b ON events USING sift_col (dim_a_id, kind, status) WITH (track_changes = off, key_order = 'kind')" >/dev/null || bail "setup failed"
pg_dump "${S[@]}" -Fc -f "$D/src.custom" postgres
restores() {
	local n errors=0
	for n in 1 2 3 4 5 6; do
		dbq postgres "DROP DATABASE IF EXISTS r$n WITH (FORCE)" "CREATE DATABASE r$n" >/dev/null
		errors=$((errors + $(pg_restore "${S[@]}" -j8 -d "r$n" "$D/src.custom" 2>&1 | grep -c 'ERROR\|pg_restore: error')))
	done
	echo "$errors"
}
check "six pg_restore -j8 of a table with two sift_col indexes: no error" test "$(restores)" == 0
finish
