#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# Concurrency of the manifest, the slabs and the vblock leases of a sift_table. (1) A commit that frees blocks holds the slot of its index in the registry of shared memory before Block 0 changes: when the
# publication that follows fails, a reader is served nothing and reads Block 0, never the manifest older than the commit, whose blocks the commit freed; 16 writers, 4 readers and maintenance
# on one table quarantine nothing and lose no row. (2) Pooled sessions that stay connected after they inserted into more tables than the registry has lease slots hold no lease: the registry
# counts no run without a slot and VACUUM sets the bits of the tables they left. (3) A SELECT does not write the micro-slabs of a table out. (4) A partition that a merge dropped is found again
# by its lower bound under the change lock, not reported as missing.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

TABLES=120
POOLED=20
WRITERS=16
READERS=4
out=
plan 9
node_init ms 4 "autovacuum = off
shared_preload_libraries = 'pg_sift'
sift.async_maintenance = off
sift.shared_cache_kb = 16384
max_connections = 80"
node_start ms || bail "start failed"
sql ms "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "setup failed"

served() { sql ms "SELECT sift._test_shm_manifest_served('$1'::regclass)"; }

sql ms "CREATE TABLE t1 (v int)" "CREATE INDEX t1_idx ON t1 USING sift_store_test (v)" "SELECT sift._test_f2_init('t1_idx')" "SELECT sift._test_f2_add_part('t1_idx', 1, NULL, 100, 0, 1) > 0" \
	"SELECT sift._test_f2_split_many('t1_idx', 1, NULL, 20, 1000)" >/dev/null || bail "index failed"
tap_tx_hold ms snap "SELECT parts FROM sift._test_f2_plan_snapshot('t1_idx')"
check "a reader is served the manifest of an index of 21 partitions, as a snapshot" test "$(served t1_idx) $(sql ms "SELECT sift._test_shm_manifest_stat('snaps_live') > 0")" == "t t"
out=$(sql ms "SET sift.test_dsm_fail = 'manifest_drop_snap'" "SELECT sift._test_f2_add_part('t1_idx', 2, 1000, 10, 1000, 100000) > 0" 2>&1) || true
check "a commit whose publication fails is a commit the registry does not serve" test "$(served t1_idx) $(grep -c manifest_drop_snap <<<"$out")" == "f 1"
out=$(sql ms "SELECT (SELECT parts FROM sift._test_f2_plan_snapshot('t1_idx')) || ' ' || (SELECT sum(nparts) FROM sift._test_f2_partitions('t1_idx'))" 2>&1)
check "and the reader that comes after it reads the parts the commit made" test "$out" == "2 2"
tap_tx_release snap

sql ms "CREATE TABLE c1s (k int, v int) USING sift_table" >/dev/null
out=
for w in $(seq $WRITERS); do
	sql ms "DO \$\$ BEGIN FOR i IN 1..40 LOOP INSERT INTO c1s SELECT g, $w FROM generate_series(1, 5) g; COMMIT; END LOOP; END \$\$" >"$(dir ms)/w$w.out" 2>&1 &
done
for r in $(seq $READERS); do
	sql ms "DO \$\$ BEGIN FOR i IN 1..80 LOOP PERFORM count(*) FROM c1s; COMMIT; END LOOP; END \$\$" >"$(dir ms)/r$r.out" 2>&1 &
done
sql ms "DO \$\$ BEGIN FOR i IN 1..20 LOOP PERFORM * FROM sift.maintain('c1s'); COMMIT; END LOOP; END \$\$" >"$(dir ms)/m.out" 2>&1 &
wait
out=$(cat "$(dir ms)"/{w,r}*.out "$(dir ms)/m.out")
check "writers, readers and maintenance of one table end with no error" test -z "$out"
out=$(sql ms "SELECT (SELECT count(*) FROM c1s) || ' ' || (SELECT quarantined FROM sift.status('c1s'))" 2>&1)
check "the table has every row and is not quarantined" test "$out" == "$((WRITERS * 200)) false"

out=$(sql ms "DO \$\$ BEGIN FOR i IN 1..$TABLES LOOP EXECUTE format('CREATE TABLE lt_%s (k int, v int) USING sift_table', i); COMMIT; END LOOP; END \$\$" 2>&1)
check "created $TABLES sift_tables" test -z "$out"
for b in $(seq $POOLED); do
	tap_tx_hold ms "p$b" "DO \$\$ BEGIN FOR i IN 1..$TABLES LOOP EXECUTE format('INSERT INTO lt_%s VALUES (%s, 1)', i, $b); COMMIT; END LOOP; END \$\$"
done
sql ms "CREATE FUNCTION lease_floor(regclass, int8) RETURNS int8 AS 'pg_sift', 'sift_test_vm_lease_floor' LANGUAGE C STRICT" >/dev/null
out=$(sql ms "SELECT lease_floor('lt_1', 1000000) || ' ' || lease_floor('lt_$TABLES', 1000000)" "VACUUM lt_1, lt_$TABLES" 2>&1)
check "$POOLED sessions that stay connected hold no lease on the tables they wrote and left: VACUUM may close every vblock" test "$out" == "1000000 1000000"

sql ms "CREATE TABLE c3 (k int, v int) USING sift_table" "INSERT INTO c3 SELECT g, g FROM generate_series(1, 10) g" >/dev/null
before=$(sql ms "SELECT l0_slabs FROM sift.status('c3')")
sql ms "SELECT count(*) FROM c3" "ANALYZE c3" >/dev/null
out="before $before after $(sql ms "SELECT l0_slabs FROM sift.status('c3')")"
check "a SELECT and an ANALYZE leave the micro-slabs where they are" test "$out" == "before 1 after 1"

sql ms "CREATE TABLE t4 (v int)" "CREATE INDEX t4_idx ON t4 USING sift_store_test (v)" "SELECT sift._test_f2_init('t4_idx')" \
	"SELECT sift._test_f2_add_part('t4_idx', 1, NULL, 100, 0, 1) > 0" "SELECT sift._test_f2_split('t4_idx', 1, NULL, 1000)" "SELECT sift._test_f2_merge('t4_idx', 1, NULL, 2, 1000)" >/dev/null
out=$(sql ms "SELECT sift._test_f2_slab_flush('t4_idx', 2, 1000)" 2>&1)
check "the change lock of a partition that a merge dropped is taken on the partition that holds its lower bound" test "$out" == f
finish
