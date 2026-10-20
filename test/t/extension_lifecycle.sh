#!/usr/bin/env bash
# Extension lifecycle with existing indexes: pg_dump | psql and pg_restore round trips, ALTER EXTENSION UPDATE
# along a multi-step upgrade path (the real pg_sift--1.0--1.1.sql, 1.1--1.2.sql, 1.2--3.0.sql, 3.0--3.1.sql and 3.1--3.2.sql in a private
# extension_control_path directory; 1.0 -> 3.0 and 1.2 -> 3.0 are compared with a fresh 3.0 install, 1.0 -> 3.2 and 3.1 -> 3.2 with a fresh 3.2 install), installation through extension_control_path and dynamic_library_path
# alone, and a same-major pg_upgrade that carries the index files over. Every step: sift.verify with
# heapallindexed and index results equal to a sequential scan.
. "$(dirname "$0")/lib.bash"

q() { local n=$1 db=$2; shift 2; sql_db "$n" "$db" "$@"; }
sql_db() {
	local n=$1 db=$2
	shift 2
	PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir "$n")/sock" \
		-p "$(port "$n")" -U postgres -d "$db" "${@/#/--command=}"
}
pgc() { local n=$1 tool=$2; shift 2; "$tool" -h "$(dir "$n")/sock" -p "$(port "$n")" -U postgres "$@"; }

# fp: results through the index, seq: the same queries through a sequential scan, ranked: scores
SETUP=$(cat <<'SQL'
CREATE EXTENSION pg_sift;
CREATE TABLE docs (id int PRIMARY KEY, body text);
INSERT INTO docs SELECT g, concat_ws(' ', 'alpha' || g % 7, 'beta' || g % 13, 'w' || g % 50, CASE WHEN g % 3 = 0 THEN 'gamma' END)
	FROM generate_series(1, 4000) g;
CREATE INDEX docs_idx ON docs USING sift (body) WITH (write_buffer_limit = 64);
INSERT INTO docs SELECT g, concat_ws(' ', 'alpha' || g % 7, 'late', 'w' || g % 50) FROM generate_series(4001, 4600) g;
UPDATE docs SET body = 'beta5 moved gamma' WHERE id % 97 = 0;
DELETE FROM docs WHERE id % 89 = 0;
CREATE FUNCTION run() RETURNS text LANGUAGE plpgsql AS $$
DECLARE q text; r text; o text := '';
BEGIN
	FOREACH q IN ARRAY ARRAY['alpha3', 'beta5 gamma', 'alpha3 NOT beta5', 'w1*', 'gamma OR late', 'moved'] LOOP
		EXECUTE 'SELECT count(*) || '':'' || coalesce(sum(id), 0) FROM docs WHERE body @@> $1' INTO r USING q;
		o := o || q || '=' || r || ';';
	END LOOP;
	RETURN o;
END $$;
CREATE FUNCTION fp() RETURNS text LANGUAGE sql SET enable_seqscan = off RETURN run();
CREATE FUNCTION seq() RETURNS text LANGUAGE sql
	SET enable_indexscan = off SET enable_bitmapscan = off SET sift.enable_scan = off SET sift.enable_count = off RETURN run();
CREATE FUNCTION ranked() RETURNS text LANGUAGE plpgsql SET enable_seqscan = off AS $$
DECLARE r record; o text := '';
BEGIN
	FOR r IN SELECT id, round(sift.score(ctid)::numeric, 4) s FROM docs WHERE body @@> 'alpha3 OR gamma'
		ORDER BY sift.score(ctid) DESC, id LIMIT 20 LOOP
		o := o || r.id || '@' || r.s || ',';
	END LOOP;
	RETURN o;
END $$;
SQL
)

# healthy NODE DB: the index verifies and agrees with a sequential scan
vq() { q "$1" "$2" "SELECT sift.verify('docs_idx', true)" "$3" | tail -n 1; }
healthy() { [[ $(vq "$1" "$2" "SELECT fp() = seq()") == t ]]; }

# the databases pg_upgrade carries over; upg (an update path run) needs extension_control_path
DBS="src plain custom"
(( PG_MAJOR < 18 )) || DBS+=" upg"
plan $((41 + $(wc -w <<<"$DBS")))
node_init life 6
node_start life || bail "start failed"
q life postgres "CREATE DATABASE src" >/dev/null
q life src "$SETUP" >/dev/null || bail "setup failed"
q life src "SELECT sift.promote('docs_idx') >= 0" >/dev/null
FP=$(q life src "SELECT fp()")
check "the source index verifies and agrees with a sequential scan" healthy life src
diag "$FP"

