#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The registry of the manifests in shared memory when the snapshot of a slot cannot be dropped (the memory of the system is short, or a session cannot attach the area of the snapshots): an index
# put out of service is out of service, as damaged as the statement that met the damage said it is, and the snapshot a publication made for its commit is not left behind by a publication that
# fails (sift.test_dsm_fail names manifest_area and manifest_drop_snap).
. "$(dirname "$0")/lib.bash"

out=
plan 3
node_init pm 3 "autovacuum = off
sift.shared_cache_kb = 16384"
node_start pm || bail "start failed"
sql pm "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "setup failed"

sql pm "SELECT sift._test_shm_manifest_publish(900501, 1, 4, true)" "SELECT pg_sleep(600)" >/dev/null 2>&1 &
holder=$!
check "a snapshot is published, and held by the backend that made it" wait_for "[[ \$(sql pm \"SELECT sift._test_shm_manifest_peek(900501) IS NOT NULL\") == t ]]" 30

out=$(sql pm "SET sift.test_dsm_fail = 'manifest_area'" "SELECT sift._test_shm_manifest_invalidate(900501, true)" 2>&1) || true
check "an index is out of service when the session that met its damage cannot drop the snapshot" test "$(sql pm "SELECT sift._test_shm_manifest_peek(900501) IS NULL AND NOT sift._test_shm_manifest_publish(900501, 2, 0, false)")" == t

sql pm "SELECT sift._test_shm_manifest_publish(900502, 1, 4, true)" >/dev/null
before=$(sql pm "SELECT sift._test_shm_manifest_stat('snaps_live')")
sql pm "SET sift.test_dsm_fail = 'manifest_drop_snap'" "SELECT sift._test_shm_manifest_publish(900502, 2, 4, true)" >/dev/null 2>&1
after=$(sql pm "SELECT sift._test_shm_manifest_stat('snaps_live')")
out="before $before after $after"
check "a publication that fails leaves no snapshot behind" test "$after" == "$before"

sql pm "SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE query LIKE '%pg_sleep(600)%' AND pid <> pg_backend_pid()" >/dev/null
wait "$holder" 2>/dev/null
sql pm "SELECT sift._test_shm_manifest_invalidate(900501, false)" "SELECT sift._test_shm_manifest_invalidate(900502, false)" >/dev/null
finish
