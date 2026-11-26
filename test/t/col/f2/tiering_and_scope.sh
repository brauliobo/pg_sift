#!/usr/bin/env bash
# Tiering and scope of a sift_table: a part the move skips does not stop the ones after it, a store that fails ends the step at its first failure, a rewrite of the relation never
# overwrites the objects of the one before it, backends that read the same object share the chunks of its cache, and a scope reads a table with a row in an unflushed slab and one that was truncated.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 9
node_init tk 6 "autovacuum = off"
node_start tk || bail "start failed"
STORE=$(dir tk)/store
rows() { echo "INSERT INTO $1 SELECT i, timestamp '2020-01-01' + i * interval '1 hour', i % 100, $4 FROM generate_series($2, $3) i"; }
tiered() { sql tk "SELECT count(*) FROM sift.tier_status WHERE relation = '$1'::regclass"; }
keys() { find "$STORE" -name '*.siftpart' | sort; }
sql tk "CREATE EXTENSION pg_sift" "CREATE TABLE sk (id int8, ts timestamp NOT NULL, v int, pad text) USING sift_table" "$(rows sk 1 6000 "repeat(md5(i::text), 300)")" \
	"VACUUM (FREEZE) sk" "VACUUM (FREEZE) sk" "$(rows sk 10000 15000 "md5(i::text)")" "VACUUM (FREEZE) sk" \
	"SELECT sift.set_tier_policy('sk', 'file://$STORE', '30 days')" >/dev/null || bail "setup failed"
sql tk "VACUUM sk" >/dev/null
check "a part with wide values that the move skips does not keep the part after it from moving" test "$(tiered sk)" == 1

mkdir -p "$STORE.down"
sql tk "CREATE TABLE sf (id int8, ts timestamp NOT NULL, v int, pad text) USING sift_table" "$(rows sf 1 5001 "md5(i::text)")" "VACUUM (FREEZE) sf" "$(rows sf 10000 15001 "md5(i::text)")" \
	"VACUUM (FREEZE) sf" "$(rows sf 20000 25001 "md5(i::text)")" "VACUUM (FREEZE) sf" "SELECT sift.set_tier_policy('sf', 'file://$STORE.down', '30 days')" >/dev/null || bail "setup failed"
mkdir -p "$STORE.down"; chmod 500 "$STORE.down"
warned=$(sql tk "VACUUM sf" 2>&1)
chmod 700 "$STORE.down"
check "a store that fails ends the step at its first failure" test "$(grep -c 'cannot move a part' <<<"$warned")" == 1
check "and no part of it moved" test "$(tiered sf)" == 0

refill() { sql tk "TRUNCATE st" "$(rows st 1 5001 "md5(($1 + i)::text)")" "VACUUM (FREEZE) st" "VACUUM st" >/dev/null || bail "refill failed"; }
sql tk "CREATE TABLE st (id int8, ts timestamp NOT NULL, v int, pad text) USING sift_table" "SELECT sift.set_tier_policy('st', 'file://$STORE.re', '30 days')" >/dev/null || bail "setup failed"
refill 0
OLD=$(find "$STORE.re" -name '*.siftpart')
SUM=$("$SIFT_REPO/scripts/sha256.sh" "$OLD")
refill 7
NEW=$(find "$STORE.re" -name '*.siftpart' ! -path "$OLD")
check "after TRUNCATE the part moves to an object of another name" test -n "$NEW" -a "$NEW" != "$OLD"
check "and the object of the table before it is as it was" test "$("$SIFT_REPO/scripts/sha256.sh" "$OLD")" == "$SUM"

chunks() { find "$(dir tk)/data" -name '*.chk' | sort; }
inodes() { local f; while IFS= read -r f; do [ -n "$f" ] && ls -i "$f" | awk '{print $1}'; done < <(chunks) | sort | tr '\n' ' '; }
sql tk "CREATE TABLE sa (id int8, ts timestamp NOT NULL, v int, pad text) USING sift_table" "$(rows sa 1 5001 "md5(i::text)")" "VACUUM (FREEZE) sa" "$(rows sa 10000 15000 "md5((i * 3)::text)")" \
	"VACUUM (FREEZE) sa" "SELECT sift.set_tier_policy('sa', 'file://$STORE.shared', '30 days')" "VACUUM sa" >/dev/null || bail "setup failed"