# dump and restore: the index is rebuilt from the table by the restore
q life postgres "CREATE DATABASE plain" "CREATE DATABASE custom" "CREATE DATABASE schemaonly" >/dev/null
pgc life pg_dump src | pgc life psql -X -q -v ON_ERROR_STOP=1 -d plain >/dev/null 2>"$(dir life)/plain.err"
check "plain pg_dump restores into psql" test $? -eq 0
check "the plain restore verifies and returns the same results" test "$(vq life plain "SELECT fp() = seq()")" == t
check "the plain restore matches the source" test "$(q life plain "SELECT fp()")" == "$FP"
pgc life pg_dump -Fc src >"$(dir life)/src.dump"
pgc life pg_restore -d custom "$(dir life)/src.dump"
check "custom format pg_restore succeeds" test $? -eq 0
check "the custom restore verifies and matches the source" test "$(vq life custom "SELECT fp() = '$FP'")" == t
pgc life pg_dump --schema-only src | pgc life psql -X -q -v ON_ERROR_STOP=1 -d schemaonly >/dev/null
check "the index definition and options survive a schema-only dump" test "$(q life schemaonly \
	"SELECT indexdef FROM pg_indexes WHERE indexname = 'docs_idx'")" == "CREATE INDEX docs_idx ON public.docs USING sift (body) WITH (write_buffer_limit='64')"
q life schemaonly "INSERT INTO docs VALUES (1, 'alpha3 hello')" >/dev/null
check "the empty restored index accepts new documents" test "$(vq life schemaonly "SELECT fp() = seq()")" == t
check "pg_dump keeps the extension as one object" test "$(pgc life pg_dump src | grep -c 'CREATE EXTENSION IF NOT EXISTS pg_sift')" == 1

