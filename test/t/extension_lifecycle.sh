#!/usr/bin/env bash
# Extension lifecycle with existing indexes: pg_dump | psql and pg_restore round trips, ALTER EXTENSION UPDATE
# along a multi-step upgrade path (the real pg_sift--1.0--1.1.sql and a synthetic 1.1--1.2.sql in a private
# extension_control_path directory; 1.0 -> 1.1 is compared with a fresh 1.1 install), installation through extension_control_path and dynamic_library_path
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
CREATE INDEX docs_idx ON docs USING sift (body) WITH (mutable_limit = 64);
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

plan 25
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
	"SELECT indexdef FROM pg_indexes WHERE indexname = 'docs_idx'")" == "CREATE INDEX docs_idx ON public.docs USING sift (body) WITH (mutable_limit='64')"
q life schemaonly "INSERT INTO docs VALUES (1, 'alpha3 hello')" >/dev/null
check "the empty restored index accepts new documents" test "$(vq life schemaonly "SELECT fp() = seq()")" == t
check "pg_dump keeps the extension as one object" test "$(pgc life pg_dump src | grep -c 'CREATE EXTENSION IF NOT EXISTS pg_sift')" == 1

# extension_control_path and dynamic_library_path are the only install: nothing else makes sift available
check "sift is available through extension_control_path" test "$(q life postgres \
	"SELECT default_version FROM pg_available_extensions WHERE name = 'pg_sift'")" == 1.1
check "the library loads through dynamic_library_path" test "$(vq life src \
	"SELECT count(*) FROM pg_get_loaded_modules() WHERE module_name = 'pg_sift'")" == 1
check "without the path the extension is not found" test "$(q life postgres "SET extension_control_path = '/nonexistent'" \
	"SELECT count(*) FROM pg_available_extensions WHERE name = 'pg_sift'")" == 0

# upgrade path: 1.0 and 1.1 are the real scripts, 1.2 only adds a marker function
UPD=$(dir life)/upd
mkdir -p "$UPD/extension"
cp "$SHAREDIR"/extension/pg_sift--1.0.sql "$SHAREDIR"/extension/pg_sift--1.1.sql "$SHAREDIR"/extension/pg_sift--1.0--1.1.sql "$UPD/extension/"
sed "s/^default_version.*/default_version = '1.2'/" "$SHAREDIR/extension/pg_sift.control" >"$UPD/extension/pg_sift.control"
echo "CREATE FUNCTION sift.lifecycle_marker() RETURNS text LANGUAGE sql RETURN 'v1.2';" >"$UPD/extension/pg_sift--1.1--1.2.sql"
PATHSET="SET extension_control_path = '$UPD:\$system'"
q life postgres "CREATE DATABASE upg" >/dev/null
q life upg "$PATHSET" "CREATE EXTENSION pg_sift VERSION '1.0'" >/dev/null
q life upg "${SETUP#*CREATE EXTENSION pg_sift;}" >/dev/null || bail "upgrade setup failed"
before=$(q life upg "SELECT fp() || ranked()")
check "the update path chains the two steps" test "$(q life upg "$PATHSET" \
	"SELECT path FROM pg_extension_update_paths('pg_sift') WHERE source = '1.0' AND target = '1.2'")" == "1.0--1.1--1.2"
# the objects of the extension by description, with the definition of the functions
MEMBERS="SELECT pg_describe_object(classid, objid, objsubid) || ' ' || coalesce(CASE classid WHEN 'pg_proc'::regclass THEN pg_get_functiondef(objid) END, '')
	FROM pg_depend WHERE refclassid = 'pg_extension'::regclass AND deptype = 'e'
	AND refobjid = (SELECT oid FROM pg_extension WHERE extname = 'pg_sift') ORDER BY 1"
