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

plan 36
node_init uc 21
node_start uc || bail "start failed"
q postgres "CREATE DATABASE clean" "CREATE DATABASE cfg" "CREATE DATABASE old" >/dev/null

# the three access methods, a column index with JSON-free ints (format 4), one with a wide leaf (5) and one with the cube option (7)
q clean "CREATE EXTENSION pg_sift" \
	"CREATE TABLE docs (id int PRIMARY KEY, body text)" \
	"INSERT INTO docs SELECT g, 'alpha' || g % 7 || ' beta' || g % 11 FROM generate_series(1, 3000) g" \
	"CREATE INDEX docs_idx ON docs USING sift (body)" \
	"CREATE TABLE facts (id int, g int, n int, w interval)" \
	"INSERT INTO facts SELECT g, g % 9, g % 100, make_interval(secs => g) FROM generate_series(1, 3000) g" \
	"CREATE INDEX facts_col ON facts USING sift_col (id, g, n)" \
	"CREATE INDEX facts_wide ON facts USING sift_col (id, g, w)" \
	"CREATE INDEX facts_cube ON facts USING sift_col (id, g, n) WITH (cube_minmax = true)" \
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
check "to 3.0: a sift_table is a blocker and its advice rewrites it" grep -q -E '^blocker +tbl .*sift\.col_format_minor = 3, then ALTER TABLE tbl SET ACCESS METHOD' <<<"$out"
out=$(uc --to 3.1 clean); rc=$?
check "to 3.1: the index of format 4 and the BM25 index are ok" test -z "$(row "$out" blocker facts_col)$(row "$out" blocker docs_idx)"
check "to 3.1: the wide leaf (format 5) is a blocker that says to drop the index" grep -q -E '^blocker +facts_wide .*wide leaves.*drop the index' <<<"$out"
check "to 3.1: the cube option (format 7) is a blocker" test -n "$(row "$out" blocker facts_cube)"
out=$(uc --to 3.2 clean)
check "to 3.2: the wide leaf is ok and the cube option is a blocker with the setting 6" grep -q -E '^blocker +facts_cube .*sift\.col_format_minor = 6 and REINDEX INDEX facts_cube' <<<"$out" &&
	test -z "$(row "$out" blocker facts_wide)"
check "to 3.2: the month of a timestamp in a sift_table (format 8) is a blocker with the setting 6" grep -q -E '^blocker +months .*sift\.col_format_minor = 6, then ALTER TABLE months' <<<"$out"
check "to 3.3: nothing blocks" test "$(uc --to 3.3 clean | grep -c '^blocker')" == 0
check "a release that is not major.minor is refused" test "$(uc --to three clean 2>&1 | grep -c 'target must be a release')" == 1

# options that only the units written afterwards follow
q clean "CREATE INDEX facts_late ON facts USING sift_col (id, g, n)" "ALTER INDEX facts_late SET (cube_minmax = true)" \
	"CREATE INDEX facts_old_opt ON docs USING sift (body) WITH (mutable_limit = 1024)" >/dev/null
out=$(uc clean)
check "cube_minmax set after the build warns that the old units scan, with the REINDEX" grep -q -E '^warn +facts_late +option +cube_minmax with column format [0-9]+ .*REINDEX INDEX facts_late' <<<"$out"
check "the deprecated mutable_limit warns" grep -q -E '^warn +facts_old_opt +option +mutable_limit' <<<"$out"
check "a warning alone exits 0, and 1 with --strict" test "$(uc clean >/dev/null; echo $?)$(uc --strict clean >/dev/null; echo $?)" == 01
q clean "DROP INDEX facts_late, facts_old_opt" >/dev/null

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
