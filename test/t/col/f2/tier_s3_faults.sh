#!/usr/bin/env bash
# The s3:// backend against a store that fails the way a real one does: it throttles (503), it rotates its credentials (403 ExpiredToken), a proxy answers 200 to a Range request, and its listing
# is long enough for pages with a long continuation token. The store is p98_s3_mock.py on 127.0.0.1; the credentials are read from files, as a pod with a rotating token has them.
. "$(dirname "$0")/lib.bash"

python3 -c 'import ctypes; ctypes.CDLL("libcurl.so.4")' 2>/dev/null || { echo "1..0 # SKIP python3 or libcurl.so.4 is not on this host"; exit 0; }
plan 7
MOCK_PORT=$((SIFT_PORT + 8))
D=$TAPDIR/s3creds
rm -rf "$D"
mkdir -p "$D"
python3 "$(dirname "$0")/p98_s3_mock.py" "$MOCK_PORT" &
MOCK=$!
trap 'kill $MOCK 2>/dev/null; cleanup' EXIT
ctl() { curl -fsS "http://127.0.0.1:$MOCK_PORT/__ctl/$1"; }
wait_for "ctl count/GET >/dev/null 2>&1" 30 || bail "the mock store did not start"
rotate() { echo "$1" >"$D/key_id"; echo "secret-$1" >"$D/secret"; ctl "key/$1" >/dev/null; }
rotate AKIDONE
export AWS_ACCESS_KEY_ID_FILE=$D/key_id AWS_SECRET_ACCESS_KEY_FILE=$D/secret AWS_REGION=us-east-1
unset AWS_ACCESS_KEY_ID AWS_SECRET_ACCESS_KEY AWS_SESSION_TOKEN
node_init s3 7 "autovacuum = off"
node_start s3 || bail "start failed"
ND=$(dir s3)
URI="s3://bucket/pre"
EP="http://127.0.0.1:$MOCK_PORT"
rows() { echo "INSERT INTO $1 SELECT i, timestamp '2020-01-01' + i * interval '1 minute', i % 100, md5(i::text) || md5((i + 1)::text) FROM generate_series($2, $3) i"; }
sql s3 "CREATE EXTENSION pg_sift" "CREATE TABLE ev (id int8, ts timestamp NOT NULL, v int, pad text) USING sift_table" "$(rows ev 1 60000)" "VACUUM (FREEZE) ev" \
	"SELECT sift.set_tier_policy('ev', '$URI', '30 days', '$EP')" >/dev/null || bail "setup failed"
tiered() { sql s3 "SELECT count(*) FROM sift.tier_status"; }
truth() { sql s3 "SELECT md5(string_agg(id || ':' || v || ':' || pad, ',' ORDER BY id)) FROM ev"; }
WANT=$(truth)

ctl fail/503/2 >/dev/null
sql s3 "VACUUM ev" >/dev/null 2>&1
check "two answers of 503 to the upload: the part still moves" test "$(tiered)" == 1
cache_clear() { find "$ND/data" -name '*.chk' -delete; }
cache_clear
ctl fail/429/1 >/dev/null
check "a 429 to the first read: the table reads as it did" test "$(truth)" == "$WANT"

cache_clear
ctl range200/1 >/dev/null
out=$(sql s3 "SELECT sum(length(pad)) FROM ev" 2>&1)
ctl range200/0 >/dev/null
check "a 200 to a Range request is refused as what it is" bash -c '[[ $1 == *"answered HTTP 200"* ]]' _ "$out"

# one session: it opened the store with the first credentials, which the store then stops accepting, and the files hold the new ones
cache_clear
sql s3 "$(rows ev 60001 61000)" "VACUUM (FREEZE) ev" "VACUUM ev" "SELECT 1" >/dev/null 2>&1
out=$(sql s3 "SELECT sum(length(pad)) FROM ev" "\\! echo AKIDTWO >$D/key_id; echo secret-AKIDTWO >$D/secret; curl -fsS http://127.0.0.1:$MOCK_PORT/__ctl/key/AKIDTWO >/dev/null" \
	"$(rows ev 61001 62000)" "VACUUM (FREEZE) ev" "VACUUM ev" "SELECT count(*) FROM sift.tier_status" 2>&1 | tail -1)
check "the credentials rotated under a session: it reads the files again and moves the next part" test "$out" == 3

ctl page/1 >/dev/null
ctl token/1500 >/dev/null
SCOPE=$(sql s3 "SELECT lpad(to_hex(scope_id), 16, '0') FROM sift.tier_policies")
ctl "put/bucket/pre/r$SCOPE/p77-g77.siftpart" >/dev/null
sql s3 "VACUUM ev" >/dev/null 2>&1
check "a listing of pages with a long continuation token reaches the orphan at its end: it is marked for deletion" test "$(ctl keys | grep -c "p77-g77")" -ge 2
check "and the orphan is still there: a standby may read it until its grace is over" test "$(ctl keys | grep -c 'p77-g77.siftpart$')" == 1
check "the table is sound" test "$(sql s3 "SELECT count(*) FROM sift.check('ev')")" == 0

finish
