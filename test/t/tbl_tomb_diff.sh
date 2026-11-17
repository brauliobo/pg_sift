#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# One workload of DELETE, UPDATE (of a column a b-tree reads: a HOT chain, and of its key), INSERT, VACUUM and reads, run on a heap table, on a sift_table with the
# tombstone log in memory (sift.tomb_log_limit 8 GB) and on one whose log lives in runs on temp files (64 kB, 1,024 entries): after every step the three give the same
# rows by a scan, by the b-tree and by an aggregate, and the store of the sift_tables verifies.
. "$(dirname "$0")/lib.bash"

ROWS=$(knob SIFT_TD_ROWS 20000 6000)
ROUNDS=$(knob SIFT_TD_ROUNDS 6 4)
node_init primary 2 "autovacuum = off
log_temp_files = 0"
node_start primary || bail "primary start failed"
D=$(dir primary)
sql primary "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE src AS SELECT g AS id, g % 500 AS grp, (g % 997)::numeric(10,2) AS amount, md5(g::text) AS pad FROM generate_series(1, $ROWS) g" \
	"CREATE TABLE ht (LIKE src)" "CREATE TABLE lm (LIKE src) USING sift_table" "CREATE TABLE ls (LIKE src) USING sift_table" >/dev/null || bail "setup failed"
for t in ht lm ls; do sql primary "INSERT INTO $t SELECT * FROM src" "CREATE INDEX ${t}_grp ON $t (grp)" "VACUUM $t" >/dev/null; done
plan $((ROUNDS * 3 + 2))

SET_ht="SET sift.tomb_log_limit = 8388608"
SET_lm="SET sift.tomb_log_limit = 8388608"
SET_ls="SET sift.tomb_log_limit = 64"
digest() { local s=SET_$1; sql primary "${!s}" "SELECT count(*) || ':' || md5(string_agg(id || ':' || grp || ':' || amount || ':' || pad, ',' ORDER BY id)) FROM $1" | tail -1; }
by_index() { local s=SET_$1; sql primary "${!s}" "SET enable_seqscan = off" "SET enable_bitmapscan = off" "SELECT count(*) || ':' || sum(amount) || ':' || max(id) FROM $1 WHERE grp BETWEEN 10 AND 60" | tail -1; }
agg() { local s=SET_$1; sql primary "${!s}" "SELECT grp, count(*), sum(amount) FROM $1 GROUP BY grp ORDER BY grp" | "$(dirname "$0")/../../scripts/sha256.sh" | cut -c1-32; }
same() { # LABEL
	local d=$(digest ht) i=$(by_index ht) a=$(agg ht) bad=
	for t in lm ls; do
		[[ $(digest $t) == "$d" ]] || bad+=" $t:scan"
		[[ $(by_index $t) == "$i" ]] || bad+=" $t:btree"
		[[ $(agg $t) == "$a" ]] || bad+=" $t:agg"
	done
	out=$bad
	check "$1: scan, b-tree and aggregate agree with the heap${bad:+ ($bad)}" test -z "$bad"
}

declare -A removed
for r in $(seq "$ROUNDS"); do
	for t in ht lm ls; do
		s=SET_$t
		sql primary "${!s}" "UPDATE $t SET amount = amount + $r WHERE id % $((r + 2)) = 0" "DELETE FROM $t WHERE id % $((r + 6)) = 1" \
			"UPDATE $t SET grp = (grp + $r) % 500 WHERE id % $((r + 9)) = 2" "INSERT INTO $t SELECT id + $((ROWS * r)), grp, amount, pad FROM src WHERE id % 7 = $r" >/dev/null || bail "round $r failed on $t"
	done
	same "round $r"
	sql primary "${SET_ht}" "VACUUM ht" >/dev/null || bail "vacuum failed on ht"
	for t in lm ls; do s=SET_$t; removed[$t]=$(sql primary "${!s}" "SET client_min_messages = info" "VACUUM (VERBOSE) $t" 2>&1 | sed -n 's/.*rows: \([0-9]*\) removed.*/\1/p'); done
	out="lm ${removed[lm]} ls ${removed[ls]}"
	check "round $r: VACUUM removes as many rows with the log in runs as with the log in memory" test -n "${removed[lm]}" -a "${removed[lm]}" == "${removed[ls]}"
	same "round $r after VACUUM"
done
check "the logs were kept in temp files" grep -q "temporary file" "$D/postgres.log"
check "the store verifies" test "$(sql primary "SELECT sift._test_check('lm') >= 0 AND sift._test_check('ls') >= 0")" == t
finish
