#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The chunks of the row lock stripes of a sift_table (sift._test_rowlock_stats: chunks counts every chunk the area gave and got no back) outlive no transaction that needs them:
#   1 the committed updates a chunked stripe keeps for an open snapshot go when that snapshot ends, though the transaction that holds it touched no row of the stripes
#   2 a remove from the table of a full stripe takes the place of an entry of its chunks: a VACUUM (FREEZE) that clears the lockers of one table drains the chunks another writer still holds
#   3 a request that took a chunk for a full stripe and is cut short by an error before it used it gives the chunk back (an injection point, PostgreSQL with injection points only)
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 7
node_init lr 0 "autovacuum = off
sift.async_maintenance = off
sift.test_create_format1 = off"
node_start lr || bail "start failed"
sql lr "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE a (k int PRIMARY KEY, v int) USING sift_table" \
	"CREATE TABLE b (k int PRIMARY KEY, v int) USING sift_table" \
	"INSERT INTO a SELECT g, 0 FROM generate_series(1, 12000) g" \
	"INSERT INTO b SELECT g, 0 FROM generate_series(1, 12000) g" >/dev/null || bail "setup failed"
D=$(dir lr)
stats() { sql lr "SELECT $1 FROM sift._test_rowlock_stats()"; }
said() { wait_for "grep -q '^$2\$' $D/$1.out" 30 || bail "the session $1 did not reach $2"; }

sess_open lr rd
sess_say rd "SELECT k FROM a WHERE k = 12000 FOR KEY SHARE; BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) > 0 FROM a; \\echo snap"
said rd snap
sql lr "BEGIN" "UPDATE a SET v = v + 1 WHERE k <= 8400" "UPDATE b SET v = v + 1 WHERE k <= 8400" "COMMIT" >/dev/null || bail "the writer failed"
out=$(stats chunks)
check "the committed updates of 16,800 rows stay in chunks while a snapshot may wait on them" test "$out" -gt 0
sess_say rd "COMMIT; \\echo done"
said rd done
out=$(stats "in_chunks || ' ' || chunks")
check "the end of the snapshot gives the chunks back, though its transaction touched no row" test "$out" == "0 0"
sess_close rd

sess_open lr wa
sess_say wa "BEGIN; SELECT count(*) FROM (SELECT k FROM a WHERE k <= 8400 FOR UPDATE) s; \\echo a"
said wa a
sess_open lr wb
sess_say wb "BEGIN; SELECT count(*) FROM (SELECT k FROM b WHERE k <= 8400 FOR UPDATE) s; \\echo b"
said wb b
out=$(stats chunks)
check "two lockers of 8,400 rows each overflow the stripes" test "$out" -gt 0
sess_say wa "COMMIT; \\echo ca"
said wa ca
sql lr "VACUUM (FREEZE) a" >/dev/null || bail "vacuum failed"
out=$(stats "in_chunks || ' ' || chunks")
check "the lockers a VACUUM (FREEZE) clears make room for the entries in chunks: the chunks drain" test "$out" == "0 0"
sess_say wb "COMMIT; \\echo cb"
said wb cb
sess_close wa; sess_close wb

if pg_config --configure | grep -q -- --enable-injection-points; then
	sql lr "CREATE EXTENSION injection_points" >/dev/null || bail "injection_points failed"
	out=$(sql lr "SELECT injection_points_set_local()" "SELECT injection_points_attach('sift-rowlock-spare-taken', 'error')" "BEGIN" \
		"UPDATE a SET v = v + 1 WHERE k <= 8400" "UPDATE b SET v = v + 1 WHERE k <= 8400" "COMMIT" 2>&1)
	check "the request errors once it took a chunk" grep -q 'error triggered for injection point sift-rowlock-spare-taken' <<<"$out"
	out=$(stats "in_chunks || ' ' || chunks")
	check "the chunk of the request cut short goes back to the area" test "$out" == "0 0"
else
	skip 2 "PostgreSQL without injection points (scripts/pg-assert.sh --inject)"
fi
out=$(sql lr "SELECT sum(v) FROM a" "SELECT sum(v) FROM b" | paste -sd' ' -)
check "the rows are exact" test "$out" == "8400 8400"
finish
