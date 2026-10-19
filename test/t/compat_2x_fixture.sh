#!/usr/bin/env bash
# Indexes written by the 2.x library stay readable and maintainable: the stored relation files of a small table and of
# its sift index (test/fixtures/compat2x, made by scripts/make-compat-fixture.sh with pg_sift 2.0.0: two immutable
# segments, one of them dirty with 150 dead documents, and 100 documents in the write buffer) are put in place of
# the files of an empty table and index created by the build under test. Then: sift.verify, the results the 2.x build
# gave (counts, sums, ranked ids, segments), the same counts from the heap, and further DML that works on the 2.x
# segments: more deletes with VACUUM, inserts with a promotion, a forced merge of everything into a new segment.
# COMPAT_FIXTURE names another fixture directory (compat_30_fixture.sh runs this test on the files of 3.0). The segments of the
# fixture are older than segment format 3: they are read as they are, a promotion writes a segment of format 3 next to them (and
# raises the metapage to 1.3), and a merge or VACUUM-driven rewrite leaves nothing but format 3.
. "$(dirname "$0")/lib.bash"

FIX=$(cd "$(dirname "$0")/../fixtures/${COMPAT_FIXTURE:-compat2x}" && pwd)
SEGMENTS="SELECT gen, kind, ndocs, dirty, dead_docs FROM sift.segments('docs_idx') ORDER BY gen"
fill() {
	sql fx "INSERT INTO docs SELECT g, concat_ws(' ', 'alpha' || g % 7, 'beta' || g % 13, 'w' || g % 50,
		CASE WHEN g % 3 = 0 THEN 'gamma' END, CASE WHEN g % 250 = 0 THEN 'rareterm' END, repeat('filler ', g % 5))
		FROM generate_series($1, $2) g"
}
# query => the heap predicate that selects the same rows (the body is a function of the id)
PAIRS=("alpha3 => id % 7 = 3" "beta5 gamma => id % 13 = 5 AND id % 3 = 0" "alpha3 NOT beta5 => id % 7 = 3 AND id % 13 <> 5"
	"gamma OR rareterm => id % 3 = 0 OR id % 250 = 0" "w1* => (id % 50)::text LIKE '1%'" "filler => id % 5 <> 0"
	"rareterm => id % 250 = 0" "\"alpha3 beta5\" => id % 7 = 3 AND id % 13 = 5")

