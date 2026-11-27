#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# A VACUUM of the heap of a sift_col index whose partition is hot (its slab a directory of shards of heap references) settles the references of the versions it removes before the
# heap frees their slots. Single-row UPDATEs replace versions whose references wait in the directory; a VACUUM is held at its first flush of the slab (sift.test_phase_gate, gate
# SIFT_F2_GATE_SLAB_FLUSH = 15) while 300 rows are inserted as a part (or wait for the partition, where the VACUUM holds it), then released. The flush must come before the heap frees the slots of the old versions: a reference left
# behind names the new tuple of its slot, and the row is then in the index twice, in the part and through the reference.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 7
SECS=$(knob SIFT_S107_SECS 3 2)
G=9100
node_init hv 1 "autovacuum = off
shared_preload_libraries = 'pg_sift'
sift.test_create_format1 = off
sift.async_maintenance = off
max_connections = 40"
node_start hv || bail "start failed"
D=$(dir hv)
sql hv "CREATE EXTENSION pg_sift" \
	"CREATE FUNCTION l0_hot_dirs(regclass) RETURNS int4 LANGUAGE C STRICT AS 'pg_sift', 'sift_test_f2_l0_hot_dirs'" \
	"CREATE TABLE hc (id bigserial, v int) WITH (autovacuum_enabled = false)" \
	"CREATE INDEX hc_idx ON hc USING sift_col (id, v)" >/dev/null || bail "setup failed"
[[ $(sql hv "SELECT count(*) FROM pg_settings WHERE name = 'sift.test_phase_gate'") == 1 ]] || { skip 7 "the release library has no gate"; finish; exit; }

echo "INSERT INTO hc (v) VALUES (1);" >"$D/ins.sql"
pgbench_bg hv 16 "$SECS" -f "$D/ins.sql"
wait "$PGBENCH_PID"
check "16 writers of one partition leave it hot" test "$(sql hv "SELECT l0_hot_dirs('hc_idx')")" -ge 1
sql hv "CREATE TABLE freed AS SELECT ctid::text AS slot, id FROM hc WHERE id > (SELECT max(id) - 20 FROM hc)" >/dev/null
for id in $(sql hv "SELECT id FROM freed"); do sql hv "UPDATE hc SET v = 2 WHERE id = $id" >/dev/null; done
check "single-row UPDATEs leave the old references in the directory" test "$(sql hv "SELECT l0_hot_dirs('hc_idx')")" -ge 1

waiting() { sql hv "SELECT count(*) FROM pg_stat_activity WHERE query LIKE '$1%' AND wait_event_type = 'Lock' AND wait_event = '$2'"; }
sess_open hv gate
sess_say gate "SELECT pg_advisory_lock($((G + 15))); \\echo held"
wait_for "grep -q '^held\$' $D/gate.out" 30 || bail "the gate was not taken"
(sql hv "SET sift.test_phase_gate = $G" "VACUUM hc" >"$D/vacuum.out" 2>&1; echo $? >"$D/vacuum.rc") &
check "the VACUUM waits at its first flush of the slab" wait_for "[[ \$(waiting VACUUM advisory) == 1 ]]" 60
(sql hv "INSERT INTO hc (v) SELECT 3 FROM generate_series(1, 300)" >/dev/null; echo $? >"$D/insert.rc") &
wait_for "[[ -f '$D/insert.rc' || \$(waiting INSERT page) == 1 ]]" 60 || bail "the INSERT neither ended nor waited for the partition"
sess_say gate "SELECT pg_advisory_unlock($((G + 15)));"
wait_for "[[ -f '$D/vacuum.rc' && -f '$D/insert.rc' ]]" 120 || bail "the VACUUM or the INSERT did not finish"
sess_close gate
check "the VACUUM and the INSERT succeeded" test "$(cat "$D/vacuum.rc") $(cat "$D/insert.rc")" == "0 0"
diag "inserted rows in the slots of the replaced versions: $(sql hv "SELECT count(*) FROM hc WHERE v = 3 AND ctid::text IN (SELECT slot FROM freed)")"

# counts: the rows the column scan counts, the addresses an index scan returns more than once, and the rows of the heap
counts() {
	sql hv "SET sift.enable_agg = on" "SELECT count(*) FROM hc" | tail -1
	sql hv "SET enable_seqscan = off" "SET enable_bitmapscan = off" "SET sift.enable_agg = off" \
		"SELECT count(*) FROM (SELECT ctid FROM hc WHERE id > 0 GROUP BY ctid HAVING count(*) > 1) d" | tail -1
	sql hv "SET enable_indexscan = off" "SET enable_bitmapscan = off" "SET sift.enable_agg = off" "SELECT count(*) FROM hc" | tail -1
}
heap=$(counts | tail -1)
out=$(counts | tr '\n' ' ')
check "the column scan counts the heap's rows and no address is twice in the index" test "$out" == "$heap 0 $heap "
sql hv "SELECT count(*) FROM sift.maintain('hc_idx')" >/dev/null
out=$(counts | tr '\n' ' ')
check "and so once the slab is written out as a part" test "$out" == "$heap 0 $heap "
check "sift.check finds nothing in the index" test "$(sql hv "SELECT count(*) FROM sift.check('hc_idx'::regclass, true)")" == 0
finish
