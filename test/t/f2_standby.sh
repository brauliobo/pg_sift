#!/usr/bin/env bash
# A hot standby reads the Format 2 rows the primary has not flushed from their micro-slab yet: a small INSERT stays in the slab of its partition, and a standby may not write the part that
# would hold it ("cannot make new WAL entries during recovery"). Every way to read a sift_table and a heap table with a sift_col index runs on the standby, with one row in the slab, with
# more rows in its chain and with a part and a new slab, and the answers are those of the primary.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "$(dirname "$0")/lib.bash"
plan 10

p=f2s_primary
s=f2s_standby
F2="SET sift.test_create_format1 = off"
node_init "$p" 4 "wal_level = replica
max_wal_senders = 4
autovacuum = off
sift.async_maintenance = off"
node_start "$p" || bail "primary start failed"
sql "$p" "CREATE EXTENSION pg_sift" "$F2" \
	"CREATE TABLE t (id int PRIMARY KEY, n int, a int, b int, v text) USING sift_table" \
	"CREATE TABLE h (id int PRIMARY KEY, n int, a int, b int, v text)" \
	"CREATE INDEX h_col ON h USING sift_col (id, n, a)" \
	"INSERT INTO t VALUES (1, 10, 100, 1000, 'one')" "INSERT INTO h VALUES (1, 10, 100, 1000, 'one')" >/dev/null || bail "setup failed"
node_clone "$p" "$s" 5 "hot_standby_feedback = on"
node_start "$s" || bail "standby start failed"
check "the standby catches up" catchup "$p" "$s"

AGG_OFF="SET sift.enable_agg = off"
AGG_ON="SET sift.enable_agg = on"
INDEX_ONLY="SET enable_seqscan = off|SET enable_bitmapscan = off"
BITMAP_ONLY="SET enable_seqscan = off|SET enable_indexscan = off"
QS=("$AGG_OFF|SELECT * FROM t ORDER BY id"
	"$AGG_OFF|$INDEX_ONLY|SELECT * FROM t WHERE id = 1"
	"$AGG_OFF|$BITMAP_ONLY|SELECT * FROM t WHERE id = 1"
	"$AGG_OFF|SELECT id FROM t WHERE ctid = '(0,1)'"
	"$AGG_OFF|SELECT id FROM t WHERE ctid >= '(0,1)' AND ctid < '(1,0)' ORDER BY id"
	"$AGG_ON|SELECT count(*), sum(n), sum(a) FROM t"
	"$AGG_ON|SELECT n, count(*) FROM t GROUP BY n ORDER BY n"
	"$AGG_OFF|$INDEX_ONLY|SELECT id, n FROM t WHERE n = 10"
	"$AGG_ON|SELECT count(*), sum(n), sum(a) FROM h"
	"$AGG_ON|SELECT n, count(*) FROM h GROUP BY n ORDER BY n"
	"$AGG_OFF|$INDEX_ONLY|SELECT id, n, a FROM h WHERE n = 10 AND a = 100"
	"$AGG_OFF|SELECT count(*) FROM h WHERE a BETWEEN 1 AND 100000")
# answers NODE: one line for each of the queries, the error text instead of the rows of one that failed
answers() {
	local q parts
	for q in "${QS[@]}"; do
		IFS='|' read -ra parts <<<"$q"
		sql "$1" "${parts[@]}" 2>&1 | tr '\n' ' '
		echo
	done
}
agree() { # DESCRIPTION
	local pa sa
	pa=$(answers "$p"); sa=$(answers "$s")
	out=$sa
	check "$1: no query fails on the standby" test "$(grep -c ERROR <<<"$sa")" == 0
	check "$1: the standby answers what the primary does" test "$sa" == "$pa"
}
rows() { # TABLE FIRST LAST TAG: rows of the table, one statement
	echo "INSERT INTO $1 SELECT i, i % 7, i, i, '$4' FROM generate_series($2, $3) i"
}
agree "one row in the slab"
sql "$p" "$F2" "$(rows t 2 11 x)" "$(rows h 2 11 x)" >/dev/null || bail "second batch failed"
check "the standby catches up with the rows of a second slab batch" catchup "$p" "$s"
agree "eleven rows in the slab chain"
sql "$p" "$F2" "$(rows t 12 4000 y)" "SELECT busy FROM sift.table_maintain('t')" "$(rows h 12 4000 y)" \
	"INSERT INTO t VALUES (4001, 3, 4001, 4001, 'z')" "INSERT INTO h VALUES (4001, 3, 4001, 4001, 'z')" >/dev/null || bail "third batch failed"
check "the standby catches up with a flushed part and a new slab" catchup "$p" "$s"
agree "a part and a slab"
check "no WAL error in the standby log" test "$(grep -c 'cannot make new WAL entries' "$(dir "$s")/postgres.log")" == 0
finish
