#!/usr/bin/env bash
# The registry of the manifests in shared memory under pressure and damage: an index put out of service before the registry knows it cannot be published afterwards (a reader that never
# saw the damage races the statement that met it); a reader that cannot attach the area of the snapshots (the memory of the system is short: sift.test_dsm_fail names manifest_area) is
# not served a snapshot and reads the manifest from the buffers, it never fails for it.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 6
node_init sf 94 "autovacuum = off
sift.shared_cache_kb = 16384"
node_start sf || bail "start failed"
sql sf "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "setup failed"

quiet() { "$@" >/dev/null; }
check "an index the registry never held is put out of service" quiet sql sf "SELECT sift._test_shm_manifest_invalidate(900301, true)"
check "and the manifest of it is not published afterwards" test "$(sql sf "SELECT sift._test_shm_manifest_publish(900301, 1, 0, false)")" == f
sql sf "SELECT sift._test_shm_manifest_invalidate(900301, false)" >/dev/null

PUBLISHED="SELECT count(*) FILTER (WHERE sift._test_shm_manifest_publish(rel::oid, 1, 0, false)) FROM generate_series(920000, 920899) rel"
sql sf "$PUBLISHED" "SELECT count(sift._test_shm_manifest_invalidate(rel::oid, true)) FROM generate_series(920000, 920899) rel" >/dev/null
check "more indexes put out of service than the registry has slots: none of them can be published afterwards" test "$(sql sf "$PUBLISHED")" == 0
sql sf "SELECT count(sift._test_shm_manifest_invalidate(rel::oid, false)) FROM generate_series(920000, 920899) rel" >/dev/null

sql sf "SELECT sift._test_shm_manifest_publish(900401, 1, 4, true)" "SELECT pg_sleep(600)" >/dev/null 2>&1 &
holder=$!
check "a snapshot is published, and held by the backend that made it, for a reader to find" wait_for "[[ \$(sql sf \"SELECT sift._test_shm_manifest_peek(900401) IS NOT NULL\") == t ]]" 30
out=$(sql sf "SET sift.test_dsm_fail = 'manifest_area'" "SELECT sift._test_shm_manifest_read(900401, -1, 0) IS NULL" 2>&1) || true
check "a reader that cannot attach the area is told there is no snapshot and does not fail" test "$out" == t
check "a reader that can attach it is served the snapshot" test "$(sql sf "SELECT sift._test_shm_manifest_read(900401, -1, 0) IS NULL")" == f
sql sf "SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE query LIKE '%pg_sleep(600)%' AND pid <> pg_backend_pid()" >/dev/null
wait "$holder" 2>/dev/null
sql sf "SELECT sift._test_shm_manifest_invalidate(900401, false)" >/dev/null
finish
