#!/usr/bin/env bash
# A snapshot conflict a backend logged covers no later one: after a conflict at the next transaction id (the TTL drop of a tier logs that one), a hot standby query begins whose xmin
# a transaction still running on the primary holds back. Once that transaction ends, a conflict at its id must still be logged, and the standby (max_standby_streaming_delay = 0)
# cancels the query, which the first conflict, replayed before the query began, could not. Needs a preloading server.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "${SIFT_REPO:?}/test/t/lib/common.bash"
plan 3

p=wcc_primary
s=wcc_standby
node_init "$p" 2 "shared_preload_libraries = 'pg_sift'
wal_level = replica
max_wal_senders = 4
autovacuum = off"
node_start "$p" || bail "primary start failed"
sql "$p" "CREATE EXTENSION pg_sift_test CASCADE" "CREATE TABLE e AS SELECT g FROM generate_series(1, 10) g" >/dev/null || bail "setup failed"
node_clone "$p" "$s" 3 "max_standby_streaming_delay = 0"
node_start "$s" || bail "standby start failed"

P=$(dir "$p")
S=$(dir "$s")
sess_open "$p" holder
sess_say holder "BEGIN; SELECT 'held ' || pg_current_xact_id();"
wait_for "grep -q '^held ' '$P/holder.out'" 30 || bail "the primary transaction did not start"
held=$(sed -n 's/^held //p' "$P/holder.out")
sess_open "$p" writer
sess_say writer "SELECT 'first ' || sift._test_wal_conflict('e', pg_snapshot_xmax(pg_current_snapshot()));"
wait_for "grep -q '^first ' '$P/writer.out'" 30 || bail "the first conflict did not run"
catchup "$p" "$s" || bail "the standby did not replay the first conflict"

sess_open "$s" reader
sess_say reader "BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 'xmin ' || pg_snapshot_xmin(pg_current_snapshot());"
wait_for "grep -q '^xmin ' '$S/reader.out'" 30 || bail "the standby snapshot was not taken"
sess_say holder "COMMIT; \\echo @@released"
wait_for "grep -q '@@released' '$P/holder.out'" 30 || bail "the primary transaction did not end"
sess_say writer "SELECT 'second ' || sift._test_wal_conflict('e', '$held');"
wait_for "grep -q '^second ' '$P/writer.out'" 30 || bail "the second conflict did not run"
catchup "$p" "$s" || bail "the standby did not replay the second conflict"
sess_say reader "SELECT 'after ' || count(*) FROM e; COMMIT; \\echo @@done"
wait_for "grep -q '@@done' '$S/reader.out'" 30
sess_close reader
sess_close writer
sess_close holder
out="held $held; $(cat "$P/writer.out" "$S/reader.out")"
check "the standby query's xmin is the transaction the primary held" grep -q "^xmin $held\$" <<<"$out"
check "the backend logs the conflict at the held transaction after a newer one" grep -q '^second true$' <<<"$out"
check "the standby cancels the query older than that conflict" bash -c '! grep -q "^after " <<<"$1" && grep -q "conflict with recovery" <<<"$1"' _ "$out"
finish
