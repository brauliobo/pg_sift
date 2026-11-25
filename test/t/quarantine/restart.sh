#!/usr/bin/env bash
# A sift_col index that meets damage leaves service and the metapage is flagged; after a restart the flag alone keeps the index out of service (sift.quarantine lists it, statements run on the
# heap), and REINDEX puts it back. Format 1 (a): an autovacuum worker meets the damage, fails once, and the VACUUMs after it skip the index. Format 2 (b): a read meets it and fails once
# (a VACUUM of Format 2 does not skip an index that is out of service yet, so autovacuum is off for b).
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 14
node_init p 72 "autovacuum = on
autovacuum_naptime = 1
sift.on_corruption = quarantine"
node_start p || bail "start failed"
declare -A FORMAT1=([a]=on [b]=off) PAGE=([a]=2 [b]=3)   # the page of the index that a damage goes to: Format 1 keeps its directory in block 2, Format 2 its first part in block 3
sql p "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "setup failed"
for t in a b; do
	sql p "SET sift.test_create_format1 = ${FORMAT1[$t]}" \
		"CREATE TABLE $t (id int, v int) WITH (autovacuum_vacuum_threshold = 10, autovacuum_vacuum_scale_factor = 0, autovacuum_analyze_threshold = 100000000)" \
		"INSERT INTO $t SELECT i, i % 7 FROM generate_series(1, 20000) i" "CREATE INDEX ${t}_idx ON $t USING sift_col (id, v)" "ANALYZE $t" >/dev/null || bail "setup of $t failed"
done
sql p "ALTER TABLE b SET (autovacuum_enabled = false)" >/dev/null

uses_sift() { sql p "EXPLAIN (COSTS OFF) SELECT count(*) FROM $1 WHERE v = 3" | grep -qE "Sift|$1_idx"; }
state() { [[ $(sql p "SELECT persisted || ' ' || (skipped_vacuums > 0) FROM sift.quarantine WHERE index = '$1_idx'::regclass") == "$2" ]]; }
poke() { sql p "SELECT sift._test_poke('$1_idx', ${PAGE[$1]}, 8176, 9, 2)" "CHECKPOINT" >/dev/null || bail "damage of $1 failed"; }
heap_plan() { ! uses_sift "$1"; }
for t in a b; do check "$t: the index is planned before the damage" uses_sift $t; done

poke a
sql p "DELETE FROM a WHERE id <= 3000" >/dev/null
check "a: autovacuum skips the damaged index, and the worker it asked for flags the metapage" wait_for "state a 'true true'" 90
check "a: the autovacuum run that met the damage failed once and none after it" test "$(grep -c 'ERROR:  sift index "a_idx"' "$(dir p)/postgres.log")" == 1

poke b
out=$(sql p "SELECT count(*) FROM b WHERE v = 3" 2>&1) || true
check "b: the statement that meets the damage fails once" grep -q 'XX002\|corrupted' <<<"$out"
flagged() { sql p "SELECT count(*) FROM b WHERE v = 3" >/dev/null && state b 'true false'; }
check "b: the statements after it run on the heap, and the worker they asked for flags the metapage" wait_for flagged 60

node_stop p
node_start p || bail "restart failed"
for t in a b; do
	want=$(sql p "SELECT count(*) FROM $t WHERE v = 3")
	check "$t: after a restart the flag lists the index" test "$(sql p "SELECT persisted FROM sift.quarantine WHERE index = '${t}_idx'::regclass")" == t
	check "$t: after a restart the statements are planned on the heap" heap_plan $t
	sql p "SET sift.test_create_format1 = ${FORMAT1[$t]}" "REINDEX INDEX ${t}_idx" >/dev/null
	check "$t: REINDEX takes the index out of the list and the rows are the heap's" test "$(sql p "SELECT count(*) FROM sift.quarantine WHERE index = '${t}_idx'::regclass")" == 0 -a "$(sql p "SELECT count(*) FROM $t WHERE v = 3")" == "$want"
	sql p "ANALYZE $t" >/dev/null
	check "$t: the rebuilt index is planned again" uses_sift $t
done
finish
