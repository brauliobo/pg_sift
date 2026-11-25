#!/usr/bin/env bash
# A Format 2.0 write is several WAL records (the pages of a slab or a part, then the manifest that names them, then the extents a rewrite releases): a crash between two leaves blocks that
# belong to no list and no part. They are not corruption: after the restart sift.check is clean for a sift_table and a sift_col index, the rows are those of the committed transactions in every
# plan, VACUUM and the maintenance of the table go on, and the blocks are in use again (the file does not grow without bound over the cycles).
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "$(dirname "$0")/lib.bash"
CYCLES=$(knob SIFT_CRASH_CYCLES 6 3)
plan $((4 * CYCLES + 1))

p=f2o
F2="SET sift.test_create_format1 = off"
node_init "$p" 6 "autovacuum = off
sift.async_maintenance = off"
node_start "$p" || bail "start failed"
sql "$p" "CREATE EXTENSION pg_sift" "$F2" \
	"CREATE TABLE t (id bigserial PRIMARY KEY, n int, v text) USING sift_table" \
	"CREATE TABLE h (id bigserial PRIMARY KEY, n int, v text)" \
	"CREATE INDEX h_col ON h USING sift_col (id, n)" >/dev/null || bail "setup failed"

problems() { sql "$p" "SELECT problem FROM sift.check('t')" "SELECT problem FROM sift.check('h_col')" "SELECT problem FROM sift.check('h_col', true)" 2>&1; }
agree() { # the plans give the same rows
	local a b
	a=$(sql "$p" "SET sift.enable_agg = on" "SELECT count(*), coalesce(sum(n), 0) FROM t" "SELECT count(*), coalesce(sum(n), 0) FROM h" 2>&1)
	b=$(sql "$p" "SET sift.enable_agg = off" "SELECT count(*), coalesce(sum(n), 0) FROM t" "SELECT count(*), coalesce(sum(n), 0) FROM h" 2>&1)
	out="$a | $b"
	[[ $a == "$b" && $a != *ERROR* ]]
}
writer() { # statements of every size, forever: a micro-slab, a part, a flush, a rewrite
	while :; do
		sql "$p" "$F2" "INSERT INTO t (n, v) VALUES (1, 'a')" "INSERT INTO h (n, v) VALUES (1, 'a')" \
			"INSERT INTO t (n, v) SELECT i % 9, 'row' || i FROM generate_series(1, 300) i" "INSERT INTO h (n, v) SELECT i % 9, 'row' || i FROM generate_series(1, 300) i" \
			"UPDATE t SET n = n + 1 WHERE id % 11 = 0" "DELETE FROM t WHERE id % 13 = 0" "DELETE FROM h WHERE id % 13 = 0" \
			"SELECT busy FROM sift.table_maintain('t')" "VACUUM t" >/dev/null 2>&1 || return
	done
}

for c in $(seq "$CYCLES"); do
	writer &
	wpid=$!
	sleep "0.$((RANDOM % 9 + 3))"
	crash "$p"
	kill "$wpid" 2>/dev/null; wait "$wpid" 2>/dev/null
	node_start "$p" || bail "cycle $c: the restart failed"
	diag "cycle $c: $(sql "$p" "SELECT 't=' || count(*) FROM t" "SELECT 'h=' || count(*) FROM h" | tr '\n' ' ')"
	out=$(problems)
	check "cycle $c: sift.check is clean after the crash" test -z "$out"
	check "cycle $c: every plan gives the same rows" agree
	sql "$p" "VACUUM t" "VACUUM h" >/dev/null 2>&1
	check "cycle $c: VACUUM runs after the crash" test $? -eq 0
	out=$(problems)
	check "cycle $c: sift.check is clean after VACUUM" test -z "$out"
done
size=$(sql "$p" "SELECT pg_relation_size('t')")
check "the file of the table is bounded (it is $size bytes after $CYCLES crashes)" test "$size" -lt $((512 * 1024 * 1024))
finish
