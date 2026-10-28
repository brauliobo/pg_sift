#!/usr/bin/env bash
# The upgrade preflight (scripts/upgrade-check.sh, scripts/upgrade_check.sql): the findings for a clean database, for the formats a release could
# not read (--to), for options that wait for a REINDEX, for settings and test objects left behind, for an extension older than the library, for an
# index the library refuses, and that the check leaves nothing behind in the database.
. "$(dirname "$0")/lib.bash"

REPO=$(cd "$(dirname "$0")/../.." && pwd)
q() { local db=$1; shift; PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir uc)/sock" -p "$(port uc)" -U postgres -d "$db" "${@/#/--command=}"; }
# uc [options] DATABASE...: the preflight against the node (exit status 0, 1 blocker, 2 not checked)
uc() { PGHOST=$(dir uc)/sock PGPORT=$(port uc) PGUSER=postgres "$REPO/scripts/upgrade-check.sh" "$@"; }
# row OUTPUT STATUS OBJECT: the finding line of an object with that status
row() { awk -v s="$2" -v o="$3" '$1 == s && $2 == o' <<<"$1"; }

plan 67
# the test clusters run with sift.table_auto_defrag and sift.col_auto_merge off (scripts/pgtest.sh conf); the preflight warns about a feature switched off in the
# server configuration, so this cluster keeps the shipped default and a clean database stays clean
node_init uc 21 "sift.table_auto_defrag = on
sift.col_auto_merge = on"
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
check "to 3.1: the index of format 4 and the BM25 index are ok" test -z "$(row "$out" blocker facts_col)$(row "$out" blocker docs_idx)"
check "to 3.1: the wide leaf (format 5) is a blocker that says to drop the index" grep -q -E '^blocker +facts_wide .*wide leaves.*drop the index' <<<"$out"
check "to 3.1: the cube option (format 7) is a blocker" test -n "$(row "$out" blocker facts_cube)"
out=$(uc --to 3.2 clean)
check "to 3.2: the wide leaf is ok and the cube option is a blocker with the setting 6" grep -q -E '^blocker +facts_cube .*sift\.col_format_minor = 6 and REINDEX INDEX facts_cube' <<<"$out" &&
	test -z "$(row "$out" blocker facts_wide)"
check "to 3.2: the calendar cells of a timestamp in a sift_table (format 8) is a blocker with the setting 6" grep -q -E '^blocker +months .*sift\.col_format_minor = 6, then VACUUM FULL months ' <<<"$out"
check "to 3.2: the two event triggers of 3.3, and no other, are blockers that say to disable them" test "$(grep -cE '^blocker +sift_table_(ddl_check|repack_guard) +trigger .*ALTER EVENT TRIGGER sift_table_[a-z_]+ DISABLE' <<<"$out")/$(grep -c ' trigger ' <<<"$out")" == 2/2
q clean "ALTER EVENT TRIGGER sift_table_ddl_check DISABLE" "ALTER EVENT TRIGGER sift_table_repack_guard DISABLE" >/dev/null
check "to 3.2: a disabled event trigger is not listed" test "$(uc --to 3.2 clean | grep -c ' trigger ')" == 0
q clean "ALTER EVENT TRIGGER sift_table_ddl_check ENABLE" "ALTER EVENT TRIGGER sift_table_repack_guard ENABLE" >/dev/null
check "to 3.3: nothing blocks" test "$(uc --to 3.3 clean | grep -c '^blocker')" == 0
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
check "rollup_dims set after the build warns that the old units have no rollup of their rows, with the REINDEX" grep -q -E '^warn +facts_late_roll +option +rollup_dims with column format [0-9]+ .*REINDEX INDEX CONCURRENTLY public\.facts_late_roll;' <<<"$out"
check "order_columns set after the build warns that the old units have no sorted run, with the REINDEX" grep -q -E '^warn +facts_late_ord +option +order_columns with column format [0-9]+ .*REINDEX INDEX CONCURRENTLY public\.facts_late_ord;' <<<"$out"
check "an index built with key_columns and rollup_dims has no warning" test -z "$(grep -E '^(warn|blocker) +scat_new_opts ' <<<"$out")"
check "sift.upgrade_commands gives the same indexes, one command each, after the text index that queries need" test "$(q clean \
	"SELECT string_agg(kind || ' ' || required::text || ' ' || command, ' ' ORDER BY ord) FROM (SELECT *, row_number() OVER () ord FROM sift.upgrade_commands()) c WHERE kind = 'reindex'")" == \
	"reindex true REINDEX INDEX CONCURRENTLY public.docs_idx; reindex true REINDEX INDEX CONCURRENTLY public.facts_old_opt; reindex false REINDEX INDEX CONCURRENTLY public.facts_late; reindex false REINDEX INDEX CONCURRENTLY public.facts_late_ord; reindex false REINDEX INDEX CONCURRENTLY public.facts_late_roll; reindex false REINDEX INDEX CONCURRENTLY public.facts_late_set;"
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
# the global order of order_array_columns is column format 30 (chunks of 512 entries; 21 with 1,024), which 3.5 cannot read
q clean "CREATE INDEX facts_gord ON facts USING sift_col (id, g, n) WITH (order_columns = 'id', order_array_columns = 'id')" >/dev/null
check "to 3.3: an index with a global order (format 30) is a blocker" test -n "$(row "$(uc --to 3.3 clean)" blocker facts_gord)"
check "to 3.5: the same index is a blocker that says to turn order_array_columns off" grep -q -E "^blocker +facts_gord .*ALTER INDEX facts_gord SET \(order_array_columns = 'off'\), then REINDEX INDEX facts_gord" <<<"$(uc --to 3.5 clean)"
check "to 3.6: the same index is ok" test -z "$(row "$(uc --to 3.6 clean)" blocker facts_gord)"
q clean "DROP INDEX facts_gord" >/dev/null
# the key table of a segment is column format 22: 3.4 refuses it, an index written with key_table = off is column format 15
q clean "CREATE TABLE key_facts AS SELECT i AS id, i % 20 AS k, md5(i::text) || md5(i::text || 'a') AS pad FROM generate_series(1, 20000) i" \
	"CREATE INDEX facts_ktab ON key_facts USING sift_col (id, k, pad) WITH (cube_minmax = false, superunit = 0, key_columns = 'k')" \
	"CREATE INDEX facts_ktab_off ON key_facts USING sift_col (id, k, pad) WITH (cube_minmax = false, superunit = 0, key_columns = 'k', key_table = off)" >/dev/null
check "an index with key tables is column format 22" grep -q -E '^ok +facts_ktab +index +format 1\.[0-9]+, column format 22' <<<"$(uc clean)"
check "to 3.4: an index with key tables is a blocker that says to turn key_table off" grep -q -E '^blocker +facts_ktab .*ALTER INDEX facts_ktab SET \(key_table = off\), then REINDEX INDEX facts_ktab' <<<"$(uc --to 3.4 clean)"
check "to 3.4: the same index with key_table = off is ok" test -z "$(row "$(uc --to 3.4 clean)" blocker facts_ktab_off)"
# a unit whose ids come in bursts keeps them as runs: column format 23, which 3.4 refuses; the advice turns off the lists and the tables
q clean "CREATE TABLE run_facts AS SELECT i AS id, 1 + (hashint4(i / 100) & 2147483647) % 400 AS set_id, i % 1000 AS v, repeat('x', 150) AS pad FROM generate_series(1, 90000) i" "ALTER TABLE run_facts ADD PRIMARY KEY (id)" \
	"CREATE INDEX facts_runs ON run_facts USING sift_col (id, set_id, v, pad) WITH (key_cut = 'set_id')" \
	"CREATE INDEX facts_runs_off ON run_facts USING sift_col (id, set_id, v, pad) WITH (key_cut = 'set_id', key_columns = 'off', key_table = off)" >/dev/null
check "an index with lists of runs is column format 23" grep -q -E '^ok +facts_runs +index +format 1\.[0-9]+, column format 23' <<<"$(uc clean)"
check "to 3.4: an index with lists of runs is a blocker that says to turn key_columns and key_table off" grep -q -E "^blocker +facts_runs .*ALTER INDEX facts_runs SET \(key_columns = 'off', key_table = off\), then REINDEX INDEX facts_runs" <<<"$(uc --to 3.4 clean)"
check "to 3.4: the same index with the lists off is ok" test -z "$(row "$(uc --to 3.4 clean)" blocker facts_runs_off)"
q clean "ALTER INDEX facts_runs SET (key_columns = 'off', key_table = off)" "REINDEX INDEX facts_runs" >/dev/null
check "to 3.4: the advice makes the index readable" test -z "$(row "$(uc --to 3.4 clean)" blocker facts_runs)"
q clean "DROP TABLE run_facts" >/dev/null
# a text dictionary read in place has row counts (count_columns) and a hash of its strings: column format 25, which 3.5 refuses too
q clean "CREATE INDEX facts_cnt ON key_facts USING sift_col (id, k, pad) WITH (cube_minmax = false, count_columns = 'pad')" >/dev/null
check "an index with row counts and a hash is column format 25" grep -q -E '^ok +facts_cnt +index +format 1\.[0-9]+, column format 25' <<<"$(uc clean)"
check "to 3.4: an index with a hash is a blocker that says to write the setting 14" grep -q -E "^blocker +facts_cnt .*SET sift.col_format_minor = 14, then REINDEX INDEX facts_cnt" <<<"$(uc --to 3.4 clean)"
q clean "DROP INDEX facts_cnt" >/dev/null
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
finish
