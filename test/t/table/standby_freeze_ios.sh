#!/usr/bin/env bash
# S119-29: a REPEATABLE READ snapshot of a hot standby without feedback reads a sift_table by a sequential scan, then the primary commits an insert and a delete the snapshot cannot
# see and runs VACUUM (FREEZE), which freezes the insertion and drops the deletion as every primary snapshot sees them. The replay must end the old snapshot with a snapshot conflict
# before it applies them: the index-only scan that follows in the same snapshot either answers what the sequential scan did or is cancelled, never one row more or less.
# The testing library pins format 1 (the release one builds format 2, which logs its conflicts in the deletion vector settle).
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 5
CONF="shared_preload_libraries = 'pg_sift'
wal_level = replica
max_wal_senders = 4
autovacuum = off"
[[ -z ${SIFT_TESTING:-} ]] || CONF+=$'\nsift.test_create_format1 = on'
node_init primary 0 "$CONF"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift" "CREATE TABLE t (id int8 PRIMARY KEY, grp int, v int8) USING sift_table" \
	"INSERT INTO t SELECT g, g % 64, g FROM generate_series(1, 1000) g" "CREATE INDEX t_cov ON t (grp) INCLUDE (v)" "VACUUM (FREEZE) t" "ANALYZE t" >/dev/null ||
	bail "setup failed"
node_clone primary standby 1 "hot_standby_feedback = off
max_standby_streaming_delay = 0"
node_start standby || bail "standby start failed"
synced() { sql primary "SELECT pg_current_xact_id()" >/dev/null && catchup primary standby; }
synced || bail "the standby did not catch up"
P=$(dir primary)
S=$(dir standby)
READ="SELECT string_agg(grp || '=' || c || '/' || s, ',' ORDER BY grp) FROM (SELECT grp, count(*) c, sum(v) s FROM t WHERE grp < 16 GROUP BY grp) x"
IOS="SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL sift.enable_agg = off"
SEQ="SET LOCAL enable_indexscan = off; SET LOCAL enable_indexonlyscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL sift.enable_agg = off"
CONFL="SELECT confl_snapshot FROM pg_stat_database_conflicts WHERE datname = 'postgres'"
plan_of="$(sql standby "BEGIN" "$IOS" "EXPLAIN (COSTS OFF) $READ" "COMMIT")"
check "the standby reads the covering index by an index-only scan" grep -q "Index Only Scan using t_cov" <<<"$plan_of"

sess_open primary writer
sess_say writer "BEGIN; INSERT INTO t VALUES (1001, 1, 5000); DELETE FROM t WHERE id = 2; \\echo @@written"
wait_for "grep -q '^@@written' '$P/writer.out'" 30 || bail "the primary writes did not run"
synced || bail "the standby did not replay the writes"
sess_open standby reader
sess_say reader "BEGIN ISOLATION LEVEL REPEATABLE READ; $SEQ; SELECT 'seq ' || ($READ);"
wait_for "grep -q '^seq ' '$S/reader.out'" 30 || bail "the standby snapshot was not taken"
before=$(sql standby "$CONFL")
sess_say writer "COMMIT; VACUUM (FREEZE) t; \\echo @@vacuumed"
wait_for "grep -q '^@@vacuumed' '$P/writer.out'" 30 || bail "the primary VACUUM did not run"
synced || bail "the standby did not replay the VACUUM"
sess_say reader "$IOS; SELECT 'ios ' || ($READ); COMMIT; \\echo @@done"
wait_for "grep -q '@@done' '$S/reader.out'" 30
sess_close reader
sess_close writer
out=$(cat "$S/reader.out")
seq=$(sed -n 's/^seq //p' <<<"$out")
ios=$(sed -n 's/^ios //p' <<<"$out")
check "the index-only scan of the snapshot answers what its sequential scan did, or is cancelled" bash -c '[[ -z $2 && $3 == *"conflict with recovery"* ]] || [[ $1 == "$2" ]]' _ \
	"$seq" "$ios" "$out"
check "the replay of the VACUUM ended the old snapshot with a snapshot conflict" test "$(sql standby "$CONFL")" -gt "$before"
check "a new sequential scan of the standby reads what the primary reads" test "$(sql standby "BEGIN" "$SEQ" "$READ" "COMMIT")" == "$(sql primary "$READ")"
check "and so does its index-only scan" test "$(sql standby "BEGIN" "$IOS" "$READ" "COMMIT")" == "$(sql primary "$READ")"
finish