# extension_control_path (new in PostgreSQL 18) and dynamic_library_path are the only install, and the update path runs in a private directory
if (( PG_MAJOR >= 18 )); then
	# extension_control_path and dynamic_library_path are the only install: nothing else makes sift available
	check "sift is available through extension_control_path" test "$(q life postgres \
		"SELECT default_version FROM pg_available_extensions WHERE name = 'pg_sift'")" == 3.2
	check "the library loads through dynamic_library_path" test "$(vq life src \
		"SELECT count(*) FROM pg_get_loaded_modules() WHERE module_name = 'pg_sift'")" == 1
	check "without the path the extension is not found" test "$(q life postgres "SET extension_control_path = '/nonexistent'" \
		"SELECT count(*) FROM pg_available_extensions WHERE name = 'pg_sift'")" == 0

	# upgrade path: the real scripts of every version
	UPD=$(dir life)/upd
	mkdir -p "$UPD/extension"
	cp "$SHAREDIR"/extension/pg_sift--{1.0,1.1,1.2,3.0,3.1,3.2}.sql "$SHAREDIR"/extension/pg_sift--{1.0--1.1,1.1--1.2,1.2--3.0,3.0--3.1,3.1--3.2}.sql "$UPD/extension/"
	cp "$SHAREDIR/extension/pg_sift.control" "$UPD/extension/pg_sift.control"
	PATHSET="SET extension_control_path = '$UPD:\$system'"
	q life postgres "CREATE DATABASE upg" >/dev/null
	q life upg "$PATHSET" "CREATE EXTENSION pg_sift VERSION '1.0'" >/dev/null
	# the index of the upgraded database is built as a 3.0 library built it (segment format 2): the update has to leave it alone
	q life upg "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null
	q life upg "SET sift.test_segfmt = 2" "${SETUP#*CREATE EXTENSION pg_sift;}" >/dev/null || bail "upgrade setup failed"
	formats() { q life upg "SELECT coalesce(string_agg(format::text, ',' ORDER BY gen) FILTER (WHERE kind = 'immutable'), '') FROM sift._test_segment_formats('docs_idx')"; }
	check "the index built before the update has a segment of format 2" test "$(formats)" == 2
	before=$(q life upg "SELECT fp() || ranked()")
	check "the update path chains the five steps" test "$(q life upg "$PATHSET" \
		"SELECT path FROM pg_extension_update_paths('pg_sift') WHERE source = '1.0' AND target = '3.2'")" == "1.0--1.1--1.2--3.0--3.1--3.2"
	# the objects of the extension by description, with the definition of the functions
	MEMBERS="SELECT pg_describe_object(classid, objid, objsubid) || ' ' || coalesce(CASE classid WHEN 'pg_proc'::regclass THEN pg_get_functiondef(objid) END, '')
		FROM pg_depend WHERE refclassid = 'pg_extension'::regclass AND deptype = 'e'
		AND refobjid = (SELECT oid FROM pg_extension WHERE extname = 'pg_sift') ORDER BY 1"
	q life upg "$PATHSET" "ALTER EXTENSION pg_sift UPDATE TO '3.0'" >/dev/null
	# a column index written with the 3.0 segment format (minor 3), which the 3.1 library must keep reading across the update
	q life upg "CREATE TABLE colt (id int, grp int, v numeric(10,2))" \
		"INSERT INTO colt SELECT g, g % 9, g % 1000 / 8.0 FROM generate_series(1, 20000) g" \
		"SET sift.col_format_minor = 3" "CREATE INDEX colt_col ON colt USING sift_col (id, grp, v)" >/dev/null
	# a sift_table with the 3.0 column minor (the files of the real 3.0 library are read in old_lib_refusal.sh)
	q life upg "SET sift.col_format_minor = 3" "CREATE TABLE tbl (id int, grp int, v numeric(10,2)) USING sift_table" \
		"INSERT INTO tbl SELECT g, g % 9, g % 1000 / 8.0 FROM generate_series(1, 20000) g" "VACUUM tbl" >/dev/null
	TBLQ="SELECT count(*) || ':' || sum(v) || ':' || max(id) FROM tbl"
	TBL0=$(q life upg "$TBLQ" | tail -n 1)
	COLQ="SELECT string_agg(grp || ':' || n || ':' || s, ',' ORDER BY grp) FROM (SELECT grp, count(*) n, sum(v) s FROM colt GROUP BY grp) g"
	COLON=$(q life upg "SET sift.enable_agg = on" "$COLQ" | tail -n 1)
	COLOFF=$(q life upg "SET sift.enable_agg = off" "SET sift.enable_col = off" "$COLQ" | tail -n 1)
	check "the minor 3 column index answers the same as the heap" test "$COLON" == "$COLOFF"
	q life postgres "CREATE DATABASE fresh" "CREATE DATABASE upg12" >/dev/null
	q life fresh "CREATE EXTENSION pg_sift VERSION '3.0'" >/dev/null
	check "1.0 updated to 3.0 has the objects of a fresh 3.0 install" test "$(q life upg "$MEMBERS")" == "$(q life fresh "$MEMBERS")"
	q life upg12 "$PATHSET" "CREATE EXTENSION pg_sift VERSION '1.2'" "ALTER EXTENSION pg_sift UPDATE TO '3.0'" >/dev/null
	check "1.2 updated to 3.0 has the objects of a fresh 3.0 install" test "$(q life upg12 "$MEMBERS")" == "$(q life fresh "$MEMBERS")"
	check "the updated extension has the column index and the table access methods" test "$(q life upg12 \
		"SELECT string_agg(amname, ',' ORDER BY amname) FROM pg_am WHERE amname IN ('sift', 'sift_col', 'sift_table')")" == sift,sift_col,sift_table
	FACETS="SELECT (SELECT string_agg(value || ':' || count, ',' ORDER BY value) FROM sift.facets('docs_idx', 'docs_kw', 'gamma', NULL))
		= (SELECT string_agg(k || ':' || c, ',' ORDER BY k) FROM (SELECT (id % 5)::text k, count(*) c FROM docs WHERE body @@> 'gamma' GROUP BY 1) g)"
	q life upg "CREATE INDEX docs_kw ON docs USING sift (((id % 5)::text)) WITH (analyzer = keyword)" >/dev/null
	check "sift.facets works on the updated 3.0 extension" test "$(q life upg "$FACETS")" == t
	q life upg "$PATHSET" "ALTER EXTENSION pg_sift UPDATE" >/dev/null
	check "ALTER EXTENSION UPDATE runs the next script" test "$(q life upg "SELECT extversion FROM pg_extension WHERE extname = 'pg_sift'")" == 3.2
	q life postgres "CREATE DATABASE fresh32" "CREATE DATABASE upg31" >/dev/null
	q life fresh32 "CREATE EXTENSION pg_sift" >/dev/null
	check "3.0 updated to 3.2 has the objects of a fresh 3.2 install" test "$(q life upg "$MEMBERS")" == "$(q life fresh32 "$MEMBERS")"
	q life upg31 "$PATHSET" "CREATE EXTENSION pg_sift VERSION '3.1'" \
		"CREATE TABLE kinds31 (id int, doc jsonb)" \
		"INSERT INTO kinds31 SELECT i, jsonb_build_array(CASE i % 3 WHEN 0 THEN '{}'::jsonb WHEN 1 THEN '{\"s\":null}'::jsonb ELSE '{\"s\":\"gift\"}'::jsonb END) FROM generate_series(1,8000) i" \
		"SET sift.col_format_minor = 4" "CREATE INDEX kinds31_col ON kinds31 USING sift_col (id,doc) WITH (paths = 'doc[]{s text}')" \
		"ALTER EXTENSION pg_sift UPDATE TO '3.2'" >/dev/null
	KINDQ="SELECT count(*) FROM kinds31 WHERE jsonb_path_exists(doc, '\$[*] ? (@.s != \"gift\")')"
	check "a minor 4 index survives the 3.1 SQL upgrade with exact JSON null answers" test "$(q life upg31 "SET sift.enable_agg = on" "$KINDQ" | tail -n 1)" == "$(q life upg31 "SET sift.enable_agg = off" "$KINDQ" | tail -n 1)"
	q life upg31 "REINDEX INDEX kinds31_col" >/dev/null
	check "REINDEX after the upgrade writes complete kinds in minor 6" test "$(q life upg31 "SELECT min(format) FROM sift.col_segments('kinds31_col') WHERE kind = 'immutable'")" == 6
	check "3.1 updated to 3.2 has the objects of a fresh 3.2 install" test "$(q life upg31 "$MEMBERS")" == "$(q life fresh32 "$MEMBERS")"
	ROLLUPSQL="SELECT (SELECT requested FROM sift.col_rollup_state) = 0 AND (SELECT count(*) FROM sift.col_rollup_requests) = 0"
	check "the rollup refresh views exist after a fresh 3.2 install" test "$(q life fresh32 "$ROLLUPSQL")" == t
	check "the rollup refresh views exist after ALTER EXTENSION UPDATE 3.0 to 3.2" test "$(q life upg "$ROLLUPSQL")" == t
	check "sift.facet_counts and sift.field_query exist and answer after ALTER EXTENSION UPDATE 3.0 to 3.2" test "$(q life upg \
		"SELECT (SELECT string_agg(value || ':' || count, ',' ORDER BY value) FROM sift.facet_counts('docs', 'body', 'gamma', '[{\"name\": \"k\", \"expr\": \"id % 5\"}]'))
			= (SELECT string_agg(k || ':' || c, ',' ORDER BY k) FROM (SELECT (id % 5)::text k, count(*) c FROM docs WHERE body @@> 'gamma' GROUP BY 1) g)
			AND sift.field_query('gamma', 'body') IS NOT NULL")" == t
	check "the segment views list the format of the 3.0 segments and of the new ones" test "$(q life upg \
		"SELECT (SELECT string_agg(DISTINCT format::text, ',') FROM sift.col_segments('colt_col') WHERE kind = 'immutable') || ' ' || (SELECT string_agg(DISTINCT format::text, ',') FROM sift.table_segments('tbl') WHERE kind = 'columns')")" == "3 3"
	check "the sift_table written by 3.0 answers the same after the update" test "$(q life upg "$TBLQ" | tail -n 1)" == "$TBL0"
	check "the index built before the update still verifies and answers the same" \
		test "$(vq life upg "SELECT fp() = seq() AND fp() || ranked() = '$(sed "s/'/''/g" <<<"$before")'")" == t
	check "the minor 3 column index verifies and answers the same after the update" test "$(q life upg "SELECT sift.col_verify('colt_col', true)" \
		"SET sift.enable_agg = on" "$COLQ" | tail -n 1)" == "$COLOFF"
	# minor 4 segments join the same index: written after the update, beside the minor 3 ones
	q life upg "INSERT INTO colt SELECT g, g % 9, g % 1000 / 8.0 FROM generate_series(20001, 40000) g" "SELECT sift.col_promote('colt_col')" >/dev/null
	COLOFF=$(q life upg "SET sift.enable_agg = off" "SET sift.enable_col = off" "$COLQ" | tail -n 1)
	check "the index with minor 3 and minor 4 segments verifies and answers the same" test "$(q life upg "SELECT sift.col_verify('colt_col', true)" \
		"SET sift.enable_agg = on" "$COLQ" | tail -n 1)" == "$COLOFF"
	# the sift_table written by 3.0 takes the writes of 3.1 (column minor 4 groups beside its minor 3 ones)
	q life upg "INSERT INTO tbl SELECT g, g % 9, g % 1000 / 8.0 FROM generate_series(20001, 40000) g" "VACUUM tbl" >/dev/null
	# paths declared on the updated extension: the rewrite writes them, and the query answers as the jsonb column does
	q life upg "CREATE TABLE tblj (id int, doc jsonb) USING sift_table" "INSERT INTO tblj SELECT g, jsonb_build_object('k', g % 7) FROM generate_series(1, 3000) g" \
		"SELECT sift.table_set_paths('tblj', 'doc', 'doc->k int4')" "VACUUM FULL tblj" >/dev/null
	check "sift.table_set_paths works on the updated extension" test "$(q life upg "SET sift.enable_agg = on" \
		"SELECT (SELECT string_agg(k || ':' || c, ',' ORDER BY k) FROM (SELECT doc->>'k' k, count(*) c FROM tblj WHERE (doc->>'k')::int >= 0 GROUP BY 1) g)
			= (SELECT string_agg(k || ':' || c, ',' ORDER BY k) FROM (SELECT (id % 7)::text k, count(*) c FROM tblj GROUP BY 1) g)" | tail -n 1)" == t
	check "the sift_table written by 3.0 adds up the rows written after the update" test "$(q life upg "$TBLQ" | tail -n 1)" == "$(q life upg \
		"SELECT count(*) || ':' || sum(v) || ':' || max(id) FROM (SELECT g id, (g % 1000 / 8.0)::numeric(10,2) v FROM generate_series(1, 40000) g) s" | tail -n 1)"
	q life upg "INSERT INTO docs VALUES (9000, 'alpha3 afterupdate')" "SELECT sift.promote('docs_idx')" >/dev/null
	check "the updated library writes segments of format 3 next to the format 2 one (a mixed index, verified and equal to a scan)" \
		test "$(formats | grep -c '^2,3$')$(vq life upg "SELECT fp() = seq()")" == 1t
	crash life
	node_start life || bail "restart failed"
	check "inserts after the update survive a crash" test "$(vq life upg \
		"SELECT fp() = seq() AND (SELECT count(*) FROM docs WHERE body @@> 'afterupdate') = 1")" == t
	check "the mixed column index survives the crash" test "$(q life upg "SELECT sift.col_verify('colt_col', true)" \
		"SET sift.enable_agg = on" "$COLQ" | tail -n 1)" == "$COLOFF"
	q life upg "SELECT sift.merge('docs_idx', true)" >/dev/null
	check "a merge rewrites the mixed index in format 3 (verified, results equal to a scan)" \
		test "$(formats)$(vq life upg "SELECT fp() = seq()")" == 3t
	q life upg "DROP EXTENSION pg_sift_test" >/dev/null
