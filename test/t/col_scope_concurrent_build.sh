#!/usr/bin/env bash
# Two sessions that hold ShareLock on one table (what two CREATE INDEX statements, or a parallel pg_restore, hold while they build) each create a sift_col index with a scope on it. A build sets the
# options it decides (key_columns, key_cut, point_tid_columns, ...) with a catalog update of its index, which must not ask for a stronger lock on the table than the other session's ShareLock allows:
# the two builds were a deadlock (the restore of two indexes of one table in parallel ran into it). The indexes do not track changes: the triggers that tracking installs ask the table for ShareRowExclusiveLock,
# which two builds that hold ShareLock cannot both get (PostgreSQL takes that lock for CREATE TRIGGER). Both indexes are made, verify, and read as the join does.
. "$(dirname "$0")/lib.bash"

plan 6
node_init scb 52 "sift.scope_max_multiplicity = 8
autovacuum = off"
node_start scb || bail "start failed"
D=$(dir scb)
S=(-X -q -h "$D/sock" -p "$(port scb)" -U postgres -v ON_ERROR_STOP=1)
sql scb "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id))" \
	"INSERT INTO tenant_scope SELECT m, 1 + (m * 31 + j * 13) % 12 FROM generate_series(1, 400) m, generate_series(0, 3) j ON CONFLICT DO NOTHING" \
	"CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid')" \
	"CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int)" \
	"INSERT INTO events SELECT i, 1 + i % 400, timestamptz '2026-01-01' + i * interval '1 minute', i % 10 FROM generate_series(1, 20000) i" >/dev/null || bail "setup failed"

# the event trigger that takes the stronger lock at the start (below) cannot help a session that holds ShareLock before the statement: it is off for this part
sql scb "ALTER EVENT TRIGGER sift_col_build_lock DISABLE" >/dev/null
build() { # NAME: a session that takes ShareLock on the table, waits for the other one to take it, then builds
	psql "${S[@]}" -c "BEGIN" -c "LOCK TABLE events IN SHARE MODE" -c "SELECT pg_sleep(1.5)" \
		-c "CREATE INDEX $1 ON events USING sift_col (id, dim_a_id, created_at, v) WITH (track_changes = 'off', scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)')" \
		-c "COMMIT" -d postgres >"$D/$1.out" 2>&1
}
build events_a &
A=$!
build events_b &
B=$!
wait "$A"; RA=$?
wait "$B"; RB=$?
check "two concurrent builds on one table: no error, no deadlock" test "$RA" == 0 -a "$RB" == 0
diag "$(cat "$D/events_a.out" "$D/events_b.out" | head -6)"
check "both indexes exist" test "$(sql scb "SELECT count(*) FROM pg_class WHERE relname IN ('events_a', 'events_b')")" == 2
check "both verify" test -z "$(sql scb "SELECT sift.col_verify('events_a', true)" "SELECT sift.col_verify('events_b', true)" 2>&1)"
sql scb "REINDEX INDEX events_a" "SET sift.agg_force = on" >/dev/null
check "a read through the scope is the join" test "$(sql scb "SET sift.agg_force = on" "SELECT count(*), sum(v) FROM events e WHERE sift.in_scope('events_a'::regclass, 'tenant_id', e.dim_a_id, 5)")" == \
	"$(sql scb "SET sift.enable_agg = off" "SELECT count(*), sum(v) FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 5)")"

sql scb "ALTER EVENT TRIGGER sift_col_build_lock ENABLE" >/dev/null
# the same without the barrier, with the default change tracking: the log of each index installs two triggers on the table at the end of its build (CREATE TRIGGER takes ShareRowExclusiveLock),
# so two builds that run at the same time were each waiting for the other's ShareLock. The statement takes the lock first now (an event trigger before CREATE INDEX of a sift_col index).
race() { # NAME
	psql "${S[@]}" -d postgres -c "CREATE INDEX $1 ON events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)')" >"$D/$1.out" 2>&1
}
race events_c &
A=$!
race events_d &
B=$!
wait "$A"; RA=$?
wait "$B"; RB=$?
check "two concurrent builds that track changes: no error, no deadlock" test "$RA" == 0 -a "$RB" == 0
diag "$(cat "$D/events_c.out" "$D/events_d.out" | grep -i "error\|deadlock\|waits" | head -4)"
check "both verify, and the log of each is complete" test -z "$(sql scb "SELECT sift.col_verify('events_c', true)" "SELECT sift.col_verify('events_d', true)" 2>&1)" -a \
	"$(sql scb "SELECT count(*) FROM pg_trigger WHERE tgrelid = 'events'::regclass AND tgname LIKE 'sift%'")" -ge 4
finish
