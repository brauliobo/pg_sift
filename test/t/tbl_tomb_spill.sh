#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The tombstone log of a sift_table is bounded by sift.tomb_log_limit as memory, never as a failure: past it the log is kept in sorted runs on temp files. A DELETE
# and an UPDATE of many more rows than the limit holds (256 entries at 16 kB) complete, with the same rows as the heap twin and as the same statements with the log in
# memory; a new backend reads them back (first look in chunks), through the b-tree too (HOT chains, whose members are spilled as well); VACUUM rewrites the spilled log; a
# statement that fails or a server that stops in the middle of a spilling statement leaves the table as it was.
. "$(dirname "$0")/lib.bash"

ROWS=$(knob SIFT_TS_ROWS 20000 6000)
node_init primary 2 "autovacuum = off"
node_start primary || bail "primary start failed"
D=$(dir primary)
sql primary "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE src AS SELECT g AS id, g % 500 AS grp, (g % 997)::numeric(10,2) AS amount, md5(g::text) AS pad FROM generate_series(1, $ROWS) g" \
	"CREATE TABLE ht (LIKE src)" "CREATE TABLE lm (LIKE src) USING sift_table" "CREATE TABLE ls (LIKE src) USING sift_table" \
	"CREATE INDEX lm_grp ON lm (grp)" "CREATE INDEX ls_grp ON ls (grp)" "CREATE INDEX ht_grp ON ht (grp)" >/dev/null || bail "setup failed"
plan 12

SMALL="SET sift.tomb_log_limit = 16"
BIG="SET sift.tomb_log_limit = 8388608"
digest() { sql primary "$2" "SELECT count(*) || ':' || md5(string_agg(id || ':' || grp || ':' || amount || ':' || pad, ',' ORDER BY id)) FROM $1" | tail -1; }
via_index() { sql primary "$2" "SET enable_seqscan = off" "SET enable_bitmapscan = off" "SELECT count(*) || ':' || sum(amount) FROM $1 WHERE grp BETWEEN 10 AND 40" | tail -1; }
for t in ht lm ls; do sql primary "INSERT INTO $t SELECT * FROM src" >/dev/null; done

# many more entries than the limit holds: the statements complete
out=$(sql primary "$SMALL" "UPDATE ls SET amount = amount + 1 WHERE id % 3 = 0" "DELETE FROM ls WHERE id % 7 = 0" "UPDATE ls SET grp = grp + 1 WHERE id % 11 = 0" 2>&1)
check "an UPDATE, a DELETE and an UPDATE of the key past a 16 kB limit complete" test -z "$out"
sql primary "$BIG" "UPDATE lm SET amount = amount + 1 WHERE id % 3 = 0" "DELETE FROM lm WHERE id % 7 = 0" "UPDATE lm SET grp = grp + 1 WHERE id % 11 = 0" >/dev/null
sql primary "UPDATE ht SET amount = amount + 1 WHERE id % 3 = 0" "DELETE FROM ht WHERE id % 7 = 0" "UPDATE ht SET grp = grp + 1 WHERE id % 11 = 0" >/dev/null
want=$(digest ht "SELECT 1")
check "the rows are the heap twin's, read by a new backend with the small limit" test "$(digest ls "$SMALL")" == "$want"
check "... and the same with the log in memory" test "$(digest lm "$BIG")" == "$want"
check "the b-tree reads (chain members) agree with the heap twin" test "$(via_index ls "$SMALL")" == "$(via_index ht "SELECT 1")"
check "a point read agrees" test "$(sql primary "$SMALL" "SELECT amount FROM ls WHERE id = 3")" == "$(sql primary "SELECT amount FROM ht WHERE id = 3")"

# a statement that fails in the middle, and one rolled back
out=$(sql primary "$SMALL" "BEGIN" "UPDATE ls SET amount = amount + 5" "SELECT 1 / 0" 2>&1)
check "a statement that fails after spilling leaves the rows as they were" test "$(digest ls "$SMALL")" == "$want"
sql primary "$SMALL" "BEGIN" "DELETE FROM ls WHERE id % 2 = 1" "ROLLBACK" >/dev/null
check "a rolled back DELETE leaves the rows as they were" test "$(digest ls "$SMALL")" == "$want"

# VACUUM rewrites the spilled log, and the log goes on working
sql primary "$SMALL" "VACUUM ls" >/dev/null
sql primary "VACUUM lm" >/dev/null
check "VACUUM with the small limit keeps the rows" test "$(digest ls "$SMALL")" == "$want"
sql primary "$SMALL" "UPDATE ls SET amount = amount + 2 WHERE id % 13 = 0" >/dev/null
sql primary "UPDATE ht SET amount = amount + 2 WHERE id % 13 = 0" >/dev/null
check "... and the next statements after the rewrite" test "$(digest ls "$SMALL")" == "$(digest ht "SELECT 1")"

# power loss in the middle of a spilling statement
sql primary "$SMALL" "UPDATE ls SET amount = amount + 3" >/dev/null 2>&1 &
UPD=$!
crash primary
wait "$UPD" 2>/dev/null
SIFT_TMP=$D leak_expect 1 'terminated by signal 9'
node_start primary || bail "restart after the crash failed"
a=$(digest ls "$SMALL")
check "after a power loss the table holds all of the statement or none of it" test "$a" == "$(digest ht 'SELECT 1')" -o "$a" == "$(sql primary "UPDATE ht SET amount = amount + 3" "SELECT 1" >/dev/null; digest ht 'SELECT 1')"
finish
