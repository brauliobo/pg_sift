#!/usr/bin/env bash
# The WAL a sift_table must not write: a VACUUM that has nothing to change logs nothing, a COPY FREEZE writes frozen parts so that no VACUUM has a freeze to log, and the 16,384 rows of an
# UPDATE under a repeatable read snapshot cost their pending deletes and no more (50 bytes a row, the pages of a chain written four to a record). Every window starts after a CHECKPOINT, so
# a page the first change of it touches is a full page image and shows.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "${SIFT_REPO:?}/test/t/lib/common.bash"

done_testing
node_init sw 2 "wal_level = replica
full_page_writes = on
autovacuum = off
sift.async_maintenance = off
maintenance_work_mem = '4MB'
${SIFT_TESTING:+sift.test_create_format1 = off}"
node_start sw || bail "start failed"
sql sw "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_walinspect" >/dev/null || bail "extension failed"

# The bytes of the sift records (everything but the catalog, the transaction and the checkpoint) that the statements write
wal_bytes() {
	local out
	out=$({
		echo "SELECT pg_current_wal_lsn() AS l0 \\gset"
		printf '%s\n' "$@"
		echo "SELECT pg_current_wal_lsn() AS l1 \\gset"
		echo "SELECT 'BYTES ' || coalesce(sum(record_length), 0) || ' RECORDS ' || count(*) FROM pg_get_wal_records_info(:'l0', :'l1') WHERE resource_manager NOT IN ('Standby', 'Transaction', 'XLOG', 'Heap', 'Heap2', 'Btree');"
	} | sql sw) || bail "the window failed: $*"
	awk '$1 == "BYTES" { print $2, $4 }' <<<"$out"
}

sql sw "CREATE TABLE idle (id int PRIMARY KEY, grp int NOT NULL, v bigint NOT NULL, pad text NOT NULL) USING sift_table" \
	"INSERT INTO idle SELECT g, g % 64, g, repeat('x', 40) FROM generate_series(1, 20000) g" "VACUUM idle" "VACUUM idle" "VACUUM idle" >/dev/null || bail "load idle failed"
sql sw "CHECKPOINT" >/dev/null
read -r idle_bytes _ < <(wal_bytes "VACUUM idle;")
diag "STORAGE_WAL_WASTE idle VACUUM bytes: $idle_bytes"
check "a VACUUM of a table nothing changed in writes no WAL" test "${idle_bytes:-1}" = 0

sql sw "BEGIN; CREATE TABLE frz (id int, grp int NOT NULL, v bigint NOT NULL, pad text NOT NULL) USING sift_table; COPY frz FROM PROGRAM 'seq 1 20000 | awk ''{ print \$1 \"\\t\" \$1 % 64 \"\\t\" \$1 \"\\txxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\" }''' FREEZE; COMMIT" >/dev/null || bail "COPY FREEZE failed"
check "a COPY FREEZE writes no part that a VACUUM has to freeze" test "$(sql sw "SELECT count(*) FROM sift.parts('frz') WHERE unfrozen")" = 0
sql sw "VACUUM frz" "CHECKPOINT" >/dev/null
read -r frz_bytes _ < <(wal_bytes "VACUUM frz;")
diag "STORAGE_WAL_WASTE VACUUM after COPY FREEZE bytes: $frz_bytes"
check "the VACUUM after a COPY FREEZE has nothing to log" test "${frz_bytes:-1}" = 0

sql sw "CREATE TABLE snp (id int PRIMARY KEY, grp int NOT NULL, v1 bigint NOT NULL, v2 bigint NOT NULL, v3 int NOT NULL, pad text NOT NULL) USING sift_table" \
	"INSERT INTO snp SELECT g, g % 64, g, g * 3, g % 1000, repeat('x', 40) FROM generate_series(1, 16384) g" "VACUUM snp" "SELECT sift.maintain('snp')" \
	"CREATE INDEX snp_grp ON snp (grp)" "VACUUM snp" "SELECT sift.maintain('snp')" >/dev/null || bail "load snp failed"
coproc SNAP { psql -X -At -q -v ON_ERROR_STOP=1 -h "$(node_sock sw)" -p "$(port sw)" -U postgres -d "$(node_db sw)"; }
printf '%s\n' "BEGIN ISOLATION LEVEL REPEATABLE READ;" "SELECT count(*) AS c FROM snp \\gset" "\\echo READY" >&"${SNAP[1]}"
read -r -u "${SNAP[0]}" ready
sql sw "CHECKPOINT" >/dev/null
read -r upd_bytes upd_records < <(wal_bytes "UPDATE snp SET grp = grp + 1;")
echo '\q' >&"${SNAP[1]}"
wait "$SNAP_PID"
diag "STORAGE_WAL_WASTE 16,384 updates under a snapshot: $upd_bytes bytes in $upd_records records"
check "an UPDATE under a snapshot writes at most 55 bytes a row" test "${upd_bytes:-999999999}" -le $((55 * 16384))
check "the pending deletes of it are logged four pages to a record" test "${upd_records:-999999}" -lt 70
check "the table verifies against its heap" test "$(sql sw "SELECT count(*) FROM sift.check('snp', true)")" = 0
finish
