#!/usr/bin/env bash
# The WAL resource manager of sift_table. 5.0 writes its change records under the registered id (src/table/rmgr_id.h) and keeps the
# id that 4.x wrote (the experimental one) as a replay-only alias with the same redo and decode, so that the order of an update (standbys first,
# docs/UPGRADING.md) and the crash recovery of a data directory written by 4.x need nothing but the new library:
#   crash:   the last 4.3 release publishes a sift_table and crashes; this tree replays the records of the old id, a logical slot made before
#            decodes them, and what this tree writes afterwards carries the new id (pg_waldump) with a stable name for both (pg_walinspect).
#   standby: a 4.3 primary streams those records to a standby on this tree, which replays them.
# The release is built from its tag by scripts/old-lib-check.sh (PostgreSQL 18).
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
if ((PG_MAJOR != 18)); then echo '1..0 # SKIP the released libraries build on PostgreSQL 18 only (two library roots)'; exit 0; fi
ref=$(git -C "$REPO" tag -l 'v4.3.*' --sort=-v:refname | head -n 1)
[[ -n $ref ]] || { echo '1..0 # SKIP no release tag v4.3.*'; exit 0; }
newroot=$ROOT
build_old "$ref"; oldroot=$OLDROOT
define() { sed -n "s/^#define $1[[:space:]]\{1,\}\(.*[^[:space:]]\)[[:space:]]*\$/\1/p" "$REPO/src/table/rmgr_id.h"; }
new_id=$(define SIFT_TBL_RMGR_ID)
new_name=$(define SIFT_TBL_RMGR_NAME | tr -d '"')
legacy_name=$(define SIFT_TBL_LEGACY_RMGR_NAME | tr -d '"')
legacy_id=128
plan 14

CONF="wal_level = logical
shared_preload_libraries = 'pg_sift'
max_wal_senders = 4
autovacuum = off"
SETUP=("CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_walinspect" "CREATE TABLE orders (id int PRIMARY KEY, v int) USING sift_table" "CREATE PUBLICATION pub FOR TABLE orders")
WRITE=("INSERT INTO orders SELECT g, g % 7 FROM generate_series(1, 2000) g" "UPDATE orders SET v = v + 1 WHERE id % 5 = 0"
	"DELETE FROM orders WHERE id % 11 = 0")
MORE=("INSERT INTO orders SELECT g, g % 7 FROM generate_series(10001, 12000) g" "UPDATE orders SET v = v + 1 WHERE id % 5 = 0"
	"DELETE FROM orders WHERE id % 11 = 0")
SUMMARY="SELECT count(*), sum(v) FROM orders"
# wal_names NODE FROM_LSN: the resource managers of sift_table in the WAL since FROM_LSN, as pg_walinspect on the node (library preloaded) names them
wal_names() {
	local upto='pg_current_wal_lsn()'
	[[ $(sql "$1" "SELECT pg_is_in_recovery()") != t ]] || upto='pg_last_wal_replay_lsn()'
	sql "$1" "SELECT DISTINCT resource_manager FROM pg_get_wal_records_info('$2', $upto) WHERE resource_manager LIKE 'sift_table%' ORDER BY 1" 2>&1
}
waldump_count() { # NODE FROM_LSN ID: records of the resource manager id that pg_waldump (which loads no library) prints for the node's WAL
	pg_waldump -p "$(dir "$1")/data/pg_wal" -s "$2" 2>/dev/null | grep -c "^rmgr: custom$3 "
}

