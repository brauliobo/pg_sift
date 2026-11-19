#!/usr/bin/env bash
# The adoption of the partitions of a key-cut index (col/maint/adopt.c) under SIGKILL: an index whose merge recorded no partitions (sift.test_col_record_partitions = off, as 5.0 wrote
# it) is adopted by one directory update, killed before it takes its locks, after it has written and logged the pages of the new chain and before the record that flips to them,
# and after the record. After crash recovery the directory is the one before the update or the one after it, never a part of it: no partition or all of them, the segments the same
# generations, units and bytes as before, every partition within its bounds, the index verifies and the adoption, run again, ends where a clean one does.
. "$(dirname "$0")/lib.bash"

POINTS=(before chain after)
BASE=("SET sift.test_col_segment_kb = 1024" "SET sift.col_auto_merge_partition_kb = 512" "SET max_parallel_maintenance_workers = 0" "SET sift.async_maintenance = off")
node_init ad 1 "$WAL_CONF
wal_consistency_checking = 'generic'
autovacuum = off"
node_start ad || bail "start failed"
SIFT_TMP=$(dir ad) leak_expect 400 'terminated by signal 9|terminating connection'
sql ad "CREATE EXTENSION pg_sift_test CASCADE" "CREATE TABLE c (id int PRIMARY KEY, dim int NOT NULL, day date NOT NULL, v int, w int) WITH (autovacuum_enabled = false)" \
	"INSERT INTO c SELECT g, 1 + (g::bigint * 7919 % 12)::int, date '2018-01-01' + (((g::bigint * 2654435761) % 4294967296 / 65536) % 2900)::int, g % 100, (g::bigint * 40503 % 1000003)::int FROM generate_series(1, 400000) g" \
	"VACUUM (FREEZE, ANALYZE) c" >/dev/null || bail "setup failed"

newindex() { sql ad "${BASE[@]}" "SET sift.test_col_record_partitions = off" "CREATE INDEX $1 ON c USING sift_col (id, dim, day, v, w) WITH (key_cut = 'dim, day')" >/dev/null || bail "create $1"; }
gens() { sql ad "SELECT coalesce(string_agg(gen || ':' || units || ':' || bytes || ':' || rows, ',' ORDER BY gen), '') FROM sift.col_segments('$1') WHERE kind = 'immutable'"; }
parts() { sql ad "SELECT count(*) FROM sift._test_dir_parts('$1')"; }
within() { sql ad "SELECT count(*) FILTER (WHERE rows_from_lo AND rows_below_next) || '/' || count(*) FROM sift._test_col_part_bounds('$1')"; }
answers() { sql ad "SET sift.enable_agg = $2" "SELECT string_agg(r::text, ';') FROM (SELECT dim, count(*), sum(v) FROM c GROUP BY 1 ORDER BY 1) r" | tail -n 1; }

newindex m
G=$(gens m)
adopted=$(sql ad "${BASE[@]}" "SELECT sift._test_col_adopt('m')" | tail -n 1)
N=$(parts m)
plan $((${#POINTS[@]} + 2))
check "a clean adoption records $N partitions, changes no segment, and every partition is within its bounds" \
	test "$adopted|$([[ $(gens m) == "$G" ]] && echo same)|$(within m)" == "t|same|$N/$N"
[[ $N -gt 4 ]] || { diag "$(sql ad "SELECT count(*), min(bytes), max(bytes) FROM sift.col_segments('m') WHERE kind = 'immutable'")"; bail "the fixture makes $N partitions"; }

case_no=0
for point in "${POINTS[@]}"; do
	case_no=$((case_no + 1))
	a=a$case_no
	newindex "$a"
	before=$(gens "$a")
	chain="SET sift.test_crash_chain = off"
	[[ $point != chain ]] || chain="SET sift.test_crash_chain = on"
	alive=1
	sql ad "${BASE[@]}" "$chain" "SET sift.test_crash_dir = $([[ $point == after ]] && echo -1 || echo 1)" "SET sift.test_crash_kind = 0" \
		"SELECT sift._test_col_adopt('$a')" >/dev/null 2>&1 || alive=0
	((alive == 1)) || wait_for "! kill -0 \$(head -1 $(dir ad)/data/postmaster.pid 2>/dev/null) 2>/dev/null" 60
	((alive == 1)) || node_start ad || bail "case $case_no: restart failed"
	state="$(parts "$a")|$([[ $(gens "$a") == "$before" ]] && echo same)|$(within "$a")"
	verified=$(sql ad "SELECT sift.col_verify('$a', true)" >/dev/null 2>&1 && echo t || echo f)
	again=$(sql ad "${BASE[@]}" "SELECT sift._test_col_adopt('$a')" 2>&1 | tail -n 1)
	final="$(parts "$a")|$([[ $(gens "$a") == "$before" ]] && echo same)|$(within "$a")|$([[ $(answers "$a" on) == "$(answers "$a" off)" ]] && echo heap)"
	out="state=$state verified=$verified again=$again final=$final"
	if [[ $point == after ]]; then wantstate="$N|same|$N/$N"; wantagain=f; else wantstate="0|same|0/0"; wantagain=t; fi
	check "$point the update: the directory is $([[ $point == after ]] && echo after || echo before) it, the index verifies, and the adoption run again ends with $N partitions" \
		test "$state|$verified|$again|$final" == "$wantstate|t|$wantagain|$N|same|$N/$N|heap"
done
check "the sequence ends where a clean run does" test "$(parts m)" == "$N"
finish
