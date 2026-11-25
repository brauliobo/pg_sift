#!/usr/bin/env bash
# The local cache of a tiered table is ephemeral: a base backup does not carry it, and a session that opens it removes the temporary files of dead backends only, never the one a live backend is writing.
. "$(dirname "$0")/lib.bash"

plan 5
node_init tk 6 "autovacuum = off"
node_start tk || bail "start failed"
STORE=$(dir tk)/store
sql tk "CREATE EXTENSION pg_sift" "CREATE TABLE ev (id int8, ts timestamp NOT NULL, v int, pad text) USING sift_table" \
	"INSERT INTO ev SELECT i, timestamp '2020-01-01' + i * interval '1 hour', i % 100, md5(i::text) FROM generate_series(1, 5000) i" "VACUUM (FREEZE) ev" \
	"SELECT sift.set_tier_policy('ev', 'file://$STORE', '30 days')" "VACUUM ev" >/dev/null || bail "setup failed"
sql tk "SELECT sum(v), sum(length(pad)) FROM ev" >/dev/null
chunks() { find "$1" -name '*.chk' | wc -l; }
check "a read of the tiered part fills the cache" test "$(chunks "$(dir tk)/data")" -gt 0

BAK=$(dir tk)/backup
pg_basebackup -D "$BAK" -h "$(dir tk)/sock" -p "$(port tk)" -U postgres -X fetch -c fast >/dev/null 2>&1
check "a base backup holds no chunk of the cache" test -f "$BAK/PG_VERSION" -a "$(chunks "$BAK")" == 0

CACHE=$(dirname "$(find "$(dir tk)/data" -name '*.chk' | head -1)")
tail -f /dev/null &
LIVE=$!
true &
DEAD=$!
wait $DEAD
touch "$CACHE/c.0123456789abcdef.tmp.$LIVE.1" "$CACHE/c.fedcba9876543210.tmp.$DEAD.1"
sql tk "SELECT sum(v), sum(length(pad)) FROM ev" >/dev/null
check "the temporary file of a live backend stays" test -f "$CACHE/c.0123456789abcdef.tmp.$LIVE.1"
check "the temporary file of a dead one goes" test ! -e "$CACHE/c.fedcba9876543210.tmp.$DEAD.1"
kill $LIVE
wait $LIVE 2>/dev/null
check "sift.check finds the table sound" test "$(sql tk "SELECT count(*) FROM sift.check('ev')")" == 0

finish
