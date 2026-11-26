#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The commit of an UPDATE that stages column deltas encodes the run of a partition, then takes the lock of the partition. A writer that adds a part to the partition in between moves its manifest
# generation, but every part the run names is still there: the run is appended as it was encoded, not encoded again (under sustained writers that re-encoding was a retry storm). The commit is
# held after the encoding (sift.test_phase_gate, gate SIFT_F2_GATE_COL_DELTA_RUN = 16) while the other writer commits.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

G=7700
plan 6
node_init rr 102 "autovacuum = off
sift.maintenance_naptime = 60000
sift.async_maintenance = off
sift.test_create_format1 = off"
node_start rr || bail "start failed"
D=$(dir rr)
sql rr "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE cd (id bigint, a bigint, b text) USING sift_table WITH (autovacuum_enabled = false)" \
	"INSERT INTO cd SELECT g, g, repeat('b', 40) FROM generate_series(1, 6000) g" >/dev/null || bail "setup failed"
parts() { sql rr "SELECT count(*) FROM sift.parts('cd')"; }
waiting() { sql rr "SELECT count(*) FROM pg_stat_activity WHERE application_name = 'run_writer' AND wait_event_type = 'Lock' AND wait_event = 'advisory'"; }
before=$(parts)

rm -f "$D/H.in"; mkfifo "$D/H.in"
psql -X -At -q -h "$D/sock" -p "$(port rr)" -U postgres -d postgres <"$D/H.in" >"$D/H.out" 2>&1 &
exec {H}>"$D/H.in"
echo "SELECT pg_advisory_lock($((G + 16)));" >&"$H"
echo '\echo @@held' >&"$H"
wait_for "grep -q '@@held' '$D/H.out'" 30 || bail "the gate was not taken"
sql rr "SET application_name = 'run_writer'" "SET sift.test_phase_gate = $G" "BEGIN" "UPDATE cd SET b = b || 'x' WHERE id IN (10, 2500, 5000)" "COMMIT" \
	"SELECT 'reencodes=' || sift._test_col_delta_reencodes()" >"$D/writer.out" 2>&1 &
WRITER=$!
check "the commit waits with its run encoded" wait_for "[[ \$(waiting) == 1 ]]" 60
sql rr "INSERT INTO cd SELECT g, g, repeat('n', 40) FROM generate_series(6001, 12000) g" >/dev/null
check "a part of another writer joined the partition meanwhile" test "$(parts)" -gt "$before"
echo "SELECT pg_advisory_unlock($((G + 16)));" >&"$H"
wait "$WRITER"
exec {H}>&-
wait
check "the run was appended as it was encoded" grep -qx 'reencodes=0' "$D/writer.out"
check "the three updated rows are in the table" test "$(sql rr "SELECT count(*) FROM cd WHERE right(b, 1) = 'x'")" == 3
check "no other row changed" test "$(sql rr "SELECT count(*) FILTER (WHERE b = repeat('b', 40)) || '/' || count(*) FROM cd")" == "5997/12000"
check "sift.check finds nothing" test "$(sql rr "SELECT count(*) FROM sift.check('cd'::regclass)")" == 0
finish
