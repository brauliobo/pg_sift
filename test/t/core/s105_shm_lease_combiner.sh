#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The shared-memory paths of the writers and readers when commits combine and statements fail. (1) Writers of the first four and of the last four partitions of an index of 400 partitions (18
# leaves of the partition directory) commit at once: the leader of the combiner that publishes their entries as one change of the directory writes every leaf it makes (the leaves it read
# from the first to the last, more than the few one commit makes), the server does not crash and the index checks. (2) A writer the combiner holds that is terminated leaves within a second and
# the others finish. (3) A statement that fails while it reads a shared dictionary in place gives its pin back. (4) A statement that fails while it puts the list of a partition in the area of
# the snapshots leaves the list to be made by the next reader and gives back the shared memory it took.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

PARTS=400 WRITERS=8 ROUNDS=150
out=
plan 10
node_init sl 2 "autovacuum = off
shared_preload_libraries = 'pg_sift'
sift.async_maintenance = off
sift.shared_cache_kb = 16384
shared_buffers = 32MB
max_connections = 40"
node_start sl || bail "start failed"
TESTING=$(sql sl "CREATE EXTENSION pg_sift" "SELECT count(*) FROM pg_settings WHERE name = 'sift.test_dsm_fail'" | tail -1)
[[ $TESTING == 1 ]] || bail "needs the testing library"
sql sl "CREATE EXTENSION pg_sift_test" "CREATE TABLE s (v int)" "CREATE INDEX s_idx ON s USING sift_store_test (v)" "SELECT sift._test_f2_init('s_idx')" \
	"SELECT sift._test_f2_add_part('s_idx', 1, NULL, 10, 0, 1) > 0" "SELECT sift._test_f2_split_many('s_idx', 1, NULL, $((PARTS - 1)), 1000)" >/dev/null || bail "setup failed"
D=$(dir sl)
started=$(sql sl "SELECT pg_postmaster_start_time()")
ENDS="SELECT part_id || ' ' || coalesce(key_lo::text, 'NULL') FROM sift._test_f2_partitions('s_idx')"
mapfile -t ENDS < <(sql sl "($ENDS ORDER BY key_lo NULLS FIRST LIMIT 4) UNION ALL ($ENDS ORDER BY key_lo DESC NULLS LAST LIMIT 4)")

# writer w (1 to 8) commits a part of one row to an end partition of its own, ROUNDS times (tids from base on)
writer() {
	local p lo
	read -r p lo <<<"${ENDS[$1 - 1]}"
	echo "DO \$\$ BEGIN FOR i IN 1..$2 LOOP PERFORM sift._test_f2_add_part('s_idx', $p, $lo, 1, ${lo/NULL/0} + i, $3 + i * 10 + $1); COMMIT; END LOOP; END \$\$"
}
checks() { sql sl "SELECT sift._test_f2_check('s_idx')" 2>&1; }
check "the index has $PARTS partitions" test "$(sql sl "SELECT count(*) FROM sift._test_f2_partitions('s_idx')")" == "$PARTS"
for w in $(seq $WRITERS); do
	sql sl "SET application_name = 'w$w'" "$(writer "$w" $ROUNDS 0)" >"$D/w$w.out" 2>&1 &
done
wait
out=$(cat "$D"/w*.out)
check "writers of the first and of the last partitions commit at once with no error" test -z "$out"
out=$(sql sl "SELECT pg_postmaster_start_time() = '$started'" 2>&1)
check "and the server did not restart" test "$out" == t
out=$(sql sl "SELECT sum(nparts) FROM sift._test_f2_partitions('s_idx')" 2>&1)
check "every part is there" test "$out" == $((1 + WRITERS * ROUNDS))
out=$(checks)
check "the index checks" grep -qx "parts=$((1 + WRITERS * ROUNDS)) .*" <<<"$out"

# the writers run again; one of them is terminated while the others commit through the combiner
for w in $(seq $WRITERS); do
	sql sl "SET application_name = 'w$w'" "$(writer "$w" 400 100000)" >"$D/x$w.out" 2>&1 &
done
victim=
wait_for "victim=\$(sql sl \"SELECT pid FROM pg_stat_activity WHERE application_name = 'w1' AND state = 'active'\"); [[ -n \$victim ]]" 30
sql sl "SELECT pg_terminate_backend($victim)" >/dev/null
check "a writer the combiner holds leaves within a second of its termination" wait_for "[[ \$(sql sl \"SELECT count(*) FROM pg_stat_activity WHERE pid = $victim\") == 0 ]]" 1
wait
out=$(cat "$D"/x[2-8].out; checks)
check "and the other writers finish with no error" grep -qx 'parts=[0-9]* .*' <<<"$out"

# a statement that fails while it reads a shared dictionary in place (the shared dictionaries are those of the segments of Format 1)
sql sl "SET sift.test_create_format1 = on" "CREATE TABLE d AS SELECT g AS id, 'k' || md5(g::text) AS s FROM generate_series(1, 20000) g" "CREATE INDEX d_col ON d USING sift_col (id, s) WITH (rollup_columns = 'off')" \
	"VACUUM (FREEZE, ANALYZE) d" >/dev/null || bail "dictionary setup failed"
Q="SELECT s, count(*) FROM d WHERE s LIKE '%ab%' GROUP BY s"
SET="SET max_parallel_workers_per_gather = 0; SET sift.agg_force = on; SET sift.test_col_dict_shared = 1"
sql sl "$SET" "$Q" >/dev/null
out=$(psql -X -At -q -h "$D/sock" -p "$(port sl)" -U postgres -d postgres 2>&1 <<SQL
$SET;
SET sift.test_dsm_fail = 'dictshm_read';
$Q;
SELECT 'P:' || pins FROM sift._test_col_dict_pinned();
SQL
)
check "a statement fails while it reads a shared dictionary in place" grep -q dictshm_read <<<"$out"
check "and the pin it took is given back when it fails" test "$(grep '^P:' <<<"$out")" == P:0

# a statement that fails while it puts the list of a partition in the area of the snapshots
sql sl "SELECT sift._test_shm_manifest_publish(900601, 1, 4, true)" "SELECT pg_sleep(600)" >/dev/null 2>&1 &
holder=$!
wait_for "[[ \$(sql sl \"SELECT sift._test_shm_manifest_peek(900601) IS NOT NULL\") == t ]]" 30 || bail "no snapshot to read"
before=$(sql sl "SELECT sift._test_shm_pool_used(0)")
fail=$(sql sl "SET sift.test_dsm_fail = 'part_list_install'" "SELECT sift._test_shm_manifest_read(900601, 1, 4096)" 2>&1) || true
after=$(sql sl "SELECT sift._test_shm_pool_used(0)")
reads=$(sql sl "SELECT sift._test_shm_manifest_read(900601, 1, 4096)" "SELECT sift._test_shm_manifest_read(900601, 1, 4096)" | cut -d' ' -f3- | paste -sd, -)
out="$fail / used $before -> $after / $reads"
check "a failed install of a list gives its shared memory back and the next reader makes the list" test "$(grep -c part_list_install <<<"$fail") $after $reads" == "1 $before made,not made"
sql sl "SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE query LIKE '%pg_sleep(600)%' AND pid <> pg_backend_pid()" >/dev/null
wait "$holder" 2>/dev/null
sql sl "SELECT sift._test_shm_manifest_invalidate(900601, false)" >/dev/null
finish
