#!/usr/bin/env bash
# The upgrade preflight (scripts/upgrade-check.sh, scripts/upgrade_check.sql): the findings for a clean database, for the formats a release could
# not read (--to), for options that wait for a REINDEX, for settings and test objects left behind, for an extension older than the library, for an
# index the library refuses, and that the check leaves nothing behind in the database.
. "$(dirname "$0")/lib.bash"
. "$(dirname "$0")/lib_twohop.bash"

REPO=$(cd "$(dirname "$0")/../.." && pwd)
q() { local db=$1; shift; PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir uc)/sock" -p "$(port uc)" -U postgres -d "$db" "${@/#/--command=}"; }
# uc [options] DATABASE...: the preflight against the node (exit status 0, 1 blocker, 2 not checked)
uc() { PGHOST=$(dir uc)/sock PGPORT=$(port uc) PGUSER=postgres "$REPO/scripts/upgrade-check.sh" "$@"; }
# row OUTPUT STATUS OBJECT: the finding line of an object with that status
row() { awk -v s="$2" -v o="$3" '$1 == s && $2 == o' <<<"$1"; }

plan 125
# the test clusters run with sift.table_auto_defrag and sift.col_auto_merge off (scripts/pgtest.sh conf); the preflight warns about a feature switched off in the
# server configuration, so this cluster keeps the shipped default and a clean database stays clean
node_init uc 21 "sift.table_auto_defrag = on
sift.col_auto_merge = on"
sed_i "/^sift\\.test_scope_patch = /d" "$(dir uc)/data/postgresql.conf"   # a setting of the test build the preflight would report: the clean database stays clean
node_start uc || bail "start failed"
q postgres "CREATE DATABASE clean" "CREATE DATABASE cfg" "CREATE DATABASE old" >/dev/null

# the three access methods, a column index with JSON-free ints (format 4), one with a wide leaf (5) and one with the cube option (7); cube_minmax and
# superunit are on by default since 3.3, so the older formats are written with both off
q clean "CREATE EXTENSION pg_sift" \
	"CREATE TABLE docs (id int PRIMARY KEY, body text)" \
	"INSERT INTO docs SELECT g, 'alpha' || g % 7 || ' beta' || g % 11 FROM generate_series(1, 3000) g" \
	"CREATE INDEX docs_idx ON docs USING sift (body)" \
	"CREATE TABLE facts (id int, g int, n int, w interval)" \
	"INSERT INTO facts SELECT g, g % 9, g % 100, make_interval(secs => g) FROM generate_series(1, 3000) g" \
	"CREATE INDEX facts_col ON facts USING sift_col (id, g, n) WITH (cube_minmax = false, superunit = 0)" \
	"CREATE INDEX facts_wide ON facts USING sift_col (id, g, w) WITH (cube_minmax = false, superunit = 0)" \
	"CREATE INDEX facts_cube ON facts USING sift_col (id, g, n) WITH (cube_minmax = true, superunit = 0)" \
	"CREATE TABLE tbl (id int, g int, n int) USING sift_table" \
	"INSERT INTO tbl SELECT g, g % 9, g % 100 FROM generate_series(1, 3000) g" "VACUUM tbl" \
	"CREATE TABLE months (id int, at timestamp) USING sift_table" \
	"INSERT INTO months SELECT g, timestamp '2026-01-01' + g * interval '1 hour' FROM generate_series(1, 3000) g" "VACUUM months" \
	"CREATE TABLE unwritten (id int) USING sift_table" >/dev/null || bail "setup failed"
objects=$(q clean "SELECT count(*) FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE a.amname LIKE 'sift%' AND c.relkind IN ('i', 'r')")
procs=$(q clean "SELECT count(*) FROM pg_proc WHERE pronamespace = 'sift'::regnamespace")
# VACUUM promotes the rows of a sift_table that are older than the oldest snapshot of its database: the maintenance worker this node runs (it starts at the commit of a
# CREATE INDEX) can hold one while the VACUUMs above run, and a table whose rows are still in the write buffer has no column format to report. Vacuum again until both report it.
for _ in $(seq 120); do
	out=$(uc clean)
	grep -q -E '^ok +tbl +table +format 1\.[0-9]+, column format' <<<"$out" && grep -q -E '^ok +months +table +format 1\.[0-9]+, column format' <<<"$out" && break
	q clean "VACUUM tbl" "VACUUM months" >/dev/null
	sleep 0.5
done

out=$(uc clean); rc=$?
check "a clean database exits 0" test $rc -eq 0
check "every finding is ok: the extension and each of the $objects objects" test "$(grep -c '^ok' <<<"$out")" == "$((objects + 1))"
check "no finding is a warning or a blocker" test "$(grep -c -E '^(warn|blocker)' <<<"$out")" == 0
check "a BM25 index reports its metapage format" grep -q -E '^ok +docs_idx +index +format 1\.[0-9]+ *$' <<<"$out"
check "a column index reports the metapage and its column format" grep -q -E '^ok +facts_col +index +format 1\.[0-9]+, column format [0-9]+' <<<"$out"
check "a table that was never written says so" grep -q -E '^ok +unwritten +table +nothing written yet' <<<"$out"
check "the check leaves no function in the database" test "$(q clean "SELECT count(*) FROM pg_proc WHERE pronamespace = 'sift'::regnamespace")" == "$procs"
check "the check leaves no sift.upgrade_check behind" test "$(q clean "SELECT to_regprocedure('sift.upgrade_check(text)') IS NULL")" == t

# what a release could not read: the first release that reads each format (docs/FORMATS.md)
out=$(uc --to 3.0 clean); rc=$?
check "to 3.0: exit 1" test $rc -eq 1
check "to 3.0: a packed BM25 segment is a blocker" test -n "$(row "$out" blocker docs_idx)"
check "to 3.0: a column index of format 4 is a blocker with the setting that writes what 3.0 reads" grep -q -E '^blocker +facts_col .*sift\.col_format_minor = 3 and REINDEX INDEX facts_col' <<<"$out"
check "to 3.0: a sift_table is a blocker and its advice rewrites it" grep -q -E '^blocker +tbl .*sift\.col_format_minor = 3, then VACUUM FULL tbl ' <<<"$out"
out=$(uc --to 3.1 clean); rc=$?
check "to 3.1: the column index of format 4 is ok and the BM25 index with position blocks (format 1.4) is a blocker" test -z "$(row "$out" blocker facts_col)" &&
	test -n "$(row "$out" blocker docs_idx)"
check "to 3.1: the wide leaf (format 5) is a blocker that says to drop the index" grep -q -E '^blocker +facts_wide .*wide leaves.*drop the index' <<<"$out"
check "to 3.1: the cube option (format 7) is a blocker" test -n "$(row "$out" blocker facts_cube)"
out=$(uc --to 3.2 clean)
check "to 3.2: the wide leaf is ok and the cube option is a blocker with the setting 6" grep -q -E '^blocker +facts_cube .*sift\.col_format_minor = 6 and REINDEX INDEX facts_cube' <<<"$out" &&
	test -z "$(row "$out" blocker facts_wide)"
check "to 3.2: the calendar cells of a timestamp in a sift_table (format 8) is a blocker with the setting 6" grep -q -E '^blocker +months .*sift\.col_format_minor = 6, then VACUUM FULL months ' <<<"$out"
check "to 3.2: the two event triggers of 3.3 are blockers that say to disable them" test "$(grep -cE '^blocker +sift_table_(ddl_check|repack_guard) +trigger .*ALTER EVENT TRIGGER sift_table_[a-z_]+ DISABLE' <<<"$out")" == 2
check "to 3.2: the two event triggers of 5.0 are blockers too, and they are all that are listed" test "$(grep -cE '^blocker +sift_col_(build_lock|value_store_sql_drop) +trigger .*ALTER EVENT TRIGGER sift_col_[a-z_]+ DISABLE' <<<"$out")/$(grep -c ' trigger ' <<<"$out")" == 2/4
out=$(uc --to 4.3 clean)
check "to 4.3: the event triggers of 5.0 are the blockers (CREATE INDEX and DROP INDEX fail with the older library), and no other trigger is listed" test "$(grep -cE '^blocker +sift_col_build_lock +trigger .*every CREATE INDEX' <<<"$out")$(grep -cE '^blocker +sift_col_value_store_sql_drop +trigger .*every DROP INDEX' <<<"$out")/$(grep -c ' trigger ' <<<"$out")" == 11/2
check "to 5.0: no event trigger is a blocker" test "$(uc --to 5.0 clean | grep -c ' trigger ')" == 0
q clean "ALTER EVENT TRIGGER sift_table_ddl_check DISABLE" "ALTER EVENT TRIGGER sift_table_repack_guard DISABLE" "ALTER EVENT TRIGGER sift_col_build_lock DISABLE" "ALTER EVENT TRIGGER sift_col_value_store_sql_drop DISABLE" >/dev/null
check "to 3.2: a disabled event trigger is not listed" test "$(uc --to 3.2 clean | grep -c ' trigger ')" == 0
q clean "ALTER EVENT TRIGGER sift_table_ddl_check ENABLE" "ALTER EVENT TRIGGER sift_table_repack_guard ENABLE" "ALTER EVENT TRIGGER sift_col_build_lock ENABLE" "ALTER EVENT TRIGGER sift_col_value_store_sql_drop ENABLE" >/dev/null
check "to 3.3: the BM25 index with position blocks is the one blocker but the event triggers of 5.0" test "$(uc --to 3.3 clean | awk '$1 == "blocker" && $3 != "trigger" {print $2}')" == docs_idx
check "to 3.7: nothing blocks but the event triggers of 5.0" test "$(uc --to 3.7 clean | grep '^blocker' | grep -vc ' trigger ')" == 0
check "a release that is not major.minor is refused" test "$(uc --to three clean 2>&1 | grep -c 'target must be a release')" == 1

# options that only the units written afterwards follow
q clean "CREATE INDEX facts_late ON facts USING sift_col (id, g, n) WITH (cube_minmax = false, superunit = 0)" "ALTER INDEX facts_late SET (cube_minmax = true)" \
	"CREATE INDEX facts_old_opt ON docs USING sift (body) WITH (mutable_limit = 1024)" \
	"CREATE INDEX facts_late_set ON facts USING sift_col (id, g, n) WITH (cube_minmax = false, superunit = 0)" "ALTER INDEX facts_late_set SET (key_columns = 'g')" \
	"CREATE INDEX facts_late_roll ON facts USING sift_col (id, g, n) WITH (cube_minmax = false, superunit = 0)" "ALTER INDEX facts_late_roll SET (rollup_dims = 'g, n')" \
	"CREATE INDEX facts_late_ord ON facts USING sift_col (id, g, n) WITH (cube_minmax = false, superunit = 0)" "ALTER INDEX facts_late_ord SET (order_columns = 'n')" \
	"CREATE TABLE scat (id int, k int, g int, n int)" "INSERT INTO scat SELECT i, i * 7919 % 100003, i % 9, i % 100 FROM generate_series(1, 3000) i" \
	"CREATE INDEX scat_new_opts ON scat USING sift_col (id, k, g, n) WITH (key_columns = 'k', rollup_dims = 'g, n')" >/dev/null
out=$(uc clean)
check "cube_minmax set after the build warns that the old units scan, with the REINDEX" grep -q -E '^warn +facts_late +option +cube_minmax with column format [0-9]+ .*REINDEX INDEX CONCURRENTLY public\.facts_late;' <<<"$out"
check "key_columns set after the build warns that the old units have no summary, with the REINDEX" grep -q -E '^warn +facts_late_set +option +key_columns with column format [0-9]+ .*REINDEX INDEX CONCURRENTLY public\.facts_late_set;' <<<"$out"
check "rollup_columns set after the build warns that the old units have no rollup of their rows, with the REINDEX" grep -q -E '^warn +facts_late_roll +option +rollup_columns with column format [0-9]+ .*REINDEX INDEX CONCURRENTLY public\.facts_late_roll;' <<<"$out"
check "order_columns (removed in 5.0, ignored by a build) warns to reset it, and no REINDEX is asked for it" grep -q -E '^warn +facts_late_ord +option +order_columns +removed in 5\.0 and ignored .*ALTER INDEX facts_late_ord RESET \(order_columns\)' <<<"$out"
check "an index built with key_columns and rollup_dims has no warning" test -z "$(grep -E '^(warn|blocker) +scat_new_opts ' <<<"$out")"
check "sift.upgrade_commands gives the same indexes, one command each, after the text index that queries need" test "$(q clean \
	"SELECT string_agg(kind || ' ' || required::text || ' ' || command, ' ' ORDER BY ord) FROM (SELECT *, row_number() OVER () ord FROM sift.upgrade_commands()) c WHERE kind = 'reindex'")" == \
	"reindex true REINDEX INDEX CONCURRENTLY public.docs_idx; reindex true REINDEX INDEX CONCURRENTLY public.facts_old_opt; reindex false REINDEX INDEX CONCURRENTLY public.facts_late; reindex false REINDEX INDEX CONCURRENTLY public.facts_late_roll; reindex false REINDEX INDEX CONCURRENTLY public.facts_late_set;"
# cube_minmax and superunit are on unless the index says otherwise: an index written before 3.3 lacks them, and a REINDEX gives them
q clean "SET sift.col_format_minor = 6" "CREATE INDEX facts_legacy ON facts USING sift_col (id, g, n)" >/dev/null
out=$(uc clean)
check "an index of an older format built with the defaults warns about cube_minmax and superunit, with the REINDEX" grep -q -E '^warn +facts_legacy +option +cube_minmax with column format [0-9]+ .*superunit with column format [0-9]+.*REINDEX INDEX CONCURRENTLY public\.facts_legacy;' <<<"$out"
q clean "REINDEX INDEX CONCURRENTLY public.facts_legacy" >/dev/null
check "after the REINDEX the index is not listed and is column format 12" test "$(q clean "SELECT count(*) FROM sift.upgrade_commands() WHERE object = 'facts_legacy'::regclass")$(q clean "SELECT string_agg(DISTINCT format::text, ',') FROM sift.col_segments('facts_legacy')")" == 012
q clean "DROP INDEX facts_legacy" >/dev/null
check "the deprecated mutable_limit warns" grep -q -E '^warn +facts_old_opt +option +mutable_limit' <<<"$out"
check "a warning alone exits 0, and 1 with --strict" test "$(uc clean >/dev/null; echo $?)$(uc --strict clean >/dev/null; echo $?)" == 01
# the sketches of hll_columns: set after the build, the old units lack them; built with them, the index is column format 20 and 3.3 cannot read it
q clean "CREATE INDEX facts_late_hll ON facts USING sift_col (id, g, n) WITH (cube_minmax = false, superunit = 0)" "ALTER INDEX facts_late_hll SET (hll_columns = 'n')" \
	"CREATE INDEX facts_hll ON facts USING sift_col (id, g, n) WITH (hll_columns = 'n')" >/dev/null
check "hll_columns set after the build warns that the old units have no sketch, with the REINDEX" grep -q -E '^warn +facts_late_hll +option +hll_columns with column format [0-9]+ .*REINDEX INDEX CONCURRENTLY public\.facts_late_hll;' <<<"$(uc clean)"
check "to 3.3: an index with sketches (format 20) is a blocker that says to reset hll_columns" grep -q -E '^blocker +facts_hll .*ALTER INDEX facts_hll RESET \(hll_columns\), then REINDEX INDEX facts_hll' <<<"$(uc --to 3.3 clean)"
q clean "DROP INDEX facts_late_hll, facts_hll" >/dev/null
# the global order of order_array_columns with no point directory codes its addresses with the bits of the largest offset of a chunk: column format 38, which 3.6 and older refuse
# (the setting 14 writes no array); 3.6 and 3.7 wrote it as column format 30, whose advice is to turn the option off
q clean "CREATE INDEX facts_gord ON facts USING sift_col (id, g, n) WITH (order_array_columns = 'id')" >/dev/null
check "an index with a global order is column format 38" grep -q -E '^ok +facts_gord +index +format 1\.[0-9]+, column format 38' <<<"$(uc clean)"
check "to 3.3: an index with a global order is a blocker" test -n "$(row "$(uc --to 3.3 clean)" blocker facts_gord)"
check "to 3.5: the same index is a blocker that says to write the setting 14 and that its arrays are left out" grep -q -E "^blocker +facts_gord .*SET sift.col_format_minor = 14, then REINDEX INDEX facts_gord.*written without its arrays: an ORDER BY \.\. LIMIT above 32 rows on those columns reads their units \(slower\) until a REINDEX at the normal setting writes them again" <<<"$(uc --to 3.5 clean)"
check "to 3.6: the same index is a blocker that says to write the setting 14 and that its arrays are left out" grep -q -E "^blocker +facts_gord .*SET sift.col_format_minor = 14, then REINDEX INDEX facts_gord.*written without its arrays: an ORDER BY \.\. LIMIT above 32 rows on those columns reads their units \(slower\) until a REINDEX at the normal setting writes them again" <<<"$(uc --to 3.6 clean)"
check "to 3.7: the same index is ok" test -z "$(row "$(uc --to 3.7 clean)" blocker facts_gord)"
q clean "SET sift.col_format_minor = 14" "REINDEX INDEX facts_gord" >/dev/null
check "to 3.6: the advice makes the index readable" test -z "$(row "$(uc --to 3.6 clean)" blocker facts_gord)"
check "... and it is written without the arrays: column format 12 (the setting 14 writes no array), and the preflight says the option waits for a REINDEX" grep -q -E '^warn +facts_gord +option +order_array_columns with column format 12 ' <<<"$(uc clean)"
q clean "REINDEX INDEX facts_gord" >/dev/null
check "a REINDEX at the normal setting writes the arrays again: column format 38" grep -q -E '^ok +facts_gord +index +format 1\.[0-9]+, column format 38' <<<"$(uc clean)"
q clean "DROP INDEX facts_gord" >/dev/null
# a compact global order (order_layout = 'compact') is column format 41, which 3.7 cannot read: the setting 14 and a REINDEX write what it reads; the whole order of the same index
# is what 3.7 wrote. The compact order needs a segment of many rows (a testing build lowers the threshold)
if [[ $(q clean "SELECT count(*) FROM pg_settings WHERE name = 'sift.test_col_gorder_ends'") == 1 ]]; then
	# plain chunks: the column format minors below tell the layouts apart (a default build writes seekable chunks, minor 44, checked after them)
	q clean "SET sift.test_limits = 'gord_seek_pct=0'" "SET sift.test_col_gorder_ends = 64" "CREATE TABLE gord_rows AS SELECT i AS id, TIMESTAMP '2026-01-01' + (hashint8extended(i, 7) & 35184372088831) * INTERVAL '1 microsecond' AS ts FROM generate_series(1, 20000) i" \
		"CREATE INDEX gord_whole ON gord_rows USING sift_col (id, ts) WITH (order_array_columns = 'ts', order_layout = 'whole')" \
		"CREATE INDEX gord_compact ON gord_rows USING sift_col (id, ts) WITH (order_array_columns = 'ts', order_layout = 'compact')" \
		"CREATE INDEX gord_auto ON gord_rows USING sift_col (id, ts) WITH (order_array_columns = 'ts')" \
		"CREATE TABLE gord_ties AS SELECT i AS id, DATE '2026-01-01' + (i * 31 % 40) AS day FROM generate_series(1, 20000) i" \
		"CREATE INDEX gord_auto_ties ON gord_ties USING sift_col (id, day) WITH (order_array_columns = 'day')" >/dev/null
	check "an index with a whole order is column format 38 to 40" grep -q -E '^ok +gord_whole +index +format 1\.[0-9]+, column format (3[89]|40)' <<<"$(uc clean)"
	check "an index with a compact order is column format 41" grep -q -E '^ok +gord_compact +index +format 1\.[0-9]+, column format 41' <<<"$(uc clean)"
	check "an index of order_layout auto whose keys are scattered keeps compact chunks: column format 41" grep -q -E '^ok +gord_auto +index +format 1\.[0-9]+, column format 41' <<<"$(uc clean)"
	check "and one whose keys tie much keeps the whole order: column format 38 to 40" grep -q -E '^ok +gord_auto_ties +index +format 1\.[0-9]+, column format (3[89]|40)' <<<"$(uc clean)"
	check "to 3.7: the compact order is a blocker that says to write the setting 14" grep -q -E "^blocker +gord_compact .*SET sift.col_format_minor = 14, then REINDEX INDEX gord_compact" <<<"$(uc --to 3.7 clean)"
	check "to 4.0: the compact order is ok" test -z "$(row "$(uc --to 4.0 clean)" blocker gord_compact)"
	q clean "SET sift.col_format_minor = 14" "REINDEX INDEX gord_compact" >/dev/null
	check "to 3.7: the advice makes the index readable" test -z "$(row "$(uc --to 3.7 clean)" blocker gord_compact)"
	q clean "SET sift.test_limits = 'gord_seek_pct=0'" "SET sift.test_col_gorder_ends = 64" "ALTER INDEX gord_whole SET (order_layout = 'compact')" "REINDEX INDEX gord_whole" >/dev/null
	check "order_layout = compact and a REINDEX write column format 41" grep -q -E '^ok +gord_whole +index +format 1\.[0-9]+, column format 41' <<<"$(uc clean)"
	q clean "SET sift.test_col_gorder_ends = 64" "CREATE INDEX gord_seek ON gord_rows USING sift_col (id, ts) WITH (order_array_columns = 'ts')" >/dev/null
	check "an index with seekable order chunks is column format 44" grep -q -E '^ok +gord_seek +index +format 1\.[0-9]+, column format 44' <<<"$(uc clean)"
	check "to 3.7: seekable order chunks are a blocker that says to write the setting 14" grep -q -E "^blocker +gord_seek .*SET sift.col_format_minor = 14, then REINDEX INDEX gord_seek" <<<"$(uc --to 3.7 clean)"
	q clean "DROP TABLE gord_rows, gord_ties" >/dev/null
else skip 10 "a release build has no test settings: the threshold of the compact order cannot be lowered"; fi
# the order array of a text column names its strings by their dictionary codes (column format 42, or 44 once its chunks are seekable); 3.7 reads the copy of the strings, so the setting 14 and a REINDEX write what it reads
q clean "CREATE TABLE gord_text AS SELECT i AS id, md5(i::text) AS name FROM generate_series(1, 3000) i" \
	"CREATE INDEX gord_text_i ON gord_text USING sift_col (id, name) WITH (order_array_columns = 'name')" >/dev/null
check "an index with the order array of a text column is column format 42 or 44 (seekable chunks)" grep -q -E '^ok +gord_text_i +index +format 1\.[0-9]+, column format (42|44)' <<<"$(uc clean)"
check "to 3.7: it is a blocker that says to write the setting 14" grep -q -E "^blocker +gord_text_i .*SET sift.col_format_minor = 14, then REINDEX INDEX gord_text_i" <<<"$(uc --to 3.7 clean)"
q clean "SET sift.col_format_minor = 14" "REINDEX INDEX gord_text_i" >/dev/null
check "to 3.7: the advice makes the index readable" test -z "$(row "$(uc --to 3.7 clean)" blocker gord_text_i)"
q clean "DROP TABLE gord_text" >/dev/null
# the key table of a segment is column format 22: 3.4 refuses it, an index written with key_table = off is column format 15; fat repeats below 1,024 strings, so no dictionary hash (column format 25) hides the 22
q clean "CREATE TABLE key_facts AS SELECT i AS id, i % 20 AS k, md5(i::text) || md5(i::text || 'a') AS pad, md5((i % 500)::text) || md5((i % 500)::text || 'a') AS fat FROM generate_series(1, 20000) i" \
	"CREATE INDEX facts_ktab ON key_facts USING sift_col (id, k, fat) WITH (cube_minmax = false, superunit = 0, key_columns = 'k')" \
	"CREATE INDEX facts_ktab_off ON key_facts USING sift_col (id, k, fat) WITH (cube_minmax = false, superunit = 0, key_columns = 'k', key_table = off)" >/dev/null
check "an index with key tables is column format 22" grep -q -E '^ok +facts_ktab +index +format 1\.[0-9]+, column format 22' <<<"$(uc clean)"
check "to 3.4: an index with key tables is a blocker that says to turn key_table off" grep -q -E '^blocker +facts_ktab .*ALTER INDEX facts_ktab SET \(key_table = off\), then REINDEX INDEX facts_ktab' <<<"$(uc --to 3.4 clean)"
check "to 3.4: the same index with key_table = off is ok" test -z "$(row "$(uc --to 3.4 clean)" blocker facts_ktab_off)"
# the units of a key-cut index (lists of runs among them) have the table first: column format 34, which 3.4 refuses; the setting 14 writes units by superblock
q clean "CREATE TABLE run_facts AS SELECT i AS id, 1 + (hashint4(i / 100) & 2147483647) % 400 AS set_id, i % 1000 AS v, repeat('x', 150) AS pad FROM generate_series(1, 90000) i" "ALTER TABLE run_facts ADD PRIMARY KEY (id)" \
	"CREATE INDEX facts_runs ON run_facts USING sift_col (id, set_id, v, pad) WITH (key_cut = 'set_id')" >/dev/null
check "an index with key-cut units and lists of runs is column format 34 or later" grep -q -E '^ok +facts_runs +index +format 1\.[0-9]+, column format (3[4-9]|[4-9][0-9])' <<<"$(uc clean)"
check "to 3.4: an index with key-cut units is a blocker that says to write the setting 14" grep -q -E "^blocker +facts_runs .*SET sift.col_format_minor = 14, then REINDEX INDEX facts_runs" <<<"$(uc --to 3.4 clean)"
q clean "SET sift.col_format_minor = 14" "REINDEX INDEX facts_runs" >/dev/null
check "to 3.4: the advice makes the index readable" test -z "$(row "$(uc --to 3.4 clean)" blocker facts_runs)"
q clean "DROP TABLE run_facts" >/dev/null
# a text dictionary read in place has row counts (count_columns) and a hash of its strings: column format 25, which 3.5 refuses too
q clean "CREATE INDEX facts_cnt ON key_facts USING sift_col (id, k, pad) WITH (cube_minmax = false, count_columns = 'pad')" >/dev/null
check "an index with row counts and a hash is column format 25 or later" grep -q -E '^ok +facts_cnt +index +format 1\.[0-9]+, column format (2[5-9]|[3-9][0-9])' <<<"$(uc clean)"
check "to 3.4: an index with a hash is a blocker that says to write the setting 14" grep -q -E "^blocker +facts_cnt .*SET sift.col_format_minor = 14, then REINDEX INDEX facts_cnt" <<<"$(uc --to 3.4 clean)"
q clean "DROP INDEX facts_cnt" >/dev/null
# a point directory is a packed table of entries: column format 35, which 3.6 refuses; the setting 14 writes the power of two of slots
q clean "CREATE INDEX facts_exact ON key_facts USING sift_col (id, k) WITH (cube_minmax = false, key_columns = 'off', point_tid_columns = 'id')" >/dev/null
check "an index with a point directory is column format 35 or later" grep -q -E '^ok +facts_exact +index +format 1\.[0-9]+, column format (3[5-9]|[4-9][0-9])' <<<"$(uc clean)"
check "to 3.6: an index with a point directory is a blocker that says to write the setting 14" grep -q -E "^blocker +facts_exact .*SET sift.col_format_minor = 14, then REINDEX INDEX facts_exact" <<<"$(uc --to 3.6 clean)"
check "... and an index with no order arrays is not told about arrays" test -z "$(row "$(uc --to 3.6 clean)" blocker facts_exact | grep "without its arrays")"
q clean "SET sift.col_format_minor = 14" "REINDEX INDEX facts_exact" >/dev/null
check "to 3.6: the advice makes the index readable" test -z "$(row "$(uc --to 3.6 clean)" blocker facts_exact)"
q clean "DROP INDEX facts_exact" >/dev/null
q clean "DROP TABLE key_facts" >/dev/null
# keys that spread over the range get a summary from the REINDEX, so the index leaves the list
q clean "CREATE INDEX scat_late ON scat USING sift_col (id, k, g, n) WITH (cube_minmax = false, superunit = 0)" "ALTER INDEX scat_late SET (key_columns = 'k')" >/dev/null
check "an index with an option set after its units is listed" test "$(q clean "SELECT count(*) FROM sift.upgrade_commands() WHERE object = 'scat_late'::regclass")" == 1
q clean "REINDEX INDEX CONCURRENTLY public.scat_late" >/dev/null
check "after the listed REINDEX the index is not listed" test "$(q clean "SELECT count(*) FROM sift.upgrade_commands() WHERE object = 'scat_late'::regclass")" == 0
# a text index is listed whatever its data (a word over 255 bytes is stored differently since 3.3), a keyword index never; a sift_table below the format of its schema
q clean "CREATE INDEX docs_kw ON docs USING sift (body) WITH (analyzer = keyword)" "SET sift.col_format_minor = 6" "CREATE TABLE months_old (id int, at timestamp) USING sift_table" \
	"INSERT INTO months_old SELECT g, timestamp '2026-01-01' + g * interval '1 hour' FROM generate_series(1, 3000) g" "VACUUM months_old" >/dev/null
check "a text index is required, a keyword index is not listed" test "$(q clean "SELECT string_agg(object::text, ',' ORDER BY object::text) FROM sift.upgrade_commands() WHERE required")" == docs_idx,facts_old_opt
check "a sift_table written below column format 8 is listed with a VACUUM FULL" test "$(q clean "SELECT kind || ' ' || command FROM sift.upgrade_commands() WHERE object = 'months_old'::regclass")" == "rewrite VACUUM FULL public.months_old;"
check "the table written by this library with a timestamp column is not listed" test "$(q clean "SELECT count(*) FROM sift.upgrade_commands() WHERE object = 'months'::regclass")" == 0
out=$(uc clean)
check "the preflight warns with the VACUUM FULL and its lock" grep -q -E '^warn +months_old +table +column format 4 .*ACCESS EXCLUSIVE.*VACUUM FULL public\.months_old;' <<<"$out"
check "the preflight does not warn about the text index" test -z "$(grep -E '^warn +docs_idx ' <<<"$out")"
q clean "VACUUM FULL months_old" >/dev/null
check "after the VACUUM FULL the table leaves the list" test "$(q clean "SELECT count(*) FROM sift.upgrade_commands() WHERE object = 'months_old'::regclass")" == 0
check "the table is in column format 8" test "$(q clean "SELECT string_agg(DISTINCT format::text, ',') FROM sift.table_segments('months_old') WHERE kind = 'columns'")" == 8
q clean "DROP INDEX facts_late, facts_old_opt, facts_late_set, facts_late_roll, scat_late, docs_kw" "DROP TABLE scat, months_old" >/dev/null

# settings and test objects outside the code. A new session has not loaded the library, so ALTER DATABASE takes a name it does not define.
q cfg "CREATE EXTENSION pg_sift" "CREATE TABLE t (id int)" >/dev/null
q postgres "ALTER DATABASE cfg SET sift.enable_scan = off" "ALTER DATABASE cfg SET sift.removed_setting = 1" \
	"ALTER ROLE postgres IN DATABASE cfg SET sift.col_format_minor = 4" >/dev/null 2>&1
removed=$?
out=$(uc cfg)
check "a feature switched off for a database warns" grep -q -E '^warn +sift\.enable_scan +setting +sift\.enable_scan = off, set by IN DATABASE cfg' <<<"$out"
check "a column format minor kept for a role warns" grep -q -E '^warn +sift\.col_format_minor +setting .*ALTER ROLE postgres IN DATABASE cfg' <<<"$out"
if ((removed == 0)); then
	check "a setting the library does not define warns" grep -q -E '^warn +sift\.removed_setting +setting .*does not define it' <<<"$out"
else skip 1 "the library is preloaded: ALTER DATABASE refuses a name it does not define"; fi
if [[ $(q cfg "SELECT sift.version() > ''" "SELECT count(*) FROM pg_settings WHERE name = 'sift.test_fail_merge'" | tail -n 1) == 1 ]]; then
	q postgres "ALTER DATABASE cfg SET sift.test_fail_merge = 2" >/dev/null
	check "a test-only setting warns" grep -q -E '^warn +sift\.test_fail_merge +test ' <<<"$(uc cfg)"
	q cfg "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null
	check "the test extension left behind warns" grep -q -E '^warn +pg_sift_test +test +extension installed' <<<"$(uc cfg)"
	q cfg "DROP EXTENSION pg_sift_test" >/dev/null
else skip 2 "a release build has no test settings or test extension"; fi
q postgres "ALTER DATABASE cfg RESET ALL" "ALTER ROLE postgres IN DATABASE cfg RESET ALL" >/dev/null
check "after RESET ALL the database is clean again" test "$(uc cfg | grep -c -E '^(warn|blocker)')" == 0

# an extension older than the library: the update is the next step, and the formats are read by the library that is installed
q old "CREATE EXTENSION pg_sift VERSION '3.1'" "CREATE TABLE docs (id int, body text)" \
	"INSERT INTO docs SELECT g, 'alpha' || g % 7 FROM generate_series(1, 2000) g" "CREATE INDEX docs_idx ON docs USING sift (body)" \
	"CREATE INDEX docs_col ON docs USING sift_col (id)" >/dev/null || bail "the 3.1 extension cannot be created"
out=$(uc old); rc=$?
check "an extension older than the library warns with the update" grep -q -E '^warn +pg_sift +version +extension 3\.1, library [0-9.]+ +ALTER EXTENSION pg_sift UPDATE' <<<"$out"
check "the indexes of the older SQL are read" test "$(grep -c -E '^ok +docs_(idx|col) ' <<<"$out")" == 2
check "an older extension exits 0, and 1 with --strict" test "$rc$(uc --strict old >/dev/null; echo $?)" == 01
q old "ALTER EXTENSION pg_sift UPDATE" >/dev/null
check "after the update the version is ok" grep -q -E '^ok +pg_sift +version +extension [0-9.]+, library [0-9.]+ *$' <<<"$(uc old)"
check "a database without pg_sift is skipped" grep -q 'no pg_sift, skipped' <<<"$(uc postgres)"
check "--all checks every database that has pg_sift" test "$(uc --all | grep -c '^== ')" == 4

# an index whose metapage minor this library does not read (patched to minor 9) is a blocker with the library's own hint
q clean "CREATE TABLE refused (id int, body text)" "INSERT INTO refused SELECT g, 'x' || g FROM generate_series(1, 500) g" \
	"CREATE INDEX refused_idx ON refused USING sift (body)" "CHECKPOINT" >/dev/null
file=$(dir uc)/data/$(q clean "SELECT pg_relation_filepath('refused_idx')")
node_stop uc
# the minor is the 16 bits after the 24-byte page header, the magic and the major; the page checksum is made again
checksums=$(pg_controldata "$(dir uc)/data" | awk -F: '/checksum version/ {gsub(/ /, "", $2); print $2}')
((checksums == 0)) || pg_checksums --disable -D "$(dir uc)/data" >/dev/null
printf '\x09\x00' | dd of="$file" bs=1 seek=30 conv=notrunc 2>/dev/null
((checksums == 0)) || pg_checksums --enable -D "$(dir uc)/data" >/dev/null
node_start uc || bail "restart failed"
out=$(uc clean); rc=$?
check "an index in a newer format is unreadable and blocks" grep -q -E '^blocker +refused_idx +index +unreadable +sift index "refused_idx" has on-disk format 1\.9.*REINDEX' <<<"$out"
check "the others are still checked, and the exit is 1" test $rc -eq 1 -a "$(grep -c '^ok' <<<"$out")" -ge "$objects"
# 4.2 writes two things that every release before it refuses: the buffer of a BM25 index that took an insert (format 1.6) and the boolean leaf of a jsonb column that may hold a NULL
# document; a release before 4.2 reads an index that has neither (a jsonb column with NOT NULL has no leaf)
q postgres "CREATE DATABASE v42" >/dev/null
q v42 "CREATE EXTENSION pg_sift" \
	"CREATE TABLE bm (id int, body text)" "INSERT INTO bm SELECT g, 'alpha w' || g % 50 FROM generate_series(1, 2000) g" "CREATE INDEX bm_idx ON bm USING sift (body)" \
	"INSERT INTO bm SELECT g, 'beta' FROM generate_series(2001, 2100) g" \
	"CREATE TABLE dp (id int PRIMARY KEY, doc jsonb)" "INSERT INTO dp SELECT g, CASE WHEN g % 7 = 0 THEN NULL ELSE jsonb_build_object('b', 'x' || g % 3) END FROM generate_series(1, 3000) g" \
	"CREATE INDEX dp_col ON dp USING sift_col (id, doc) WITH (paths = 'doc.b astext')" \
	"CREATE TABLE dn (id int PRIMARY KEY, doc jsonb NOT NULL)" "INSERT INTO dn SELECT g, jsonb_build_object('b', 'x' || g % 3) FROM generate_series(1, 3000) g" \
	"CREATE INDEX dn_col ON dn USING sift_col (id, doc) WITH (paths = 'doc.b astext')" >/dev/null || bail "setup of v42 failed"
out=$(uc --to 4.1 v42)
check "to 4.1: a BM25 index that took an insert (write buffer format 6, format 1.6) is a blocker" test -n "$(row "$out" blocker bm_idx)"
check "to 4.1: an index with the presence leaf is a blocker that says to REINDEX with the older release" grep -q -E '^blocker +dp_col .*REINDEX INDEX dp_col with 4\.1 ' <<<"$out"
check "to 4.1: the index of a NOT NULL jsonb column has no leaf and is ok" test -z "$(row "$out" blocker dn_col)"
check "to 4.2 nothing about the leaf blocks" test -z "$(row "$(uc --to 4.2 v42)" blocker dp_col)"
# 4.2 removed latest_hint, snapshot_masks, unit_rewrite and skip_trigger_lookup from sift.eval_methods: where a role, a database or the server file sets one, the check says so with the command that removes it
# (each q is a session of its own: the library is not loaded when the ALTER runs, which is why the older value is stored)
q postgres "CREATE ROLE ev_role" "CREATE DATABASE ev_db" >/dev/null
q postgres "ALTER ROLE ev_role SET sift.eval_methods = 'unit_rewrite'" "ALTER DATABASE ev_db SET sift.eval_methods = 'penalty_maintenance, -snapshot_masks'" >/dev/null
q postgres "ALTER ROLE ev_role IN DATABASE ev_db SET sift.eval_methods = 'skip_trigger_lookup'" "ALTER DATABASE v42 SET sift.eval_methods = 'penalty_maintenance'" >/dev/null
q postgres "ALTER SYSTEM SET sift.eval_methods = 'latest_hint'" >/dev/null
out=$(uc v42)
check "a role that sets a removed method is a blocker with the ALTER ROLE .. RESET that removes it" grep -q -E '^blocker +sift\.eval_methods .*set by ALTER ROLE ev_role .*ALTER ROLE ev_role RESET sift\.eval_methods' <<<"$out"
check "a database that sets one next to the method that stays is a blocker with the ALTER DATABASE .. RESET" grep -q -E '^blocker +sift\.eval_methods .*penalty_maintenance, -snapshot_masks, set by IN DATABASE ev_db .*ALTER DATABASE ev_db RESET sift\.eval_methods' <<<"$out"
check "a role in a database is one with ALTER ROLE .. IN DATABASE .. RESET" grep -q -E '^blocker +sift\.eval_methods .*ALTER ROLE ev_role IN DATABASE ev_db RESET sift\.eval_methods' <<<"$out"
check "a line of postgresql.auto.conf is one that says to ALTER SYSTEM RESET" grep -q -E '^blocker +sift\.eval_methods .*postgresql\.auto\.conf.*ALTER SYSTEM RESET sift\.eval_methods' <<<"$out"
check "four findings, and the method that stays alone (penalty_maintenance) is none" test "$(grep -c -E '^blocker +sift\.eval_methods' <<<"$out")" == 4
q postgres "ALTER SYSTEM RESET sift.eval_methods" "ALTER ROLE ev_role RESET sift.eval_methods" "ALTER DATABASE ev_db RESET sift.eval_methods" "ALTER ROLE ev_role IN DATABASE ev_db RESET sift.eval_methods" "ALTER DATABASE v42 RESET sift.eval_methods" >/dev/null
check "after the commands nothing is left" test "$(uc v42 | grep -c 'sift\.eval_methods')" == 0
# 4.2 removed sift.enable_col whole: the same list serves it, whatever the value, and the generic check of unknown settings does not repeat it as a warning
q postgres "ALTER ROLE ev_role SET sift.enable_col = off" "ALTER DATABASE ev_db SET sift.enable_col = off" "ALTER SYSTEM SET sift.enable_col = off" >/dev/null
q postgres "ALTER ROLE ev_role IN DATABASE ev_db SET sift.table_auto_defrag_dead_pct = 5" "ALTER DATABASE ev_db SET sift.col_auto_merge_dirty_pct = 10" >/dev/null
out=$(uc v42)
check "a removed setting of a role is a blocker with the ALTER ROLE .. RESET that removes it" grep -q -E '^blocker +sift\.enable_col .*set by ALTER ROLE ev_role .*ALTER ROLE ev_role RESET sift\.enable_col' <<<"$out"
check "a removed setting of a database is a blocker with the ALTER DATABASE .. RESET" grep -q -E '^blocker +sift\.enable_col .*set by IN DATABASE ev_db .*ALTER DATABASE ev_db RESET sift\.enable_col' <<<"$out"
check "a removed setting in postgresql.auto.conf is a blocker that says to ALTER SYSTEM RESET" grep -q -E '^blocker +sift\.enable_col .*postgresql\.auto\.conf.*ALTER SYSTEM RESET sift\.enable_col' <<<"$out"
check "the other removed setting is a blocker too, with its ALTER ROLE .. IN DATABASE .. RESET" grep -q -E '^blocker +sift\.table_auto_defrag_dead_pct .*ALTER ROLE ev_role IN DATABASE ev_db RESET sift\.table_auto_defrag_dead_pct' <<<"$out"
check "the rewrite rule setting 5.0 removed is a blocker too, with its ALTER DATABASE .. RESET" grep -q -E '^blocker +sift\.col_auto_merge_dirty_pct .*set by IN DATABASE ev_db .*ALTER DATABASE ev_db RESET sift\.col_auto_merge_dirty_pct' <<<"$out"
check "the removed setting is no warning of the generic check" test "$(grep -c -E '^warn +sift\.enable_col' <<<"$out")" == 0
q postgres "ALTER SYSTEM RESET sift.enable_col" "ALTER ROLE ev_role RESET sift.enable_col" "ALTER DATABASE ev_db RESET sift.enable_col" "ALTER ROLE ev_role IN DATABASE ev_db RESET sift.table_auto_defrag_dead_pct" "ALTER DATABASE ev_db RESET sift.col_auto_merge_dirty_pct" >/dev/null
check "after the commands nothing of it is left" test "$(uc v42 | grep -cE 'sift\.(enable_col|col_auto_merge_dirty_pct)')" == 0
# a scope is column format 45 and a scope of two paths (the grants of a member and of a group, and the memberships: three stamps in a segment) column format 49: 4.3 reads neither, 5.0 both
q postgres "CREATE DATABASE hop" "ALTER DATABASE hop SET sift.scope_max_multiplicity = 8" >/dev/null
printf '%s;\n' "CREATE EXTENSION pg_sift" "${TWOHOP_BASE[@]}" "${TWOHOP_INDEX[@]}" "CREATE TABLE one_pairs (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id))" "INSERT INTO one_pairs SELECT m, 1 + m % 12 FROM generate_series(1, 200) m" \
	"CREATE INDEX one_pairs_journal ON one_pairs USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid')" "CREATE TABLE one_events (id int PRIMARY KEY, dim_a_id int, v int)" \
	"INSERT INTO one_events SELECT i, 1 + i % 200, i % 10 FROM generate_series(1, 2000) i" \
	"CREATE INDEX one_col ON one_events USING sift_col (id, dim_a_id, v) WITH (scope = 'tenant_id: dim_a_id -> one_pairs(dim_a_id, tenant_id)')" "REINDEX INDEX one_col" |
	PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir uc)/sock" -p "$(port uc)" -U postgres -d hop -f - >/dev/null || bail "the scopes cannot be set up"