chunks | xargs -r rm -f
sess_open tk A
sess_open tk B
sess_say B "VACUUM sa;" "SELECT 'b1';"
wait_for "grep -q '^b1' $(dir tk)/B.out" 30
sess_say A "SELECT sum(length(pad)) FROM sa;" "SELECT 'a1';"
wait_for "grep -q '^a1' $(dir tk)/A.out" 30
BEFORE=$(inodes)
sess_say B "SELECT sum(length(pad)) FROM sa;" "SELECT 'b2';"
wait_for "grep -q '^b2' $(dir tk)/B.out" 30
sess_close A
sess_close B
check "a backend that reads what another backend cached takes it from the cache directory and fetches nothing" test "$(inodes)" == "$BEFORE"

node_stop tk

node_init sc 7 "autovacuum = off"
node_start sc || bail "start failed"
scope_db() {
	sql sc "CREATE EXTENSION pg_sift" \
		"CREATE TABLE pairs (member_id int NOT NULL, scope_key int NOT NULL, PRIMARY KEY (member_id, scope_key))" \
		"INSERT INTO pairs SELECT m, 1 + (m * 31 + j * 13) % 20 FROM generate_series(1, 500) m, generate_series(0, 3) j WHERE j < m % 4 GROUP BY 1, 2" \
		"SET sift.test_create_format1 = on" "CREATE INDEX pairs_journal ON pairs USING sift_col (member_id, scope_key) WITH (track_changes = 'ctid')" "SET sift.test_create_format1 = off" \
		"CREATE TABLE facts (id int, member_id int, ts timestamptz NOT NULL, a int NOT NULL, b int NOT NULL) USING sift_table WITH (autovacuum_enabled = false)" \
		"CREATE TABLE facts_heap (id int, member_id int, ts timestamptz NOT NULL, a int NOT NULL, b int NOT NULL)" \
		"INSERT INTO facts_heap SELECT i, 1 + (i * 7919) % 500, timestamptz '2026-01-01' + i * interval '7 minutes', i % 100, i % 1000 FROM generate_series(1, 20000) i" \
		"INSERT INTO facts SELECT * FROM facts_heap" \
		"SELECT sift.table_set_option('facts', 'scope', 'tenant: member_id -> pairs(member_id, scope_key) cols(a, b)')" "VACUUM (FREEZE) facts" "SELECT sift.maintain('facts')"
}
Q="SELECT p.scope_key, count(*), sum(f.b) FROM %s f JOIN pairs p ON f.member_id = p.member_id WHERE p.scope_key IN (3, 7, 11) GROUP BY 1 ORDER BY 1"
TOP="SELECT f.id FROM %s f JOIN pairs p ON f.member_id = p.member_id WHERE p.scope_key = 5 ORDER BY f.ts DESC, f.id LIMIT 10"
same() { [[ $(sql sc "$(printf "$1" facts)" 2>&1) == "$(sql sc "$(printf "$1" facts_heap)")" ]]; }
scope_db >/dev/null || bail "scope setup failed"

TR="BEGIN; CREATE TABLE tn (id int, member_id int, ts timestamptz NOT NULL, a int NOT NULL, b int NOT NULL) USING sift_table;
 INSERT INTO tn SELECT * FROM facts_heap; SELECT sift.table_set_option('tn', 'scope', 'tenant: member_id -> pairs(member_id, scope_key) cols(a, b)');
 SELECT sift.maintain('tn'); $(printf "$Q" tn); TRUNCATE tn; INSERT INTO tn SELECT id, member_id, ts, a, b + 1000 FROM facts_heap; SELECT sift.maintain('tn'); $(printf "$Q" tn); ROLLBACK"
WANT=$(sql sc "$(printf "$Q" facts_heap | sed 's/sum(f.b)/sum(f.b + 1000)/')")
check "a table that is truncated and filled again in its transaction is not answered from the summary it had" test "$(sql sc "$TR" 2>&1 | tail -3)" == "$WANT"

sql sc "INSERT INTO facts VALUES (900001, 7, timestamptz '2026-06-01', 1, 5000)" "INSERT INTO facts_heap VALUES (900001, 7, timestamptz '2026-06-01', 1, 5000)" >/dev/null
check "a scoped sum and count over a table with a row in an unflushed slab are the ones of the join" same "$Q"
check "and its newest rows" same "$TOP"

finish