else
	skip 29 "extension_control_path needs PostgreSQL 18"
fi

# same-major pg_upgrade of every database: index files are carried over. pg_upgrade checks libraries with
# LOAD '$libdir/pg_sift', which ignores dynamic_library_path, so it needs sift installed in the server's pkglibdir.
if [[ ! -f $(pg_config --pkglibdir)/pg_sift.so ]]; then
	skip $((3 + $(wc -w <<<"$DBS"))) "pg_upgrade needs sift installed in $(pg_config --pkglibdir)"
	finish
fi
for db in $DBS; do eval "FP_$db=\$(q life $db 'SELECT fp() || ranked()')"; done
node_stop life
node_init up 8
BIN=$(pg_config --bindir)
(cd "$(dir up)" && LD_PRELOAD=${SIFT_PRELOAD:-} pg_upgrade -b "$BIN" -B "$BIN" -d "$(dir life)/data" -D "$(dir up)/data" \
	-U postgres -p "$(port life)" -P "$(port up)" -s "$(dir up)/sock" >"$(dir up)/pg_upgrade.log" 2>&1)
upgraded=$?
check "pg_upgrade of the databases with sift indexes succeeds" test $upgraded -eq 0
[[ $upgraded -eq 0 ]] || diag "$(cat "$(dir up)"/data/pg_upgrade_output.d/*/loadable_libraries.txt; tail -n 5 "$(dir up)"/data/pg_upgrade_output.d/*/log/pg_upgrade_server.log)"
node_start up || bail "upgraded cluster did not start"
for db in $DBS; do
	check "upgraded $db verifies and keeps its results" test "$(vq up $db "SELECT fp() = seq()")" == t
done
mismatch=0
for db in $DBS; do
	var=FP_$db
	[[ $(q up $db "SELECT fp() || ranked()") == "${!var}" ]] || mismatch=$((mismatch + 1))
done
check "metrics_a and results are identical after pg_upgrade" test $mismatch -eq 0
q up src "INSERT INTO docs VALUES (7000, 'alpha3 postupgrade')" "VACUUM docs" >/dev/null
check "the upgraded cluster accepts writes and vacuum" test "$(vq up src "SELECT fp() = seq()")" == t
finish