# agree: count and sum(id) of every query through the index equal the ones of its predicate through the heap
agree() {
	local p q pred a h bad=()
	for p in "${PAIRS[@]}"; do
		q=${p%% => *} pred=${p#* => }
		a=$(sql fx "SET enable_seqscan = off" "SELECT count(*) || ':' || coalesce(sum(id), 0) FROM docs WHERE body @@> '$q'") || return 1
		h=$(sql fx "SET enable_indexscan = off" "SET enable_bitmapscan = off" "SET sift.enable_scan = off" "SET sift.enable_count = off" \
			"SELECT count(*) || ':' || coalesce(sum(id), 0) FROM docs WHERE $pred") || return 1
		[[ $a == "$h" ]] || bad+=("'$q' index $a heap $h")
	done
	(( ${#bad[@]} == 0 )) || { diag "${bad[*]}"; return 1; }
}
verify() { sql fx "SELECT sift.verify('docs_idx', true)" >/dev/null; }
# formats: the segment formats and the metapage minor of the index, e.g. "2,2/2"
formats_match() { [[ $(formats) =~ $1 ]]; }
formats() { sql fx "SELECT coalesce(string_agg(format::text, ',' ORDER BY gen) FILTER (WHERE kind = 'immutable'), '') || '/' ||
	coalesce(max(format) FILTER (WHERE kind = 'metapage minor'), 0) FROM sift._test_segment_formats('docs_idx')"; }

# same_results ACTUAL EXPECTED: equal files, except that the scores of the ranked lines (id|score) may differ by 0.01
same_results() {
	paste -d '#' "$1" "$2" | awk -F '#' '{
		na = split($1, a, "|"); ne = split($2, e, "|")
		if ($1 == $2) next
		if (na == 2 && ne == 2 && index(e[2], ".") && a[1] == e[1] && a[2] - e[2] < 0.01 && e[2] - a[2] < 0.01) next
		print "got " $1 " expected " $2; bad = 1 } END { exit bad }'
}

plan 11
node_init fx 3 "autovacuum = off
sift.async_maintenance = off"
node_start fx || bail "start failed"
sql fx "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test CASCADE" "CREATE TABLE docs (id int, body text)" \
	"CREATE INDEX docs_idx ON docs USING sift (body) WITH (vacuum_segments = 8, dead_rewrite_pct = 100)" >/dev/null || bail "setup failed"
# The pages of the fixture carry WAL positions of the 2.x cluster, and a checkpoint refuses to write a page (a free space map
# page, written without WAL) whose position this cluster has not reached yet: move the WAL past the fixture's.
wal_lsn=$(awk '$1 == "wal_lsn" { print $2 }' "$FIX/info.txt")
while [[ $(sql fx "SELECT pg_current_wal_insert_lsn() < '$wal_lsn'") == t ]]; do
	sql fx "SELECT pg_logical_emit_message(true, 'compat', 'x')" "SELECT pg_switch_wal()" >/dev/null || bail "switch_wal failed"
done
heap=$(sql fx "SELECT pg_relation_filepath('docs')")
index=$(sql fx "SELECT pg_relation_filepath('docs_idx')")
node_stop fx
data=$(dir fx)/data
cp "$FIX/heap" "$data/$heap"
cp "$FIX/heap_vm" "$data/${heap}_vm"
cp "$FIX/heap_fsm" "$data/${heap}_fsm"
cp "$FIX/index" "$data/$index"
node_start fx || bail "start with the fixture failed"

check "the sift index of the 2.x files is verified" verify
check "the old segments are of a format before 3 and the metapage was not raised" formats_match '^[12,]*/[12]$'
check "queries use the index" grep -q -i sift <(sql fx "SET enable_seqscan = off" "EXPLAIN SELECT count(*) FROM docs WHERE body @@> 'alpha3'")
sql fx "$SEGMENTS" >"$(dir fx)/segments.txt"
check "the segments are the ones of the 2.x build (two immutable, one dirty, and the write buffer)" \
	diff <(sed -n '2,4p' "$FIX/expected.txt") "$(dir fx)/segments.txt"
sql fx <"$FIX/../compat2x/queries.sql" >"$(dir fx)/results.txt" 2>"$(dir fx)/results.err"
check "the counts, sums, ranked ids and verify equal the 2.x results" same_results "$(dir fx)/results.txt" "$FIX/expected.txt"
check "the counts equal the heap's, the dead documents of the dirty segment stay out" agree

restarted() { verify && agree; }

# VACUUM marks documents dead in the dirty 2.x segment (150 more), in the clean one (140) and in the write buffer
vacuumed() {
	segments_are "1|immutable|1500|t|300" "4|immutable|1400|t|140" && verify && agree
}
# the segments listed, as the prefix of the lines of sift.segments
segments_are() {
	local s
	for s in "$@"; do grep -q -F "$s" "$(dir fx)/segments.txt" || { diag "no segment $s in $(tr '\n' ' ' <"$(dir fx)/segments.txt")"; return 1; }; done
}
# a promotion seals the 2.x write buffer together with the new rows into a segment of this build
promoted() {
	test "$(grep -c immutable "$(dir fx)/segments.txt")" -ge 3 && formats_match '^[12](,[12])*(,3)+/3$' && verify && agree
}
# the 2.x segments, dirty ones included, were merged into one segment written by this build
merged() {
	local rows
	rows=$(sql fx "SELECT count(*) FROM docs")
	test "$(grep -c immutable "$(dir fx)/segments.txt")" -eq 1 && grep -q "^[0-9]*|immutable|$rows|f|0$" "$(dir fx)/segments.txt" \
		&& formats_match '^3/3$' && verify && agree
}

sql fx "DELETE FROM docs WHERE id <= 1500 AND id % 10 = 7" "DELETE FROM docs WHERE id BETWEEN 1501 AND 2900 AND id % 10 = 9" \
	"DELETE FROM docs WHERE id BETWEEN 2950 AND 2959" "VACUUM docs" >/dev/null || bail "delete and vacuum failed"
sql fx "$SEGMENTS" >"$(dir fx)/segments.txt"
check "VACUUM marks more documents dead in the 2.x segments (segments, verify, counts)" vacuumed

fill 3001 3600 >/dev/null && sql fx "SELECT sift.promote('docs_idx')" >/dev/null || bail "insert and promote failed"
sql fx "$SEGMENTS" >"$(dir fx)/segments.txt"
check "the 2.x segments and a new one are read together (segments, verify, counts)" promoted

sql fx "SELECT sift.merge('docs_idx', true)" >/dev/null || bail "merge failed"
sql fx "$SEGMENTS" >"$(dir fx)/segments.txt"
check "a forced merge of the 2.x segments leaves one clean immutable segment of the live rows (segments, verify, counts)" merged

fill 3601 3610 >/dev/null && sql fx "SELECT sift.promote('docs_idx')" "VACUUM docs" >/dev/null || bail "more DML failed"
check "the merged segment takes more DML (verify, counts)" restarted


sql fx "CHECKPOINT" >/dev/null && node_stop fx && node_start fx || bail "restart failed"
check "the index survives a checkpoint and a restart (verify, counts)" restarted
finish
