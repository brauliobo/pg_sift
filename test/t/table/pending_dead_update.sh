#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The delete of the version an UPDATE of a sift_table replaced is two records of the pending deletes of its partition: the address of the old version with a flag that says it was an UPDATE, and the
# address of the new one with the flag of a successor. The set of addresses a snapshot sees deleted (the one a reader of the partition matches rows with) holds the address of each replaced version
# and nothing else: no flag in an address, no address of a successor. The updated column is indexed: an UPDATE of a column no index holds is a column delta that keeps the address and replaces nothing.
# 30 rows are 60 records, under SIFT_BULK_DEL_MIN: they stay pending. A larger UPDATE is a bulk delete that folds every pending delete of the partition into the deletion vector, whose settled pairs a reader does not match rows with.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 5
[[ -n ${SIFT_TESTING:-} ]] || { skip 5 "needs the pending deletes of the testing flavour"; finish; }
node_init pd 99 "autovacuum = off
sift.maintenance_naptime = 60000
sift.test_create_format1 = off"
node_start pd || bail "start failed"
sql pd "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE st (id int PRIMARY KEY, v int) USING sift_table WITH (autovacuum_enabled = false)" "CREATE INDEX st_v ON st (v)" \
	"INSERT INTO st SELECT g, g FROM generate_series(1, 5000) g" "CHECKPOINT" >/dev/null || bail "setup failed"
sql pd "UPDATE st SET v = -v WHERE id <= 30" >/dev/null || bail "update failed"

SEEN="SELECT count(*) AS n, count(*) FILTER (WHERE addr < 0 OR addr >= (1::int8 << 48)) AS flagged, count(DISTINCT addr) AS addrs FROM sift._test_f2_pending_dead('st')"
IFS='|' read -r N FLAGGED ADDRS <<<"$(sql pd "$SEEN")"
diag "pending deletes: $N, with a flag: $FLAGGED, distinct addresses: $ADDRS"
check "the 30 replaced versions are pending deletes of the table" test "$N" == 30
check "no address of the set carries a flag" test "$FLAGGED" == 0
check "each address is one version" test "$ADDRS" == 30
sql pd "UPDATE st SET v = -v WHERE id > 1000 AND id <= 1040" >/dev/null || bail "bulk update failed"
check "a bulk UPDATE of 40 rows folds them with the pending deletes of the partition into the deletion vector" \
	test "$(sql pd "SELECT (SELECT count(*) FROM sift._test_f2_pending_dead('st')), sum(dead_rows) FROM sift.table_segments('st')")" == "0|70"
check "the table shows the updated rows" test "$(sql pd "SELECT count(*) FROM st WHERE v < 0")" == 70
finish
