#!/usr/bin/env bash
# A sift_col index that left service on the primary is out of service on its hot standby (the flag of the metapage arrives with the WAL; the standby plans on the heap); damage that only
# the standby meets is kept in its own registry once a second read confirms it, and when the standby is promoted the registry is written to the metapage.
. "$(dirname "$0")/lib.bash"

plan 9
CONF="wal_level = replica
max_wal_senders = 5
autovacuum = off
synchronous_commit = on
sift.on_corruption = quarantine"
node_init p 70 "$CONF"
node_start p || bail "start failed"
sql p "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE a (id int, v int)" "INSERT INTO a SELECT i, i % 7 FROM generate_series(1, 20000) i" "CREATE INDEX a_idx ON a USING sift_col (id, v)" "ANALYZE a" \
	"CREATE TABLE b (id int, v int)" "INSERT INTO b SELECT i, i % 7 FROM generate_series(1, 20000) i" "CREATE INDEX b_idx ON b USING sift_col (id, v)" "ANALYZE b" >/dev/null || bail "setup failed"
# the damage of b is on the disk of the primary when the standby is cloned: the standby meets it, the primary has not yet
sql p "SELECT sift._test_poke('b_idx', 2, 8176, 9, 2)" "CHECKPOINT" >/dev/null || bail "poke failed"
node_clone p s 71 "sift.on_corruption = quarantine"
node_start s || bail "standby start failed"
catchup p s || bail "standby catchup failed"

uses_sift() { sql "$1" "EXPLAIN (COSTS OFF) SELECT count(*) FROM $2 WHERE v = 3" | grep -qE 'Sift|_idx'; }
persisted() { [[ $(sql "$1" "SELECT persisted FROM sift.quarantine WHERE index = '$2'::regclass") == "$3" ]]; }
check "before any damage is read the standby plans on the index" uses_sift s a

sql p "SELECT sift._test_poke('a_idx', 2, 8176, 9, 2)" >/dev/null
out=$(sql p "SELECT count(*) FROM a WHERE v = 3" 2>&1) || true
check "the primary's statement that meets the damage fails once" grep -q 'XX002\|corrupted' <<<"$out"
check "the worker flags the metapage of the primary" wait_for "persisted p a_idx t" 60
catchup p s || bail "standby catchup failed"
flagged_out() { persisted s a_idx t && ! uses_sift s a; }
check "the standby lists the index and plans without it" flagged_out
check "the standby answers from the heap as the primary does" test "$(sql s "SELECT count(*) FROM a WHERE v = 3")" == "$(sql p "SELECT count(*) FROM a WHERE v = 3")"

out=$(sql s "SELECT count(*) FROM b WHERE v = 3" 2>&1) || true
check "the standby meets the damage of b: the first read fails" grep -q 'corrupted' <<<"$out"
confirmed() { out=$(sql s "SELECT count(*) FROM b WHERE v = 3" 2>&1) || true; [[ $out == [0-9]* ]]; }
check "a second read, a second later, confirms it and the standby stops reading the index" wait_for confirmed 60
check "its registry holds b_idx: not persisted, the standby cannot write the metapage" persisted s b_idx f

node_promote s
promoted() { [[ $(sql s "SELECT pg_is_in_recovery()") == f ]]; }
writes_and_flags() { sql s "INSERT INTO b VALUES (20001, 1)" >/dev/null 2>&1 && wait_for "persisted s b_idx t" 60; }
check "the promoted standby accepts writes and flags the index in its metapage" wait_for promoted 60 && writes_and_flags
finish
