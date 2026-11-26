#!/usr/bin/env bash
# A session that sees the location of a tier policy change reads the parts it keeps open from the new location and closes what it opened for the old one: the libcurl handle, its
# connection and the credentials. The endpoint of an s3:// policy flips between two names of one mock store; a read through a closed port fails, and after many flips the session
# holds as many sockets as after the first ones.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

python3 -c 'import ctypes; ctypes.CDLL("libcurl.so.4")' 2>/dev/null || { echo "1..0 # SKIP python3 or libcurl.so.4 is not on this host"; exit 0; }
plan 3
MOCK_PORT=$((SIFT_PORT + 8))
python3 "${SIFT_REPO:?}/test/t/table/f2/object_store_mock.py" "$MOCK_PORT" &
MOCK=$!
trap 'kill $MOCK 2>/dev/null; cleanup' EXIT
wait_for "curl -fsS http://127.0.0.1:$MOCK_PORT/__ctl/count/GET >/dev/null 2>&1" 30 || bail "the mock store did not start"
export AWS_ACCESS_KEY_ID=AKIDTEST AWS_SECRET_ACCESS_KEY=secret AWS_REGION=us-east-1
node_init rb 7 "sift.test_create_format1 = off
autovacuum = off"
node_start rb || bail "start failed"
ND=$(dir rb)
sql rb "CREATE EXTENSION pg_sift" "CREATE TABLE ev (id int8, ts timestamp NOT NULL, v int, pad text) USING sift_table" \
	"INSERT INTO ev SELECT i, timestamp '2020-01-01' + i * interval '1 minute', i % 100, md5(i::text) FROM generate_series(1, 4000) i" "VACUUM (FREEZE) ev" \
	"SELECT sift.set_tier_policy('ev', 's3://bucket/pre', '30 days', 'http://127.0.0.1:$MOCK_PORT')" "VACUUM ev" >/dev/null || bail "setup failed"
check "the part is in the store" test "$(sql rb "SELECT count(*) FROM sift.tier_status")" == 1

moved() { stmts+=("UPDATE sift.tier_policies SET endpoint = 'http://$1'" "ALTER TABLE sift.tier_policies SET (fillfactor = 100)" "\\! find '$ND/data' -name '*.chk' -delete"); }
stmts=("SELECT sum(length(pad)) FROM ev")
moved 127.0.0.1:9
out=$(sql rb "${stmts[@]}" "SELECT sum(length(pad)) FROM ev" 2>&1)
check "a read after the endpoint moved to a closed port goes there, not to the store the session opened" bash -c '[[ $1 == *"transfer failed"* ]]' _ "$out"

ROUNDS=$(knob ROUNDS 24 12)
stmts=()
for ((i = 1; i <= ROUNDS; i++)); do
	moved "$([[ $((i % 2)) == 0 ]] && echo 127.0.0.1 || echo localhost):$MOCK_PORT"
	stmts+=("SELECT sum(length(pad)) FROM ev" "COPY (SELECT 1) TO PROGRAM 'cat >/dev/null; echo $i \$(ls -l /proc/\$PPID/fd | grep -c socket:) >>$ND/fds'")
done
sql rb "${stmts[@]}" >/dev/null || bail "the flips failed"
out=$(( $(awk -v r="$ROUNDS" '$1 == r {print $2}' "$ND/fds") - $(awk '$1 == 4 {print $2}' "$ND/fds") ))
check "$ROUNDS flips of the endpoint leave no socket behind" test "$out" -le 0
finish
