#!/usr/bin/env bash
# The WAL of 800 one-row UPDATEs of a sift_table with a repeatable read snapshot open: the pending deletes grow to four pages and the buffer of new rows is flushed again and again, and none
# of those commits rewrites the list of parts of the partition unless a part changed, so the full page images of the sift records stay under 62,000 bytes (73,700 when each of them did).
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "${SIFT_REPO:?}/test/t/lib/common.bash"

done_testing
node_init sp 2 "wal_level = replica
autovacuum = off
sift.async_maintenance = off
sift.standby_conflicts = on
sift.backend_cache_kb = 2048
maintenance_work_mem = '4MB'
temp_file_limit = '1MB'"
node_start sp || bail "start failed"
sql sp "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_walinspect" >/dev/null || bail "extension failed"

sql sp "CREATE TABLE snp (id int PRIMARY KEY, grp int NOT NULL, v1 bigint NOT NULL, v2 bigint NOT NULL, v3 int NOT NULL, pad text NOT NULL) USING sift_table" \
	"INSERT INTO snp SELECT g, g % 64, g, g * 3, g % 1000, repeat('x', 40) FROM generate_series(1, 8192) g" "VACUUM snp" "SELECT sift.maintain('snp')" \
	"CREATE INDEX snp_grp ON snp (grp)" "VACUUM snp" "SELECT sift.maintain('snp')" >/dev/null || bail "load snp failed"
coproc SNAP { psql -X -At -q -v ON_ERROR_STOP=1 -h "$(node_sock sp)" -p "$(port sp)" -U postgres -d "$(node_db sp)"; }
printf '%s\n' "BEGIN ISOLATION LEVEL REPEATABLE READ;" "SELECT count(*) AS c FROM snp \\gset" "\\echo READY" >&"${SNAP[1]}"
read -r -u "${SNAP[0]}" ready
sql sp "CHECKPOINT" >/dev/null
out=$({
	echo "SELECT pg_current_wal_lsn() AS l0 \\gset"
	for i in $(seq 800); do echo "UPDATE snp SET grp = grp + 1 WHERE id = $i;"; done
	echo "SELECT pg_current_wal_lsn() AS l1 \\gset"
	echo "SELECT 'FPI ' || coalesce(sum(fpi_length), 0) FROM pg_get_wal_records_info(:'l0', :'l1') WHERE resource_manager = 'Generic';"
} | sql sp) || bail "the updates under a snapshot failed"
echo '\q' >&"${SNAP[1]}"
wait "$SNAP_PID"
fpi=$(awk '$1 == "FPI" { print $2 }' <<<"$out")
diag "SNAP_PEND_DEL_WAL full page image bytes of the sift records: $fpi"
check "the full page images of the 800 updates stay under 62,000 bytes" test "${fpi:-999999}" -lt 62000
check "the table verifies against its heap" test "$(sql sp "SELECT count(*) FROM sift.check('snp', true)")" = 0
check "the updated rows are all there" test "$(sql sp "SELECT count(*) FROM snp")" = 8192
finish