out=$(uc hop)
check "a scope of one path is column format 45, one of two paths is column format 49" grep -q -E '^ok +one_col +index +format 1\.[0-9]+, column format 45' <<<"$out" && grep -q -E '^ok +hop_col +index +format 1\.[0-9]+, column format 49' <<<"$out"
# a scope whose key is text, varchar or uuid is column format 50 (its units are cut by the codes of the key's value store): 4.3 and 5.0 builds before it refuse it, 5.0 reads it
printf '%s;\n' "CREATE TABLE txt_pairs (dim_a_id int NOT NULL, tenant_id text NOT NULL, PRIMARY KEY (dim_a_id, tenant_id))" "INSERT INTO txt_pairs SELECT m, 'tenant-' || (1 + m % 12) FROM generate_series(1, 200) m" \
	"CREATE INDEX txt_pairs_journal ON txt_pairs USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid')" \
	"CREATE INDEX txt_col ON one_events USING sift_col (id, dim_a_id, v) WITH (scope = 'tenant_id: dim_a_id -> txt_pairs(dim_a_id, tenant_id)')" "REINDEX INDEX txt_col" |
	PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir uc)/sock" -p "$(port uc)" -U postgres -d hop -f - >/dev/null || bail "the scope of a text key cannot be set up"
printf '%s;\n' "CREATE TABLE txm_pairs (acct text NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (acct, tenant_id))" "INSERT INTO txm_pairs SELECT 'acct-' || m, 1 + m % 12 FROM generate_series(1, 200) m" \
	"CREATE INDEX txm_pairs_journal ON txm_pairs USING sift_col (acct, tenant_id) WITH (track_changes = 'ctid')" \
	"CREATE TABLE txm_events (id int PRIMARY KEY, acct text, v int)" "INSERT INTO txm_events SELECT i, 'acct-' || (1 + i % 200), i FROM generate_series(1, 1000) i" \
	"CREATE INDEX txm_col ON txm_events USING sift_col (id, acct, v) WITH (scope = 'tenant_id: acct -> txm_pairs(acct, tenant_id)')" "REINDEX INDEX txm_col" |
	PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir uc)/sock" -p "$(port uc)" -U postgres -d hop -f - >/dev/null || bail "the scope of a text member cannot be set up"
