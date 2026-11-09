#!/usr/bin/env bash
# The real update scripts of the patch releases of 4.2 (the ones in sql/; extension_lifecycle writes empty ones of its own): a database that starts at 4.2, 4.2.1 or 4.2.2 reaches the
# default version with the objects of a fresh install, sift.table_find_stale_versions (4.2.2: plpgsql, STABLE, SQL only) is there after every path, and the way back to 4.2.1 (through
# the empty script back to 4.2) and forward again works: the script that adds a function runs a second time, so it replaces it.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
EXT=$(sed -n "s/^default_version *= *'\(.*\)'/\1/p" "$REPO/pg_sift.control")
plan 12
node_init ext 42 'autovacuum = off'
node_start ext || bail "cannot start"
MEMBERS="SELECT pg_describe_object(classid, objid, objsubid) || ' ' || coalesce(CASE classid WHEN 'pg_proc'::regclass THEN pg_get_functiondef(objid) END, '')
	FROM pg_depend WHERE refclassid = 'pg_extension'::regclass AND deptype = 'e' AND refobjid = (SELECT oid FROM pg_extension WHERE extname = 'pg_sift') ORDER BY 1"
FN="SELECT p.provolatile::text || l.lanname FROM pg_proc p JOIN pg_language l ON l.oid = p.prolang WHERE p.oid = to_regprocedure('sift.table_find_stale_versions(regclass,integer)')"
VER="SELECT extversion FROM pg_extension WHERE extname = 'pg_sift'"
db() { sql ext "DROP DATABASE IF EXISTS $1" "CREATE DATABASE $1" >/dev/null; }
q() { local d=$1; shift; PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir ext)/sock" -p "$(port ext)" -U postgres -d "$d" "${@/#/--command=}"; }

db fresh; q fresh "CREATE EXTENSION pg_sift" >/dev/null
fresh=$(q fresh "$MEMBERS")
check "a fresh install is at the default version $EXT" test "$(q fresh "$VER")" == "$EXT"
check "... and has sift.table_find_stale_versions, STABLE, plpgsql" test "$(q fresh "$FN")" == splpgsql
for from in 4.2 4.2.1 4.2.2; do
	d=from_${from//./_}; db "$d"
	q "$d" "CREATE EXTENSION pg_sift VERSION '$from'" "ALTER EXTENSION pg_sift UPDATE" >/dev/null
	check "a database created at $from and updated is at $EXT with the objects of a fresh install" test "$(q "$d" "$VER")" == "$EXT" -a "$(q "$d" "$MEMBERS")" == "$fresh"
	check "... and has sift.table_find_stale_versions, STABLE, plpgsql" test "$(q "$d" "$FN")" == splpgsql
done

db back; q back "CREATE EXTENSION pg_sift VERSION '4.2.2'" >/dev/null
check "the path from 4.2.2 to 4.2.1 is the empty script back to 4.2 and the update to 4.2.1" \
	test "$(q back "SELECT path FROM pg_extension_update_paths('pg_sift') WHERE source = '4.2.2' AND target = '4.2.1'")" == "4.2.2--4.2--4.2.1"
q back "ALTER EXTENSION pg_sift UPDATE TO '4.2.1'" >/dev/null
check "a database taken back to 4.2.1 is at 4.2.1" test "$(q back "$VER")" == 4.2.1
check "... and keeps sift.table_find_stale_versions, as it keeps sift.col_track_convert: the script back drops nothing" test "$(q back "$FN")|$(q back "SELECT to_regprocedure('sift.col_track_convert()') IS NOT NULL")" == "splpgsql|t"
q back "ALTER EXTENSION pg_sift UPDATE" >/dev/null
check "... and updated again is at $EXT with the objects of a fresh install" test "$(q back "$VER")" == "$EXT" -a "$(q back "$MEMBERS")" == "$fresh"
finish
