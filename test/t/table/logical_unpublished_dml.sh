#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# A sift_table no publication names, under wal_level = logical with pg_sift preloaded and 8,192 replication slots: whether a logical slot of the database decodes every table is asked once
# for a transaction, not for every row (the walk of the slots takes the lock of their array and the spinlock of each), so its inserts and updates run at the rate they have under
# wal_level = replica. A slot of test_decoding made before the next transaction still gets the rows of that transaction.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 4
node_init lu 57 "wal_level = logical
shared_preload_libraries = 'pg_sift'
max_replication_slots = 8192
max_wal_senders = 4
autovacuum = off
sift.async_maintenance = off
${SIFT_TESTING:+sift.test_create_format1 = off}"
node_start lu || bail "start failed"
EXT=pg_sift
[[ -z ${SIFT_TESTING:-} ]] || EXT="pg_sift_test CASCADE"
sql lu "CREATE EXTENSION $EXT" "CREATE TABLE t (id int, v int) USING sift_table" >/dev/null || bail "setup failed"

dml_seconds() {
	local start=$(date +%s.%N)

	sql lu "TRUNCATE t" "INSERT INTO t SELECT g, 0 FROM generate_series(1, 20000) g" "UPDATE t SET v = v + 1" >/dev/null || bail "the DML failed"
	echo "$(date +%s.%N) - $start" | bc
}

logical=$(dml_seconds)
node_stop lu
node_conf lu "wal_level = replica"
node_start lu || bail "restart failed"
replica=$(dml_seconds)
out="20,000 inserted and updated rows: $logical s under wal_level = logical, $replica s under replica"
diag "$out"
check "the DML of an unpublished sift_table runs under wal_level = logical about as fast as under replica" awk -v l="$logical" -v r="$replica" 'BEGIN { exit !(l <= 2 * r + 0.5) }'

node_stop lu
node_conf lu "wal_level = logical"
node_start lu || bail "restart failed"
if [[ ! -e $(pg_config --pkglibdir)/test_decoding.so ]]; then
	skip 2 "this PostgreSQL has no test_decoding"
else
	sql lu "SELECT 1 FROM pg_create_logical_replication_slot('td', 'test_decoding')" >/dev/null || bail "the slot was not made"
	sql lu "INSERT INTO t VALUES (-1, 7)" "UPDATE t SET v = 8 WHERE id = -1" >/dev/null || bail "the DML after the slot failed"
	out=$(sql lu "SELECT data FROM pg_logical_slot_get_changes('td', NULL, NULL) WHERE data LIKE 'table public.t:%'")
	check "a slot that decodes every table gets the insert of the next transaction" grep -q "INSERT: id\[integer\]:-1 v\[integer\]:7" <<<"$out"
	check "and its update" grep -q "UPDATE: .*v\[integer\]:8" <<<"$out"
	sql lu "SELECT pg_drop_replication_slot('td')" >/dev/null
fi
check "no backend died" test "$(grep -ac 'terminated by signal' "$(dir lu)/postgres.log")" -eq 0
finish
