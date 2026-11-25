#!/usr/bin/env bash
# A sift_col index that left service on the primary is out of service on its hot standby (the flag of the metapage arrives with the WAL; the standby plans on the heap); damage that only
# the standby meets is raised at every read (a replay applies a record of several pages one page at a time, so what it sees may be a passing mix: it never takes an index out of service),
# and when the standby is promoted the damage is met again as on a primary: it leaves service and the metapage is flagged. Format 1 (a, b) and Format 2 (c, d): a and c are damaged on the
# primary after the clone, b and d before it.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 17
CONF="wal_level = replica
max_wal_senders = 5
autovacuum = off
synchronous_commit = on
sift.on_corruption = quarantine"
node_init p 70 "$CONF"
node_start p || bail "start failed"
declare -A FORMAT1=([a]=on [b]=on [c]=off [d]=off) PAGE=([a]=2 [b]=2 [c]=3 [d]=3)   # the page of the index that a damage goes to: Format 1 keeps its directory in block 2, Format 2 its first part in block 3
declare -A RAISED=([b]=corrupted [d]='recycled during the scan')   # what a standby says of it: Format 2 takes a page of the wrong type for one that was recycled under the scan
sql p "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "setup failed"
for t in a b c d; do
	sql p "SET sift.test_create_format1 = ${FORMAT1[$t]}" "CREATE TABLE $t (id int, v int)" "INSERT INTO $t SELECT i, i % 7 FROM generate_series(1, 20000) i" \
		"CREATE INDEX ${t}_idx ON $t USING sift_col (id, v)" "ANALYZE $t" >/dev/null || bail "setup of $t failed"
done
poke() { sql p "SELECT sift._test_poke('$1_idx', ${PAGE[$1]}, 8176, 9, 2)" "CHECKPOINT" >/dev/null || bail "poke of $1 failed"; }
# the damage of b and d is on the disk of the primary when the standby is cloned: the standby meets it, the primary has not yet
poke b
poke d
node_clone p s 71 "sift.on_corruption = quarantine"
node_start s || bail "standby start failed"
catchup p s || bail "standby catchup failed"

uses_sift() { sql "$1" "EXPLAIN (COSTS OFF) SELECT count(*) FROM $2 WHERE v = 3" | grep -qE "Sift|$2_idx"; }
persisted() { [[ $(sql "$1" "SELECT persisted FROM sift.quarantine WHERE index = '$2'::regclass") == "$3" ]]; }
for t in a c; do check "$t: before any damage is read the standby plans on the index" uses_sift s $t; done

for t in a c; do
	poke $t
	out=$(sql p "SELECT count(*) FROM $t WHERE v = 3" 2>&1) || true
	check "$t: the primary's statement that meets the damage fails once" grep -q 'XX002\|corrupted' <<<"$out"
	flagged() { sql p "SELECT count(*) FROM $t WHERE v = 3" >/dev/null 2>&1; persisted p ${t}_idx t; }
	check "$t: the worker flags the metapage of the primary" wait_for flagged 60
	catchup p s || bail "standby catchup failed"
	flagged_out() { persisted s ${t}_idx t && ! uses_sift s $t; }
	check "$t: the standby lists the index and plans without it" flagged_out
	check "$t: the standby answers from the heap as the primary does" test "$(sql s "SELECT count(*) FROM $t WHERE v = 3")" == "$(sql p "SELECT count(*) FROM $t WHERE v = 3")"
done

for u in b d; do
	out=$(sql s "SELECT count(*) FROM $u WHERE v = 3" 2>&1) || true
	check "$u: the standby meets the damage: the first read fails" grep -q "${RAISED[$u]}" <<<"$out"
done
sleep 2
for u in b d; do
	out=$(sql s "SELECT count(*) FROM $u WHERE v = 3" 2>&1) || true
	check "$u: a read a second later fails again: the standby does not take the index out of service" grep -q "${RAISED[$u]}" <<<"$out"
	check "$u: its registry does not hold ${u}_idx out of service" test "$(sql s "SELECT count(*) FROM sift.quarantine WHERE index = '${u}_idx'::regclass")" == 0
done

node_promote s
promoted() { [[ $(sql s "SELECT pg_is_in_recovery()") == f ]]; }
reads_and_flags() { local u; for u in b d; do sql s "SELECT count(*) FROM $u WHERE v = 3" >/dev/null 2>&1; wait_for "persisted s ${u}_idx t" 60 || return 1; done; }
check "the promoted standby meets the damage as a primary does and flags the indexes in their metapages" wait_for promoted 60 && reads_and_flags
finish
