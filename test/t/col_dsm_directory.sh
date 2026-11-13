#!/usr/bin/env bash
# The directory of the shared tables (col/dsmreg.c) is a segment of fixed layout that every library of any age reads, and has 1024 entries. A directory whose magic is not the one
# of this library is not read: the join raises a clear error and a backend that asks for a cache goes without the table (sift_dsm_attach), a statement never fails for it. A full
# directory (1024 tables that backends map) is the same: a cache goes without its table, the rows of a write buffer are written with their values where the references have no
# arena, and the tables are there again once the backends that mapped them are gone.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "$(dirname "$0")/lib.bash"

plan 10
# a dynamic shared memory segment for each table: the control segment has 64 + 5 slots for each backend the server can have
node_init dd 80 "max_connections = 300
autovacuum = off"
node_start dd || bail "start failed"
sql dd "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "extension failed"
log() { grep -c "$1" "$(dir dd)/postgres.log" || true; }

# a directory with another magic
GOOD=$(sql dd "SELECT sift._test_dsm_magic(1)")
out=$(sql dd "SELECT sift._test_dsm_join('mg_probe', 4096)" 2>&1)
check "a directory that is not of this library is not joined: a clear error" grep -q 'the directory of the shared tables is not one this library can read' <<<"$out"
out=$(sql dd "SELECT sift._test_dsm_attach('mg_probe', 4096)" 2>&1)
check "and a cache goes without its table, with no error" test "$out" == f
sql dd "SELECT sift._test_dsm_magic($GOOD)" >/dev/null
out=$(sql dd "SELECT sift._test_dsm_attach('mg_probe', 4096)")
check "the table is had when the directory is the one of this library" test "$out" == t

# a full directory: one session maps tables until the directory has no entry for the next, and holds them while the others ask
sql dd <<'SQL' >/dev/null || bail "setup failed"
CREATE TABLE ev (id int PRIMARY KEY, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, 1 + i % 50, i % 11 FROM generate_series(1, 5000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, k, v) WITH (track_changes = 'ctid');
VACUUM (FREEZE, ANALYZE) ev;
SQL
held=$(mktemp)
sql dd "DO \$\$ DECLARE n int := 0; BEGIN
	FOR i IN 1..2000 LOOP
		BEGIN PERFORM sift._test_dsm_join('full_' || i, 8192); n := i;
		EXCEPTION WHEN configuration_limit_exceeded THEN EXIT; END;
	END LOOP;
	RAISE WARNING 'holding %', n; END \$\$" "SELECT pg_sleep(8)" >"$held" 2>&1 &
holder=$!
wait_for "grep -q holding '$held'" 20 || bail "the holder did not fill the directory"
out=$(sql dd "SELECT sift._test_dsm_join('full_extra', 8192)" 2>&1)
check "a full directory: the join says so" grep -q 'the directory of the shared tables is full' <<<"$out"
out=$(sql dd "SELECT sift._test_dsm_attach('full_extra', 8192)" 2>&1)
check "a cache goes without its table, with no error" test "$out" == f
check "and says why in the log" test "$(log 'the test tables are not shared with other backends: sift: the directory of the shared tables is full')" -ge 1
out=$(sql dd "INSERT INTO ev SELECT 5000 + i, 1 + i % 50, i % 11 FROM generate_series(1, 500) i" "SELECT count(*) FROM ev" 2>&1)
check "a write buffer takes its rows, with their values where the references have no arena" test "$out" == 5500
check "and the log says the references are not shared" test "$(log 'the references of the write buffers are not shared with other backends: the directory of the shared tables is full')" -ge 1
out=$(sql dd "SELECT count(*) FROM ev WHERE k = 7" "SELECT sift.col_verify('ev_col', true) IS NOT NULL" 2>&1 | tr '\n' ' ')
check "the reads answer and the index verifies" test "$out" == "110 t "
wait $holder
rm -f "$held"
out=$(sql dd "SELECT sift._test_dsm_attach('full_extra', 8192)")
check "the tables come back when the backends that held the others are gone" test "$out" == t
finish