# crash
a=rmgr_crash
node_init "$a" 1 "$CONF"
use_root "$a" "$oldroot"
node_start "$a" || bail "$ref primary start failed"
sql "$a" "${SETUP[@]}" "SELECT pg_create_logical_replication_slot('s', 'pgoutput')" >/dev/null || bail "setup failed"
lsn0=$(sql "$a" "SELECT pg_current_wal_lsn()")
sql "$a" "${WRITE[@]}" >/dev/null || bail "$ref writes failed"
expected=$(sql "$a" "$SUMMARY")
crash "$a"
use_root "$a" "$newroot"
node_start "$a" || bail "this tree did not replay the WAL of $ref (resource manager $legacy_id)"
check "crash recovery by this tree replays the sift_table records of $ref" test "$(sql "$a" "$SUMMARY")" == "$expected"
check "the rows verify after the replay" test "$(sql "$a" "SELECT count(*) FROM orders o WHERE v IS NULL")" == 0
out=$(wal_names "$a" "$lsn0")
check "the records of $ref are named $legacy_name" test "$out" == "$legacy_name"
check "pg_waldump shows them under the experimental id" test "$(waldump_count "$a" "$lsn0" $legacy_id)" -gt 0
out=$(sql "$a" "SELECT count(*) >= 2000 FROM pg_logical_slot_peek_binary_changes('s', NULL, NULL, 'proto_version', '1', 'publication_names', 'pub')" 2>&1)
check "a logical slot made under $ref decodes those records" test "$out" == t
lsn1=$(sql "$a" "SELECT pg_current_wal_lsn()")
sql "$a" "${MORE[@]}" >/dev/null || bail "writes of this tree failed"
out=$(wal_names "$a" "$lsn1")
check "the records of this tree are named $new_name" test "$out" == "$new_name"
check "pg_waldump shows them under id $new_id" test "$(waldump_count "$a" "$lsn1" "$new_id")" -gt 0
check "this tree writes none under the experimental id" test "$(waldump_count "$a" "$lsn1" $legacy_id)" == 0
out=$(sql "$a" "SELECT count(*) >= 4000 FROM pg_logical_slot_peek_binary_changes('s', NULL, NULL, 'proto_version', '1', 'publication_names', 'pub')" 2>&1)
check "the same slot decodes the records of both ids" test "$out" == t
node_stop "$a"

# standby
p=rmgr_primary s=rmgr_standby
node_init "$p" 3 "$CONF"
use_root "$p" "$oldroot"
node_start "$p" || bail "$ref primary start failed"
sql "$p" "${SETUP[@]}" >/dev/null || bail "setup failed"
node_clone "$p" "$s" 4 "hot_standby_feedback = on"
use_root "$s" "$newroot"
node_start "$s" || bail "standby start failed"
lsn0=$(sql "$p" "SELECT pg_current_wal_lsn()")
sql "$p" "${WRITE[@]}" >/dev/null || bail "$ref writes failed"
check "a standby on this tree replays the stream of a $ref primary" catchup "$p" "$s"
check "the standby has the rows of the primary" test "$(sql "$s" "$SUMMARY")" == "$(sql "$p" "$SUMMARY")"
out=$(wal_names "$s" "$lsn0")
check "the standby names the records $legacy_name" test "$out" == "$legacy_name"
check "the standby replays without a FATAL or a missing resource manager" test "$(grep -c -E 'FATAL|PANIC|not registered' "$(dir "$s")/postgres.log")" == 0
node_stop "$s"
node_stop "$p"

# the other direction is the order that docs/UPGRADING.md forbids: a 4.3 standby of a primary on this tree stops at the first record of the new id
p=rmgr_new_primary s=rmgr_old_standby
node_init "$p" 5 "$CONF"
use_root "$p" "$newroot"
node_start "$p" || bail "primary start failed"
sql "$p" "${SETUP[@]}" >/dev/null || bail "setup failed"
node_clone "$p" "$s" 6 "hot_standby_feedback = on"
use_root "$s" "$oldroot"
node_start "$s" || bail "$ref standby start failed"
sql "$p" "${WRITE[@]}" >/dev/null || bail "writes failed"
out=$(wait_for "grep -q 'FATAL:  resource manager with ID $new_id not registered' '$(dir "$s")/postgres.log'" 30 && echo found)
check "a $ref standby of a primary on this tree stops at the records of id $new_id" test -n "$out"
node_stop "$s"
node_stop "$p"
finish
