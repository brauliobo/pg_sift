#!/usr/bin/env bash
# A transaction that fetches rows of a sift_table by address keeps the map of the column deltas it read for the next fetch, and uses it again while what it was made for holds. A READ COMMITTED
# reader is the one whose snapshot changes the least: with another transaction holding the oldest xmin and a later one that has completed, the xmin, the xmax and the address of the snapshot of
# its next statement are those of the last. What the map says changes in two ways, and the reader must see both: a writer appends a record to the head page of the chain and commits (on an UNLOGGED
# table the page never gets a LSN, so the append leaves the chain looking as it did), and a writer whose records the map skipped commits without writing anything.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "$(dirname "$0")/lib.bash"

plan 3
node_init dm 0 "autovacuum = off
sift.async_maintenance = off"
node_start dm || bail "start failed"
sql dm "CREATE EXTENSION pg_sift" >/dev/null || bail "setup failed"
F2=$([[ $(sql dm "SELECT count(*) FROM pg_settings WHERE name = 'sift.test_create_format1'") == 1 ]] && echo "SET sift.test_create_format1 = off" || echo "SELECT 1")

hold() { coproc "$1" { psql -X -q -At -h "$(node_sock dm)" -p "$(port dm)" -U postgres -d "$(node_db dm)" 2>&1; }; }
# ask SESSION STATEMENTS: what the session answers, up to the end marker
ask() {
	local -n in=$1[1] outp=$1[0]
	local line
	echo "$2; SELECT '--end';" >&"${in}"
	while read -r -t 20 line <&"${outp}" && [[ $line != --end ]]; do echo "$line"; done
}
hold PIN; hold WRITER; hold READER

# scenario KIND WRITES: a wide table of KIND (a few columns of it are a column delta) with a chain of one record; the reader fetches the row 10 of it by address before and after WRITES of a writer
# that commits after the reader's first fetch, the oldest xmin and the xmax of the snapshots held the same
scenario() {
	local kind=$1 writes=$2 addr
	sql dm "$F2" "DROP TABLE IF EXISTS t" "CREATE $kind TABLE t (id int PRIMARY KEY, v int, a int, b int, c int, d int, e int, f int, g int, pad text) USING sift_table" \
		"INSERT INTO t SELECT g, g, g, g, g, g, g, g, g, repeat('x', 20) FROM generate_series(1, 2000) g" "SELECT busy FROM sift.table_maintain('t')" \
		"UPDATE t SET a = -1 WHERE id = 20" >/dev/null || bail "setup of $kind failed"
	addr=$(sql dm "SELECT ctid FROM t WHERE id = 10")
	ask PIN "BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT pg_current_xact_id()" >/dev/null
	ask WRITER "BEGIN; SELECT pg_current_xact_id()" >/dev/null
	sql dm "SELECT pg_current_xact_id()" >/dev/null
	[[ $writes == before ]] && ask WRITER "UPDATE t SET v = 7777 WHERE id = 10; SELECT count(*) FROM t" >/dev/null
	ask READER "BEGIN ISOLATION LEVEL READ COMMITTED" >/dev/null
	before=$(ask READER "SELECT v FROM t WHERE ctid = '$addr'")
	[[ $writes == after ]] && ask WRITER "UPDATE t SET v = 7777 WHERE id = 10; SELECT count(*) FROM t" >/dev/null
	ask WRITER "COMMIT" >/dev/null
	after=$(ask READER "SELECT v FROM t WHERE ctid = '$addr'")
	ask READER "COMMIT" >/dev/null
	ask PIN "COMMIT" >/dev/null
	out="before=$before after=$after"
}

scenario "" after
check "LOGGED: a record appended to the chain after the first fetch shows in the next" test "$before" == 10 -a "$after" == 7777
scenario UNLOGGED after
check "UNLOGGED: a record appended to the chain after the first fetch shows in the next" test "$before" == 10 -a "$after" == 7777
scenario "" before
check "a writer that commits after the first fetch shows in the next, its records written before it" test "$before" == 10 -a "$after" == 7777
finish