out=$(uc hop)
check "a scope of a text member is column format 50" grep -q -E '^ok +txm_col +index +format 1\.[0-9]+, column format 50' <<<"$out"
check "a scope of a text key is column format 50" grep -q -E '^ok +txt_col +index +format 1\.[0-9]+, column format 50' <<<"$out"
out=$(uc --to 4.3 hop)
check "to 4.3: a scope of a text key is a blocker" test -n "$(row "$out" blocker txt_col)"
check "to 4.3: a scope of a text member is a blocker" test -n "$(row "$out" blocker txm_col)"
out=$(uc --to 5.0 hop)
check "to 5.0: a scope of a text key is no blocker" test -z "$(row "$out" blocker txt_col)"
check "to 5.0: a scope of a text member is no blocker" test -z "$(row "$out" blocker txm_col)"
out=$(uc --to 4.3 hop)
check "to 4.3: both are blockers" test -n "$(row "$out" blocker one_col)" -a -n "$(row "$out" blocker hop_col)"
check "to 4.3: the journal indexes and the grants of the scopes, which have no scope, are ok" test -z "$(row "$out" blocker grants_by_member)" -a -z "$(row "$out" blocker memberships_by_member)"
out=$(uc --to 5.0 hop)
check "to 5.0: neither is a blocker" test -z "$(row "$out" blocker one_col)" -a -z "$(row "$out" blocker hop_col)"
check "the preflight lists no REINDEX for the scope indexes or their journals" test "$(q hop "SELECT count(*) FROM sift.upgrade_commands()")" == 0
check "a scope of two paths the older library cannot read is a finding with an advice, not an error" test "$(uc --to 4.3 hop | grep -c -E '^blocker +hop_col .+')" == 1
check "the advice for a scope names RESET (scope), the setting 14 and the REINDEX" grep -q -E '^blocker +one_col .*ALTER INDEX one_col RESET \(scope\), then SET sift\.col_format_minor = 14 and REINDEX INDEX one_col ' <<<"$(uc --to 4.3 hop)"
# the setting alone cannot write a scope (it used to fail with "duplicate segment generation"): the build says so and names the RESET
err=$(q hop "SET sift.col_format_minor = 14" "REINDEX INDEX one_col" 2>&1)
check "the setting 14 with the scope on refuses the REINDEX and names the RESET" grep -q -E 'has a scope, which sift.col_format_minor 14 cannot write' <<<"$err"
# the advised commands, as written
q hop "ALTER INDEX one_col RESET (scope)" "SET sift.col_format_minor = 14" "REINDEX INDEX one_col" >/dev/null || bail "the advised commands for a scope fail"
out=$(uc --to 4.3 hop)
check "after the advice the index is column format 14" grep -q -E '^ok +one_col +index +format 1\.[0-9]+, column format 14' <<<"$out"
check "... and no blocker to 4.3" test -z "$(row "$out" blocker one_col)"
check "... and it answers like the table" test "$(q hop "SELECT sum(v) FROM one_events WHERE id < 500")" == "$(q hop "SET sift.enable_agg = off" "SELECT sum(v) FROM one_events WHERE id < 500")"
q hop "ALTER INDEX hop_col RESET (scope)" "SET sift.col_format_minor = 14" "REINDEX INDEX hop_col" >/dev/null || bail "the advised commands for a scope of two paths fail"
check "the same for a scope of two paths: no blocker to 4.3" test -z "$(row "$(uc --to 4.3 hop)" blocker hop_col)"
# the shared value store (column format 46) is read by 5.0 only: sift.col_value_store = off writes the strings in the segments again, which 4.3 reads, and a database that sets it off is warned
q postgres "CREATE DATABASE vst" >/dev/null
printf '%s;\n' "CREATE EXTENSION pg_sift" "SET sift.test_vstore_columns = 'uid,url'" "CREATE TABLE vs (id int PRIMARY KEY, tenant int, uid uuid, url text)" \
	"INSERT INTO vs SELECT i, i % 7, md5((i % 300)::text)::uuid, 'https://example.com/p/' || md5((i % 400)::text) FROM generate_series(1, 2000) i" \
	"CREATE INDEX vs_a ON vs USING sift_col (id, tenant, uid, url)" "CREATE INDEX vs_b ON vs USING sift_col (id, uid, url)" |
	PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir uc)/sock" -p "$(port uc)" -U postgres -d vst -f - >/dev/null || bail "the shared value store cannot be set up"
check "to 4.3: an index that reads its strings from the store is a blocker, and the advice is the setting and the REINDEX" grep -q -E '^blocker +vs_a .*SET sift\.col_value_store = off, then REINDEX INDEX vs_a ' <<<"$(uc --to 4.3 vst)"
q vst "SET sift.col_value_store = off" "REINDEX INDEX vs_a" "REINDEX INDEX vs_b" >/dev/null || bail "the advised commands for the store fail"
out=$(uc --to 4.3 vst)
check "after the advice no index of the table is a blocker to 4.3" test -z "$(row "$out" blocker vs_a)" -a -z "$(row "$out" blocker vs_b)"
q postgres "ALTER DATABASE vst SET sift.col_value_store = off" >/dev/null
check "a database that sets the store off is warned" grep -q -E '^warn +sift\.col_value_store +setting +sift\.col_value_store = off, set by IN DATABASE vst' <<<"$(uc vst)"
finish
