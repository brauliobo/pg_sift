#!/usr/bin/env bash
# Extension lifecycle with existing indexes: pg_dump | psql and pg_restore round trips, ALTER EXTENSION UPDATE
# along a multi-step upgrade path (the real pg_sift--1.0--1.1.sql, 1.1--1.2.sql, 1.2--3.0.sql, 3.0--3.1.sql, 3.1--3.2.sql and 3.2--3.3.sql in a private
# extension_control_path directory; 1.0 -> 3.0 and 1.2 -> 3.0 are compared with a fresh 3.0 install, 3.1 -> 3.2 with a fresh 3.2 install, 1.0, 3.0, 3.1 and 3.2 -> 3.3 with a fresh 3.3 install), installation through extension_control_path and dynamic_library_path
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
plan $((68 + $(wc -w <<<"$DBS") + (PG_MAJOR >= 18 ? 7 : 0)))
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
		"SELECT default_version FROM pg_available_extensions WHERE name = 'pg_sift'")" == 3.3
	check "the library loads through dynamic_library_path" test "$(vq life src \
		"SELECT count(*) FROM pg_get_loaded_modules() WHERE module_name = 'pg_sift'")" == 1
	check "without the path the extension is not found" test "$(q life postgres "SET extension_control_path = '/nonexistent'" \
		"SELECT count(*) FROM pg_available_extensions WHERE name = 'pg_sift'")" == 0

	# upgrade path: the real scripts of every version
	UPD=$(dir life)/upd
	mkdir -p "$UPD/extension"
	cp "$SHAREDIR"/extension/pg_sift--{1.0,1.1,1.2,3.0,3.1,3.2,3.3}.sql "$SHAREDIR"/extension/pg_sift--{1.0--1.1,1.1--1.2,1.2--3.0,3.0--3.1,3.1--3.2,3.2--3.3}.sql "$UPD/extension/"
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
	check "the update path chains the six steps" test "$(q life upg "$PATHSET" \
		"SELECT path FROM pg_extension_update_paths('pg_sift') WHERE source = '1.0' AND target = '3.3'")" == "1.0--1.1--1.2--3.0--3.1--3.2--3.3"
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
	upd=$(LC_ALL=C q life upg "$PATHSET" "ALTER EXTENSION pg_sift UPDATE" 2>&1)
	# a text index of an older release cannot say whether it holds a word over 255 bytes, so it is listed as required; a keyword index never held one
	check "the 1.0 to 3.3 update warns that the text index needs a REINDEX and skips the keyword one" test "$(grep -c 'REINDEX INDEX CONCURRENTLY public.docs_idx;' <<<"$upd")/$(grep -c docs_kw <<<"$upd")" == 1/0
	check "ALTER EXTENSION UPDATE runs the next script" test "$(q life upg "SELECT extversion FROM pg_extension WHERE extname = 'pg_sift'")" == 3.3
	q life postgres "CREATE DATABASE fresh32" "CREATE DATABASE fresh33" "CREATE DATABASE upg31" "CREATE DATABASE upg32" >/dev/null
	q life fresh32 "$PATHSET" "CREATE EXTENSION pg_sift VERSION '3.2'" >/dev/null
	check "a fresh 3.3 install prints nothing about a REINDEX" test -z "$(LC_ALL=C q life fresh33 "CREATE EXTENSION pg_sift" 2>&1)"
	check "3.0 updated to 3.3 has the objects of a fresh 3.3 install" test "$(q life upg "$MEMBERS")" == "$(q life fresh33 "$MEMBERS")"
	q life upg31 "$PATHSET" "CREATE EXTENSION pg_sift VERSION '3.1'" \
		"CREATE TABLE kinds31 (id int, doc jsonb)" \
		"INSERT INTO kinds31 SELECT i, jsonb_build_array(CASE i % 3 WHEN 0 THEN '{}'::jsonb WHEN 1 THEN '{\"s\":null}'::jsonb ELSE '{\"s\":\"gift\"}'::jsonb END) FROM generate_series(1,8000) i" \
		"SET sift.col_format_minor = 4" "CREATE INDEX kinds31_col ON kinds31 USING sift_col (id,doc) WITH (paths = 'doc[]{s text}')" \
		"CREATE INDEX kinds31_old ON kinds31 USING sift_col (id,doc) WITH (paths = 'doc[]{s text}')" \
		"ALTER EXTENSION pg_sift UPDATE TO '3.2'" >/dev/null
	KINDQ="SELECT count(*) FROM kinds31 WHERE jsonb_path_exists(doc, '\$[*] ? (@.s != \"gift\")')"
	check "a minor 4 index survives the 3.1 SQL upgrade with exact JSON null answers" test "$(q life upg31 "SET sift.enable_agg = on" "$KINDQ" | tail -n 1)" == "$(q life upg31 "SET sift.enable_agg = off" "$KINDQ" | tail -n 1)"
	q life upg31 "REINDEX INDEX kinds31_col" >/dev/null
	check "REINDEX after the upgrade writes complete kinds in minor 6" test "$(q life upg31 "SELECT min(format) FROM sift.col_segments('kinds31_col') WHERE kind = 'immutable'")" == 6
	check "3.1 updated to 3.2 has the objects of a fresh 3.2 install" test "$(q life upg31 "$MEMBERS")" == "$(q life fresh32 "$MEMBERS")"
	# the 3.2 scripts are the released ones: the functions of 3.3 come with 3.2--3.3.sql and pg_sift--3.3.sql only
	NEWFN="SELECT string_agg(proname, ',' ORDER BY proname) FROM pg_proc WHERE pronamespace = 'sift'::regnamespace
		AND proname IN ('col_refresh', 'col_clustering', 'stats', 'stats_reset', 'col_rollup_misses', 'explain_why', 'upgrade_commands')"
	check "a fresh 3.2 install has none of the functions added in 3.3" test "$(q life fresh32 "$NEWFN")" == ""
	# the last step of the chain names the index of minor 4 that nothing rewrote, not the one the REINDEX above rewrote
	upd=$(LC_ALL=C q life upg31 "$PATHSET" "ALTER EXTENSION pg_sift UPDATE" 2>&1)
	check "the 3.1 to 3.3 update prints the REINDEX of an index written at minor 4 with JSON paths" grep -qF "REINDEX INDEX CONCURRENTLY public.kinds31_old;" <<<"$upd"
	check "the update does not list the index that was reindexed" test "$(grep -c kinds31_col <<<"$upd")" == 0
	check "3.1 updated to 3.3 has the objects of a fresh 3.3 install" test "$(q life upg31 "$MEMBERS")" == "$(q life fresh33 "$MEMBERS")"
	q life upg32 "$PATHSET" "CREATE EXTENSION pg_sift VERSION '3.2'" "CREATE TABLE items (id int, kind int, val int)" \
		"INSERT INTO items SELECT g, g % 5, g FROM generate_series(1, 5000) g" "CREATE INDEX items_col ON items USING sift_col (id, kind, val)" \
		"SELECT sift.col_promote('items_col')" "SET sift.col_format_minor = 6" "CREATE INDEX items_late ON items USING sift_col (id, kind, val)" \
		"ALTER INDEX items_late SET (set_columns = 'kind')" "CREATE TABLE tbl32 (id int, at timestamp) USING sift_table" \
		"INSERT INTO tbl32 SELECT g, timestamp '2026-01-01' + g * interval '1 hour' FROM generate_series(1, 3000) g" "VACUUM tbl32" >/dev/null
	q life upg32 "CREATE TABLE tbl32_new (id int, at timestamp) USING sift_table" \
		"INSERT INTO tbl32_new SELECT g, timestamp '2026-01-01' + g * interval '1 hour' FROM generate_series(1, 3000) g" "VACUUM tbl32_new" >/dev/null
	upd=$(LC_ALL=C q life upg32 "$PATHSET" "ALTER EXTENSION pg_sift UPDATE" 2>&1)
	check "the 3.2 to 3.3 update warns with the REINDEX of the index whose option came after its units" grep -qF "REINDEX INDEX CONCURRENTLY public.items_late;" <<<"$upd"
	check "the update does not list the index without options" test "$(grep -c items_col <<<"$upd")" == 0
	check "the printed command runs" q life upg32 "REINDEX INDEX CONCURRENTLY public.items_late"
	check "the update names the sift_table written without calendar cells, with a VACUUM FULL, and not the one written by this library" test \
		"$(grep -c 'VACUUM FULL public.tbl32;' <<<"$upd")/$(grep -c 'public.tbl32_new;' <<<"$upd")" == 1/0
	q life upg32 "VACUUM FULL public.tbl32" >/dev/null
	check "after the VACUUM FULL the table is in column format 8 and no longer listed" test "$(q life upg32 "SELECT (SELECT string_agg(DISTINCT format::text, ',') FROM sift.table_segments('tbl32') WHERE kind = 'columns') || ':' || \
		(SELECT count(*) FROM sift.upgrade_commands() WHERE kind = 'rewrite')")" == 8:0
	check "a VACUUM FULL under sift.col_format_minor = 6 writes format 4" test "$(q life upg32 "SET sift.col_format_minor = 6" "VACUUM FULL tbl32_new" \
		"SELECT string_agg(DISTINCT format::text, ',') FROM sift.table_segments('tbl32_new') WHERE kind = 'columns'")" == 4
	check "3.2 updated to 3.3 has the objects of a fresh 3.3 install" test "$(q life upg32 "$MEMBERS")" == "$(q life fresh33 "$MEMBERS")"
	check "the functions added in 3.3 exist after a fresh 3.3 install and after each update path" test "$(for d in fresh33 upg upg31 upg32; do q life $d "$NEWFN"; done | sort -u)" == "col_clustering,col_refresh,col_rollup_misses,explain_why,stats,stats_reset,upgrade_commands"
	check "the functions added in 3.3 run on an index written before the update" test "$(q life upg32 "SELECT sift.stats_reset()" \
		"SELECT remaining FROM sift.col_refresh('items_col', 0)" "SELECT count(*) > 0 FROM sift.col_clustering('items_col')" \
		"SELECT count(*) >= 0 FROM sift.stats()" "SELECT count(*) >= 0 FROM sift.col_rollup_misses()" \
		"SELECT count(*) > 0 FROM sift.explain_why('SELECT kind, count(*) FROM items GROUP BY 1')" | tr -d '\n')" == "0tttt"
	# 22 indexes: the first 20 commands and the count of the rest; an index that another session holds is never waited for
	q life postgres "CREATE DATABASE cap32" "CREATE DATABASE lock32" >/dev/null
	q life cap32 "$PATHSET" "CREATE EXTENSION pg_sift VERSION '3.2'" "CREATE TABLE f (id int, g int)" "INSERT INTO f SELECT i, i % 9 FROM generate_series(1, 500) i" \
		"DO \$\$ BEGIN FOR i IN 1..22 LOOP EXECUTE format('CREATE INDEX f_%s ON f USING sift_col (id, g)', i); EXECUTE format('ALTER INDEX f_%s SET (set_columns = ''g'')', i); END LOOP; END \$\$" >/dev/null
	upd=$(LC_ALL=C q life cap32 "$PATHSET" "ALTER EXTENSION pg_sift UPDATE" 2>&1)
	check "the update lists 20 commands" test "$(grep -c 'REINDEX INDEX CONCURRENTLY' <<<"$upd")" == 20
	check "the update counts the rest" grep -qx "and 2 more" <<<"$upd"
	q life postgres "CREATE DATABASE none32" >/dev/null
	q life none32 "$PATHSET" "CREATE EXTENSION pg_sift VERSION '3.2'" "CREATE TABLE f (id int, g int)" "INSERT INTO f SELECT i, i % 9 FROM generate_series(1, 500) i" \
		"CREATE INDEX f_1 ON f USING sift_col (id, g)" >/dev/null
	upd32=$(LC_ALL=C q life none32 "$PATHSET" "ALTER EXTENSION pg_sift UPDATE" 2>&1)
	check "an update with nothing to do says so, and that a 3.2 library cannot read what 3.3 writes next" test "$(grep -cF -e "nothing to REINDEX or rewrite" -e "a 3.2 library cannot read the sift_table tables and sift_col indexes written after this update, unless sift.col_format_minor is lowered to 6" <<<"$upd32")" == 2
	q life lock32 "$PATHSET" "CREATE EXTENSION pg_sift VERSION '3.2'" "CREATE TABLE f (id int, g int)" "INSERT INTO f SELECT i, i % 9 FROM generate_series(1, 500) i" \
		"CREATE INDEX f_1 ON f USING sift_col (id, g)" "ALTER INDEX f_1 SET (set_columns = 'g')" >/dev/null
	q life lock32 "BEGIN" "REINDEX INDEX f_1" "SELECT pg_sleep(60)" >/dev/null 2>&1 &
	for _ in $(seq 100); do
		[[ $(q life lock32 "SELECT count(*) FROM pg_locks WHERE locktype = 'relation' AND mode = 'AccessExclusiveLock' AND granted AND relation = 'f_1'::regclass") != 0 ]] && break
		sleep 0.1
	done
	upd=$(LC_ALL=C q life lock32 "$PATHSET" "ALTER EXTENSION pg_sift UPDATE" 2>&1); rc=$?
	q life lock32 "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname = 'lock32' AND pid <> pg_backend_pid() AND query LIKE '%pg_sleep(60)%'" >/dev/null; wait
	check "the update with a locked index finishes and says to run the function after reconnecting" test "$rc/$(grep -c 'the check for what to REINDEX or rewrite did not run' <<<"$upd")/$(q life lock32 \
		"SELECT extversion FROM pg_extension WHERE extname = 'pg_sift'")" == 0/1/3.3
	check "the function answers once the lock is gone" test "$(q life lock32 "SELECT command FROM sift.upgrade_commands()")" == "REINDEX INDEX CONCURRENTLY public.f_1;"
	ROLLUPSQL="SELECT (SELECT requested FROM sift.col_rollup_state) = 0 AND (SELECT count(*) FROM sift.col_rollup_requests) = 0"
	check "the rollup refresh views exist after a fresh 3.3 install" test "$(q life fresh33 "$ROLLUPSQL")" == t
	check "the rollup refresh views exist after ALTER EXTENSION UPDATE 3.0 to 3.3" test "$(q life upg "$ROLLUPSQL")" == t
	check "sift.facet_counts and sift.field_query exist and answer after ALTER EXTENSION UPDATE 3.0 to 3.3" test "$(q life upg \
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
	# minor 7 segments (the cube also holds min and max; opt-in) join an index of minor 3 segments: the units written before scan, the others answer, the results are the heap's
	q life upg "CREATE TABLE colm (id int, grp int, v numeric(10,2))" "INSERT INTO colm SELECT g, g % 9, g % 1000 / 8.0 FROM generate_series(1, 20000) g" \
		"SET sift.col_format_minor = 3" "CREATE INDEX colm_col ON colm USING sift_col (id, grp, v)" "RESET sift.col_format_minor" \
		"ALTER INDEX colm_col SET (cube_minmax = true)" "INSERT INTO colm SELECT g, g % 9, g % 1000 / 8.0 FROM generate_series(20001, 40000) g" \
		"SELECT sift.col_promote('colm_col')" >/dev/null
	MMQ="SELECT string_agg(grp || ':' || lo || ':' || hi, ',' ORDER BY grp) FROM (SELECT grp, min(v) lo, max(v) hi FROM colm GROUP BY grp) g"
	MMOFF=$(q life upg "SET sift.enable_agg = off" "SET sift.enable_col = off" "$MMQ" | tail -n 1)
	check "an index with minor 3 and minor 7 segments" test "$(q life upg "SELECT string_agg(DISTINCT format::text, ',' ORDER BY format::text) FROM sift.col_segments('colm_col') WHERE kind = 'immutable'")" == 3,7
	check "the index with minor 3 and minor 7 segments verifies and answers min and max as the heap" test "$(q life upg "SELECT sift.col_verify('colm_col', true)" \
		"SET sift.enable_agg = on" "$MMQ" | tail -n 1)" == "$MMOFF"
	# a sift_table with a timestamp column written as minor 3 takes the writes of the default setting (the month of the column is a dim of the cube): the units written before scan, the others answer, the results are the plain plan's
	q life upg "SET sift.col_format_minor = 3" "CREATE TABLE tblm (id int, ts timestamp, v numeric(10,2), d date, tz timestamptz) USING sift_table" \
		"INSERT INTO tblm SELECT g, timestamp '2020-01-01' + g * interval '1 hour', g % 1000 / 8.0, date '2020-01-01' + g / 24, timestamptz '2020-01-01+00' + g * interval '1 hour' FROM generate_series(1, 30000) g" "VACUUM tblm" >/dev/null
	q life upg "INSERT INTO tblm SELECT g, timestamp '2020-01-01' + g * interval '1 hour', g % 1000 / 8.0, date '2020-01-01' + g / 24, timestamptz '2020-01-01+00' + g * interval '1 hour' FROM generate_series(30001, 60000) g" "VACUUM tblm" >/dev/null
	MONQ="SELECT string_agg(m || ':' || n || ':' || s, ',' ORDER BY m) FROM (SELECT date_trunc('month', ts)::date m, count(*) n, sum(v) s FROM tblm GROUP BY 1) g"
	MONOFF=$(q life upg "SET sift.enable_agg = off" "$MONQ" | tail -n 1)
	CALQ="SELECT string_agg(y || ':' || n || ':' || s, ',' ORDER BY y) FROM (SELECT extract(isoyear FROM d) y, count(*) n, sum(v) s FROM tblm WHERE d >= '2020-03-01' AND d < '2025-03-01' GROUP BY 1) g"
	CALOFF=$(q life upg "SET sift.enable_agg = off" "$CALQ" | tail -n 1)
	TZQ="SELECT string_agg(m || ':' || n, ',' ORDER BY m) FROM (SELECT date_trunc('quarter', tz AT TIME ZONE 'UTC')::date m, count(*) n FROM tblm WHERE tz >= '2021-01-01+00' GROUP BY 1) g"
	TZOFF=$(q life upg "SET sift.enable_agg = off" "$TZQ" | tail -n 1)
	check "a sift_table with minor 3 and minor 8 segments" test "$(q life upg "SELECT string_agg(DISTINCT format::text, ',' ORDER BY format::text) FROM sift.table_segments('tblm') WHERE kind = 'columns'")" == 3,8
	check "the sift_table with minor 3 and minor 8 segments answers the months as the plain plan" test "$(q life upg "SET sift.enable_agg = on" "SET cpu_tuple_cost = 0.5" "$MONQ" | tail -n 1)" == "$MONOFF"
	check "the sift_table with minor 3 and minor 8 segments answers the ISO years of a date range as the plain plan" test "$(q life upg "SET sift.enable_agg = on" "SET cpu_tuple_cost = 0.5" "$CALQ" | tail -n 1)" == "$CALOFF"
	check "the sift_table with minor 3 and minor 8 segments answers the UTC quarters of a timestamptz as the plain plan" test "$(q life upg "SET sift.enable_agg = on" "SET cpu_tuple_cost = 0.5" "$TZQ" | tail -n 1)" == "$TZOFF"
	# minor 9 segments (the rollup of the rows; opt-in) join an index of minor 3 and minor 7 segments: the units written before scan, the others answer, the results are the heap's
	q life upg "CREATE TABLE rowr (id int, a int, b int, v numeric(10,2))" "INSERT INTO rowr SELECT g, g % 9, g % 7, g % 1000 / 8.0 FROM generate_series(1, 20000) g" \
		"SET sift.col_format_minor = 3" "CREATE INDEX rowr_col ON rowr USING sift_col (id, a, b, v)" "RESET sift.col_format_minor" \
		"ALTER INDEX rowr_col SET (cube_minmax = true)" "INSERT INTO rowr SELECT g, g % 9, g % 7, g % 1000 / 8.0 FROM generate_series(20001, 30000) g" \
		"SELECT sift.col_promote('rowr_col')" "ALTER INDEX rowr_col SET (rollup_dims = 'a, b')" \
		"INSERT INTO rowr SELECT g, g % 9, g % 7, g % 1000 / 8.0 FROM generate_series(30001, 40000) g" "SELECT sift.col_promote('rowr_col')" >/dev/null
	RRQ="SELECT string_agg(a || ':' || b || ':' || n || ':' || s, ',' ORDER BY a, b) FROM (SELECT a, b, count(*) n, sum(v) s FROM rowr GROUP BY a, b) g"
	RROFF=$(q life upg "SET sift.enable_agg = off" "SET sift.enable_col = off" "$RRQ" | tail -n 1)
	check "an index with minor 3, minor 7 and minor 9 segments" test "$(q life upg "SELECT string_agg(DISTINCT format::text, ',' ORDER BY format::text) FROM sift.col_segments('rowr_col') WHERE kind = 'immutable'")" == 3,7,9
	check "the index with minor 3, 7 and 9 segments verifies and answers the two-key group as the heap" test "$(q life upg "SELECT sift.col_verify('rowr_col', true)" \
		"SET sift.enable_agg = on" "$RRQ" | tail -n 1)" == "$RROFF"
	# minor 10 segments (the small-set summary of a leaf; opt-in) join an index of minor 3 segments: the units written before are read, the others skip by their summary, the results are the heap's
	q life upg "CREATE TABLE cols8 (id int, grp int, v numeric(10,2))" "INSERT INTO cols8 SELECT g, g * 7919 % 50000, g % 1000 / 8.0 FROM generate_series(1, 20000) g" \
		"SET sift.col_format_minor = 3" "CREATE INDEX cols8_col ON cols8 USING sift_col (id, grp, v)" "RESET sift.col_format_minor" \
		"ALTER INDEX cols8_col SET (set_columns = 'grp')" "INSERT INTO cols8 SELECT g, g * 7919 % 50000, g % 1000 / 8.0 FROM generate_series(20001, 40000) g" \
		"SELECT sift.col_promote('cols8_col')" >/dev/null
	S8Q="SELECT count(*) || ':' || sum(v) FROM cols8 WHERE grp = 7919"
	S8OFF=$(q life upg "SET sift.enable_agg = off" "SET sift.enable_col = off" "$S8Q" | tail -n 1)
	check "an index with minor 3 and minor 10 segments" test "$(q life upg "SELECT string_agg(f::text, ',' ORDER BY f) FROM (SELECT DISTINCT format f FROM sift.col_segments('cols8_col') WHERE kind = 'immutable') x")" == 3,10
	check "the index with minor 3 and minor 10 segments verifies and answers an equality as the heap" test "$(q life upg "SELECT sift.col_verify('cols8_col', true)" \
		"SET sift.enable_agg = on" "$S8Q" | tail -n 1)" == "$S8OFF"
	# minor 12 segments (a summary of each group of units; opt-in) join an index of minor 3 segments: the groups answer, the units written before are read one by one, the results are the heap's
	q life upg "CREATE TABLE cols (id int, grp int, v numeric(10,2), pad text)" "ALTER TABLE cols ALTER COLUMN pad SET STORAGE PLAIN" \
		"INSERT INTO cols SELECT g, g % 9, g % 1000 / 8.0, repeat('p', 1500) FROM generate_series(1, 12000) g" \
		"SET sift.col_format_minor = 3" "CREATE INDEX cols_col ON cols USING sift_col (id, grp, v)" "RESET sift.col_format_minor" \
		"ALTER INDEX cols_col SET (superunit = 4)" "INSERT INTO cols SELECT g, g % 9, g % 1000 / 8.0, repeat('p', 1500) FROM generate_series(12001, 24000) g" \
		"VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) cols" "SELECT sift.col_promote('cols_col')" >/dev/null
	SUQ="SELECT string_agg(grp || ':' || n || ':' || s, ',' ORDER BY grp) FROM (SELECT grp, count(*) n, sum(v) s FROM cols GROUP BY grp) g"
	SUOFF=$(q life upg "SET sift.enable_agg = off" "SET sift.enable_col = off" "$SUQ" | tail -n 1)
	check "an index with minor 3 and minor 12 segments" test "$(q life upg "SELECT string_agg(f::text, ',' ORDER BY f) FROM (SELECT DISTINCT format f FROM sift.col_segments('cols_col') WHERE kind = 'immutable') x")" == 3,12
	check "the index with minor 3 and minor 12 segments verifies and answers as the heap, with and without the summaries" test "$(q life upg "SELECT sift.col_verify('cols_col', true)" \
		"SET sift.enable_agg = on" "$SUQ" | tail -n 1)$(q life upg "SET sift.agg_super = off" "SET sift.enable_agg = on" "$SUQ" | tail -n 1)" == "$SUOFF$SUOFF"
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
	check "the sift_table with minor 3 and minor 8 segments answers the months as before after the crash" test "$(q life upg "SET sift.enable_agg = on" "SET cpu_tuple_cost = 0.5" "$MONQ" | tail -n 1)" == "$MONOFF"
	check "the sift_table with minor 3 and minor 8 segments answers the date range and the UTC quarters as before after the crash" test "$(q life upg "SET sift.enable_agg = on" "SET cpu_tuple_cost = 0.5" "$CALQ" "$TZQ" | tail -n 2 | tr '\n' '|')" == "$CALOFF|$TZOFF|"
	q life upg "SELECT sift.merge('docs_idx', true)" >/dev/null
	check "a merge rewrites the mixed index in format 3 (verified, results equal to a scan)" \
		test "$(formats)$(vq life upg "SELECT fp() = seq()")" == 3t
	q life upg "DROP EXTENSION pg_sift_test" >/dev/null
else
	skip 56 "extension_control_path needs PostgreSQL 18"
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
check "scores and results are identical after pg_upgrade" test $mismatch -eq 0
q up src "INSERT INTO docs VALUES (7000, 'alpha3 postupgrade')" "VACUUM docs" >/dev/null
check "the upgraded cluster accepts writes and vacuum" test "$(vq up src "SELECT fp() = seq()")" == t
finish
