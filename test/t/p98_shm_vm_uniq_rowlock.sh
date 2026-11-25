#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# A transaction gives back its row locks when it ends without asking for memory: a writer that locked rows while no stripe had overflowed, and has not mapped the area of the chunks, commits or
# rolls back after another writer filled the stripes and the area cannot be attached (sift.test_dsm_fail = sift_rowlock_attach, as when /dev/shm is short). The end of a transaction that fails
# is a PANIC of the whole server. The setup is that of tbl_lock_overflow.sh, with the gates of two advisory locks: the first holds the writer that locked few rows before its end, the second
# the writer that locks more rows than the stripes take until the first has ended.
. "$(dirname "$0")/lib.bash"

out=
plan 5
node_init rl 1 "autovacuum = off
shared_preload_libraries = 'pg_sift'
sift.async_maintenance = off
sift.test_create_format1 = off
deadlock_timeout = 200ms"
node_start rl || bail "start failed"
D=$(dir rl)
sql rl "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE a (k int PRIMARY KEY, v int) USING sift_table" \
	"CREATE TABLE b (k int PRIMARY KEY, v int) USING sift_table" \
	"CREATE TABLE c (k int PRIMARY KEY, v int) USING sift_table" \
	"INSERT INTO a SELECT g, 0 FROM generate_series(1, 6000) g" \
	"INSERT INTO b SELECT g, 0 FROM generate_series(1, 6000) g" \
	"INSERT INTO c SELECT g, 0 FROM generate_series(1, 6000) g" >/dev/null || bail "setup failed"

stats() { sql rl "SELECT $1 FROM sift._test_rowlock_stats()"; }
bump() { echo "UPDATE $1 SET v = v + 1 WHERE k BETWEEN $2 AND $3"; }
waiting() { [[ $(sql rl "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted AND objid = $1") -ge 1 ]]; }
hold() {
	rm -f "$D/locked$1" "$D/release$1"
	{ echo "SELECT pg_advisory_lock($1);"; echo "\\! touch '$D/locked$1'"; echo "\\! while [ ! -e '$D/release$1' ]; do sleep 0.05; done"; } | sql rl >/dev/null &
	holders[$1]=$!
	wait_for "[[ -e $D/locked$1 ]]" 60 || bail "the holder did not take the gate $1"
}
release() { touch "$D/release$1"; wait "${holders[$1]}"; }

declare -A holders
rc=()
for end in COMMIT ROLLBACK; do
	hold 7001
	hold 7002
	sql rl "SET sift.test_dsm_fail = 'sift_rowlock_attach'" "BEGIN" "$(bump a 1 300)" "SELECT pg_advisory_lock(7001)" "$end" >"$D/first.out" 2>&1 &
	first=$!
	wait_for "waiting 7001" 60 || bail "the first writer did not reach its gate"
	sql rl "BEGIN" "$(bump a 3001 6000)" "$(bump b 1 3500)" "$(bump c 1 3500)" "SELECT pg_advisory_lock(7002)" "COMMIT" >"$D/second.out" 2>&1 &
	second=$!
	wait_for "waiting 7002" 60 || bail "the second writer did not reach its gate"
	[[ $(stats in_chunks) -gt 0 ]] || bail "the second writer did not fill a stripe"
	release 7001
	wait "$first"
	rc+=($?)
	release 7002
	wait "$second"
done
check "the writer that never mapped the area commits" test "${rc[0]}" -eq 0
check "and the one that rolls back" test "${rc[1]}" -eq 0 -a -z "$(grep -a 'ERROR\|server closed' "$D/first.out")"
check "no backend died" test "$(grep -ac 'PANIC\|terminated by signal' "$D/postgres.log")" -eq 0
out=$(sql rl "SELECT (SELECT sum(v) FROM a) || ' ' || (SELECT sum(v) FROM b) || ' ' || (SELECT sum(v) FROM c)")
check "the rows of the writer that committed are changed, nothing else" test "$out" == "6300 7000 7000"
out=$(stats "in_table || ' ' || in_chunks || ' ' || chunks")
check "the entries of the transactions that ended are given back" test "${out#* }" == "0 0"
finish
