#!/usr/bin/env bash
# The upgrade preflight of 6.0 (sift.upgrade_check, scripts/upgrade-check.sh): ok for what needs nothing, info for what is optional (a relation of Format Major 1, an option 6.0 no
# longer documents), warn for what to look at (an extension older than the library, the test extension), blocker for what stops the update (a removed setting, a view that depends
# on a function 6.0 recreates, an index the library refuses). --to RELEASE names what an older release cannot read: a relation of Format 2.0. The check leaves nothing behind.
. "$(dirname "$0")/lib.bash"

REPO=$(cd "$(dirname "$0")/../.." && pwd)
# QO: the options of the sessions of one q (QO="-c sift.test_create_format1=on" q db "CREATE INDEX ..." builds Format Major 1, as 5.1 did)
q() { local db=$1; shift; PGOPTIONS="-c client_min_messages=warning ${QO:-}" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir uc)/sock" -p "$(port uc)" -U postgres -d "$db" "${@/#/--command=}"; }
f1() { QO="-c sift.test_create_format1=on" q "$@"; }
# uc [options] DATABASE...: the preflight against the node (exit status 0, 1 blocker, 2 not checked)
uc() { PGHOST=$(dir uc)/sock PGPORT=$(port uc) PGUSER=postgres "$REPO/scripts/upgrade-check.sh" "$@" 2>/dev/null; }
# row OUTPUT STATUS OBJECT: the finding lines of an object with that status
row() { awk -v s="$2" -v o="$3" '$1 == s && $2 == o' <<<"$1"; }
findings() { grep -c -E "^($2)" <<<"$1"; }

# the test clusters run with sift.async_maintenance off and the settings of the test build (scripts/pgtest.sh conf): the preflight reports both, so this cluster keeps the shipped
# defaults and a clean database stays clean
node_init uc 21 "sift.async_maintenance = on"
sed_i "/^sift\\.test_/d;/^sift\\.col_build_source/d" "$(dir uc)/data/postgresql.conf"
node_start uc || bail "start failed"
q postgres "CREATE DATABASE clean" "CREATE DATABASE f1" "CREATE DATABASE cfg" "CREATE DATABASE old" >/dev/null

# Format 2.0, which is what CREATE INDEX and CREATE TABLE write
q clean "CREATE EXTENSION pg_sift" \
	"CREATE TABLE docs (id int PRIMARY KEY, body text)" \
	"INSERT INTO docs SELECT g, 'alpha' || g % 7 || ' beta' || g % 11 FROM generate_series(1, 3000) g" \
	"CREATE INDEX docs_idx ON docs USING sift (body)" \
	"CREATE TABLE facts (id int, g int, n int, w interval)" \
	"INSERT INTO facts SELECT g, g % 9, g % 100, make_interval(secs => g) FROM generate_series(1, 3000) g" \
	"CREATE INDEX facts_col ON facts USING sift_col (id, g, n)" \
	"CREATE TABLE tbl (id int, g int, n int) USING sift_table" \
	"INSERT INTO tbl SELECT g, g % 9, g % 100 FROM generate_series(1, 3000) g" "VACUUM tbl" \
	"CREATE TABLE unwritten (id int) USING sift_table" >/dev/null || bail "setup failed"
objects=$(q clean "SELECT count(*) FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE a.amname LIKE 'sift%' AND c.relkind IN ('i', 'r')")
procs=$(q clean "SELECT count(*) FROM pg_proc WHERE pronamespace = 'sift'::regnamespace")

out=$(uc clean); rc=$?
check "a clean database exits 0" test $rc -eq 0
check "every finding is ok: the extension and each of the $objects objects" test "$(findings "$out" ok)" == "$((objects + 1))"
check "no finding is a warning, a blocker or an info" test "$(findings "$out" 'warn|blocker|info|action')" == 0
check "a BM25 index reports its metapage format and that it needs nothing" grep -q -E '^ok +public\.docs_idx +index +format 1\.[0-9]+ +read and written as before' <<<"$out"
check "a column index reports Format 2.0" grep -q -E '^ok +public\.facts_col +index +format 2\.0 *$' <<<"$out"
check "a table reports Format 2.0, written or not" test "$(grep -c -E '^ok +public\.(tbl|unwritten) +table +format 2\.0 *$' <<<"$out")" == 2
check "the check leaves no function in the database" test "$(q clean "SELECT count(*) FROM pg_proc WHERE pronamespace = 'sift'::regnamespace")" == "$procs"
check "the installed sift.upgrade_check stays a member of the extension" test "$(q clean "SELECT count(*) FROM pg_depend WHERE refclassid = 'pg_extension'::regclass AND deptype = 'e' AND objid = 'sift.upgrade_check(text)'::regprocedure")" == 1

# what an older release cannot read: Format 2.0 (a relation of Format Major 1 and a BM25 index are read as before)
out=$(uc --to 5.1 clean); rc=$?
check "to 5.1: exit 1" test $rc -eq 1
check "to 5.1: a column index of Format 2.0 is a blocker that says to build it again with that release" grep -q -E '^blocker +public\.facts_col +index +format 2\.0 +5\.1 cannot read Format 2\.0: drop the index and build it again' <<<"$out"
check "to 5.1: a table of Format 2.0 is a blocker and its advice is SET ACCESS METHOD heap" grep -q -E '^blocker +public\.tbl +table +format 2\.0 +5\.1 cannot read Format 2\.0: ALTER TABLE public\.tbl SET ACCESS METHOD heap' <<<"$out"
check "to 5.1: a table never written is a blocker too" test -n "$(row "$out" blocker public.unwritten)"
check "to 5.1: the BM25 index is ok" test -n "$(row "$out" ok public.docs_idx)"
check "to 3.2: the same three are the blockers, and nothing else" test "$(findings "$(uc --to 3.2 clean)" blocker)/$(grep -c -E '^blocker +public\.(facts_col|tbl|unwritten) ' <<<"$(uc --to 3.2 clean)")" == 3/3
check "to 6.0: nothing blocks" test "$(findings "$(uc --to 6.0 clean)" blocker)" == 0
uc --to 6.x clean >/dev/null; rc=$?
check "a release that is not major.minor is not checked" test $rc -eq 2
q clean "ALTER TABLE public.unwritten SET ACCESS METHOD heap" >/dev/null
check "to 5.1: after the advice the table is not listed" test -z "$(row "$(uc --to 5.1 clean)" blocker public.unwritten)"

# Format Major 1 is read and written as before: info, with the optional step to Format 2.0
f1 f1 "CREATE EXTENSION pg_sift" \
	"CREATE TABLE docs (id int PRIMARY KEY, body text)" \
	"INSERT INTO docs SELECT g, 'alpha' || g % 7 || ' beta' || g % 11 FROM generate_series(1, 3000) g" \
	"CREATE INDEX docs_mut ON docs USING sift (body) WITH (mutable_limit = 128)" \
	"CREATE TABLE facts (id int, g int, n int)" \
	"INSERT INTO facts SELECT g, g % 9, g % 100 FROM generate_series(1, 3000) g" \
	"CREATE INDEX facts_col ON facts USING sift_col (id, g, n) WITH (cube_minmax = true, superunit = 0)" \
	"CREATE TABLE tbl (id int, g int, n int) USING sift_table" \
	"INSERT INTO tbl SELECT g, g % 9, g % 100 FROM generate_series(1, 3000) g" "VACUUM tbl" >/dev/null || bail "setup of f1 failed"
out=$(uc f1); rc=$?
check "Format Major 1 exits 0, and 0 with --strict: nothing in it is a warning or a blocker" test "$rc$(uc --strict f1 >/dev/null; echo $?)" == 00
check "a column index of Format Major 1 is info that says REINDEX upgrades it to Format 2.0" grep -q -E '^info +public\.facts_col +index +format 1\.[0-9]+ \(Format Major 1\) +Format 1\.x .*REINDEX INDEX CONCURRENTLY upgrades to Format 2\.0' <<<"$out"
check "a table of Format Major 1 is info that says the upgrade takes an ACCESS EXCLUSIVE lock" grep -q -E '^info +public\.tbl +table +format 1\.[0-9]+ \(Format Major 1\) +Format 1\.x .*ACCESS EXCLUSIVE' <<<"$out"
check "cube_minmax and superunit set after the build are info: deprecated, still read" test "$(grep -c -E '^info +public\.facts_col +option +(cube_minmax|superunit) +deprecated, still read as before' <<<"$out")" == 2
check "the deprecated mutable_limit is info" grep -q -E '^info +public\.docs_mut +option +mutable_limit +the deprecated name of write_buffer_limit' <<<"$out"
check "the BM25 index is ok" grep -q -E '^ok +public\.docs_mut +index +format 1\.[0-9]+ ' <<<"$out"
check "sift.upgrade_commands lists the REINDEX of the index and the upgrade of the table, none of them required" test "$(q f1 "SELECT string_agg(kind || ' ' || required::text, ' ' ORDER BY object::text) FROM sift.upgrade_commands() WHERE kind IN ('reindex', 'rewrite')")" == "reindex false rewrite false"
q f1 "REINDEX INDEX CONCURRENTLY public.facts_col" "SELECT * FROM sift.upgrade_table('public.tbl')" >/dev/null
out=$(uc f1)
check "after the REINDEX the index is Format 2.0" grep -q -E '^ok +public\.facts_col +index +format 2\.0 *$' <<<"$out"
check "after the upgrade the table is Format 2.0" grep -q -E '^ok +public\.tbl +table +format 2\.0 *$' <<<"$out"
out=$(q f1 "SELECT object, kind, command FROM sift.upgrade_commands() WHERE kind IN ('reindex', 'rewrite')")
check "no REINDEX or upgrade is listed any more" test -z "$out"
q f1 "ALTER INDEX facts_col RESET (cube_minmax, superunit)" >/dev/null
check "after RESET the options are not listed" test "$(grep -c -E '^info +public\.facts_col +option' <<<"$(uc f1)")" == 0
check "to 5.1: the upgraded relations are blockers again" test "$(grep -c -E '^blocker +public\.(facts_col|tbl) ' <<<"$(uc --to 5.1 f1)")" == 2

# settings, on the server, a database, a role and a role in a database: the ones 6.0 removes are blockers with the command that removes them. A new session has not loaded the library,
# so ALTER DATABASE and ALTER ROLE take names it does not define (the library warns about them when it loads)
q cfg "CREATE EXTENSION pg_sift" "CREATE TABLE t (id int)" "CREATE VIEW dv AS SELECT * FROM sift.refusal_slugs()" >/dev/null
q postgres "CREATE ROLE ev_role" >/dev/null
q postgres "ALTER DATABASE cfg SET sift.enable_scan = off" "ALTER DATABASE cfg SET sift.eval_methods = 'penalty_maintenance, -snapshot_masks'" \
	"ALTER ROLE ev_role SET sift.eval_methods = 'unit_rewrite'" "ALTER ROLE ev_role IN DATABASE cfg SET sift.col_format_minor = 4" \
	"ALTER ROLE ev_role IN DATABASE cfg SET sift.col_auto_merge = off" "ALTER DATABASE cfg SET sift.col_auto_merge_dirty_pct = 10" "ALTER SYSTEM SET sift.enable_col = off" >/dev/null 2>&1 || bail "settings"
out=$(uc cfg); rc=$?
check "a removed setting of a role is a blocker" grep -q -E '^blocker +sift\.eval_methods .*unit_rewrite, set by ALTER ROLE ev_role ' <<<"$out"
check "a removed setting of a database is a blocker" grep -q -E '^blocker +sift\.eval_methods .*penalty_maintenance, -snapshot_masks, set by IN DATABASE cfg ' <<<"$out"
check "a removed setting of a role in a database is a blocker" grep -q -E '^blocker +sift\.col_format_minor .*set by ALTER ROLE ev_role IN DATABASE cfg ' <<<"$out"
check "col_auto_merge says what replaces it" grep -q -E '^blocker +sift\.col_auto_merge .*use sift\.async_maintenance' <<<"$out"
check "a rewrite rule setting that 5.0 removed is a blocker" grep -q -E '^blocker +sift\.col_auto_merge_dirty_pct .*set by IN DATABASE cfg ' <<<"$out"
check "a removed setting in postgresql.auto.conf is a blocker" grep -q -E '^blocker +sift\.enable_col .*set by file .*postgresql\.auto\.conf' <<<"$out"
# the commands that remove them are the runbook of sift.migrate_plan
plan=$(q cfg "SELECT action_sql FROM sift.migrate_plan() WHERE phase = 'pre_guc' AND action_sql IS NOT NULL ORDER BY step")
check "the runbook resets a setting of a role with ALTER ROLE .. RESET" grep -qxF 'ALTER ROLE ev_role RESET sift.eval_methods;' <<<"$plan"
check "the runbook resets a setting of a database with ALTER DATABASE .. RESET" grep -qxF 'ALTER DATABASE cfg RESET sift.eval_methods;' <<<"$plan"
check "the runbook resets a setting of a role in a database with ALTER ROLE .. IN DATABASE .. RESET" grep -qxF 'ALTER ROLE ev_role IN DATABASE cfg RESET sift.col_format_minor;' <<<"$plan"
check "the runbook resets a setting of postgresql.auto.conf with ALTER SYSTEM RESET and reloads last" test "$(grep -xF -e 'ALTER SYSTEM RESET sift.enable_col;' -e 'SELECT pg_reload_conf();' <<<"$plan" | tr '\n' ' ')" == 'ALTER SYSTEM RESET sift.enable_col; SELECT pg_reload_conf(); '
check "a setting the library defines is no finding" test "$(grep -c 'sift\.enable_scan' <<<"$out")" == 0
check "a view that depends on a function 6.0 recreates is a blocker" grep -q -E '^blocker +public\.dv +dependency +depends on refusal_slugs\(\)' <<<"$out"
check "the settings and the view block: exit 1" test $rc -eq 1
q postgres "ALTER DATABASE cfg SET sift.test_fail_merge = 2" >/dev/null
check "a setting of the test build is a blocker of kind test" grep -q -E '^blocker +sift\.test_fail_merge +test ' <<<"$(uc cfg)"
q cfg "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null
check "the test extension left behind is a warning" grep -q -E '^warn +pg_sift_test +test +extension installed' <<<"$(uc cfg)"
q cfg "DROP EXTENSION pg_sift_test" "DROP VIEW dv" >/dev/null
q postgres "ALTER SYSTEM RESET sift.enable_col" "ALTER DATABASE cfg RESET ALL" "ALTER ROLE ev_role RESET ALL" "ALTER ROLE ev_role IN DATABASE cfg RESET ALL" >/dev/null
out=$(uc cfg); rc=$?
check "after the commands nothing is left: ok, and exit 0" test "$rc$(findings "$out" 'warn|blocker|info|action')" == 00

# an extension older than the library: the update is the next step, and the formats are read by the library that is installed
f1 old "CREATE EXTENSION pg_sift VERSION '5.1'" "CREATE TABLE docs (id int, body text)" \
	"INSERT INTO docs SELECT g, 'alpha' || g % 7 FROM generate_series(1, 2000) g" "CREATE INDEX docs_idx ON docs USING sift (body)" \
	"CREATE INDEX docs_col ON docs USING sift_col (id)" >/dev/null || bail "the 5.1 extension cannot be created"
out=$(uc old); rc=$?
check "an extension older than the library is a warning with the update" grep -q -E "^warn +pg_sift +version +extension 5\.1, library [0-9.]+ +ALTER EXTENSION pg_sift UPDATE TO '6\.0'" <<<"$out"
check "the indexes of the older SQL are read" test "$(grep -c -E '^(ok|info) +public\.docs_(idx|col) ' <<<"$out")" == 2
check "an older extension exits 0, and 1 with --strict" test "$rc$(uc --strict old >/dev/null; echo $?)" == 01
q old "ALTER EXTENSION pg_sift UPDATE" >/dev/null
check "after the update the version is ok" grep -q -E '^ok +pg_sift +version +extension [0-9.]+, library [0-9.]+ *$' <<<"$(uc old)"
check "a database without pg_sift is skipped" grep -q 'no pg_sift, skipped' <<<"$(uc postgres)"
check "--all checks every database" test "$(uc --all | grep -c '^== ')" == "$(q postgres "SELECT count(*) FROM pg_database WHERE datallowconn AND NOT datistemplate")"

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
check "an index in a newer format is unreadable and blocks" grep -q -E '^blocker +public\.refused_idx +index +unreadable +sift index "refused_idx" uses on-disk format 1\.9, newer than this pg_sift supports.*Upgrade the pg_sift library' <<<"$out"
check "the others are still checked, and the exit is 1" test $rc -eq 1 -a "$(findings "$out" ok)" -ge "$objects"
done_testing
finish