q life upg "$PATHSET" "ALTER EXTENSION pg_sift UPDATE TO '1.1'" >/dev/null
q life postgres "CREATE DATABASE fresh" >/dev/null
q life fresh "CREATE EXTENSION pg_sift VERSION '1.1'" >/dev/null
check "1.0 updated to 1.1 has the objects of a fresh 1.1 install" test "$(q life upg "$MEMBERS")" == "$(q life fresh "$MEMBERS")"
FACETS="SELECT (SELECT string_agg(value || ':' || count, ',' ORDER BY value) FROM sift.facets('docs_idx', 'docs_kw', 'gamma', NULL))
	= (SELECT string_agg(k || ':' || c, ',' ORDER BY k) FROM (SELECT (id % 5)::text k, count(*) c FROM docs WHERE body @@> 'gamma' GROUP BY 1) g)"
q life upg "CREATE INDEX docs_kw ON docs USING sift (((id % 5)::text)) WITH (analyzer = keyword)" >/dev/null
check "sift.facets works on the updated 1.1 extension" test "$(q life upg "$FACETS")" == t
q life upg "$PATHSET" "ALTER EXTENSION pg_sift UPDATE" >/dev/null
check "ALTER EXTENSION UPDATE runs the next script" test "$(q life upg "SELECT extversion || ' ' || sift.lifecycle_marker() FROM pg_extension WHERE extname = 'pg_sift'")" == "1.2 v1.2"
check "the index built before the update still verifies and answers the same" \
	test "$(vq life upg "SELECT fp() = seq() AND fp() || ranked() = '$(sed "s/'/''/g" <<<"$before")'")" == t
q life upg "INSERT INTO docs VALUES (9000, 'alpha3 afterupdate')" "SELECT sift.promote('docs_idx')" >/dev/null
crash life
node_start life || bail "restart failed"
check "inserts after the update survive a crash" test "$(vq life upg \
	"SELECT fp() = seq() AND (SELECT count(*) FROM docs WHERE body @@> 'afterupdate') = 1")" == t

# same-major pg_upgrade of every database: index files are carried over. pg_upgrade checks libraries with
# LOAD '$libdir/pg_sift', which ignores dynamic_library_path, so it needs sift installed in the server's pkglibdir.
if [[ ! -f $(pg_config --pkglibdir)/pg_sift.so ]]; then
	for _ in $(seq 7); do NTEST=$((NTEST + 1)); echo "ok $NTEST # skip pg_upgrade needs sift installed in $(pg_config --pkglibdir)"; done
	finish
fi
for db in src plain custom upg; do eval "FP_$db=\$(q life $db 'SELECT fp() || ranked()')"; done
node_stop life
node_init up 8
BIN=$(pg_config --bindir)
(cd "$(dir up)" && LD_PRELOAD=${SIFT_PRELOAD:-} pg_upgrade -b "$BIN" -B "$BIN" -d "$(dir life)/data" -D "$(dir up)/data" \
	-U postgres -p "$(port life)" -P "$(port up)" -s "$(dir up)/sock" >"$(dir up)/pg_upgrade.log" 2>&1)
upgraded=$?
check "pg_upgrade of the databases with sift indexes succeeds" test $upgraded -eq 0
[[ $upgraded -eq 0 ]] || diag "$(cat "$(dir up)"/data/pg_upgrade_output.d/*/loadable_libraries.txt; tail -n 5 "$(dir up)"/data/pg_upgrade_output.d/*/log/pg_upgrade_server.log)"
node_start up || bail "upgraded cluster did not start"
for db in src plain custom upg; do
	check "upgraded $db verifies and keeps its results" test "$(vq up $db "SELECT fp() = seq()")" == t
done
mismatch=0
for db in src plain custom upg; do
	var=FP_$db
	[[ $(q up $db "SELECT fp() || ranked()") == "${!var}" ]] || mismatch=$((mismatch + 1))
done
check "scores and results are identical after pg_upgrade" test $mismatch -eq 0
q up src "INSERT INTO docs VALUES (7000, 'alpha3 postupgrade')" "VACUUM docs" >/dev/null
check "the upgraded cluster accepts writes and vacuum" test "$(vq up src "SELECT fp() = seq()")" == t
finish
