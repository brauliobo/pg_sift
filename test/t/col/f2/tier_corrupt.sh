#!/usr/bin/env bash
# A byte of the stream of a tiered part that changes in the store is found by the checksum of the range it is in (data_corrupted, XX001), and the bytes that were fetched are not kept in the local
# cache: with the byte put back the same table reads as it did.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 5
node_init tc 5 "autovacuum = off"
node_start tc || bail "start failed"
STORE=$(dir tc)/store
sql tc "CREATE EXTENSION pg_sift" "CREATE TABLE ev (id int8, ts timestamp NOT NULL, v int, pad text) USING sift_table" \
	"INSERT INTO ev SELECT i, timestamp '2020-01-01' + i * interval '1 hour', i % 100, md5(i::text) FROM generate_series(1, 5000) i" "VACUUM (FREEZE) ev" \
	"SELECT sift.set_tier_policy('ev', 'file://$STORE', '30 days')" "VACUUM ev" >/dev/null || bail "setup failed"
truth() { sql tc "SELECT md5(string_agg(id || ':' || ts || ':' || v || ':' || pad, ',' ORDER BY id)) FROM ev"; }
OBJ=$(sql tc "SELECT substr(location, 8) FROM sift.tier_status")
check "the part is in the store" test -f "$OBJ"
SIZE=$(wc -c <"$OBJ" | tr -d " ")
AT=$((SIZE / 2))
WANT=$(truth)
cache_clear() { find "$(dir tc)/data" -name '*.chk' -delete; }
cache_chunks() { find "$(dir tc)/data" -name '*.chk' | wc -l; }
ORIG=$(dd if="$OBJ" bs=1 skip=$AT count=1 2>/dev/null | od -An -tx1 | tr -d ' \n')
FLIP=$([[ $ORIG == ff ]] && echo 00 || echo ff)
cache_clear
printf "\\x$FLIP" | dd of="$OBJ" bs=1 seek=$AT conv=notrunc 2>/dev/null
out=$(sql tc "\\set VERBOSITY sqlstate" "SELECT md5(string_agg(id || ':' || ts || ':' || v || ':' || pad, ',' ORDER BY id)) FROM ev" 2>&1)
check "a changed byte of the stream is data_corrupted" test "${out##*ERROR:  }" == XX001
printf "\\x$ORIG" | dd of="$OBJ" bs=1 seek=$AT conv=notrunc 2>/dev/null
check "the byte put back: the table reads as it did" test "$(truth)" == "$WANT"
cache_clear
check "and the local cache holds only what verified" test "$(truth)" == "$WANT" -a "$(cache_chunks)" -gt 0
check "sift.check finds the object sound" test "$(sql tc "SELECT count(*) FROM sift.check('ev')")" == 0

finish
