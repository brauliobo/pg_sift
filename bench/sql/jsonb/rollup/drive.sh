#!/usr/bin/env bash
# drive.sh N HOST PORT LABEL [OPTS [BUILDS]]: a database (DB, default jb) of N documents (gen.sql) on the cluster and the run of run.sql on it;
# REUSE=1 keeps the database of the last call. OPTS is appended to the WITH (...) of the index, e.g. ", rollup = 'off'".
here=$(cd "$(dirname "$0")" && pwd)
n=$1; host=$2; port=$3; label=$4; opts=${5:-}; builds=${6:-5}; db=${DB:-jb}
P="psql -h $host -p $port -U postgres -X -q"
if [[ -z ${REUSE:-} ]]; then
	$P -d postgres -c "DROP DATABASE IF EXISTS $db" -c "CREATE DATABASE $db" >/dev/null
	$P -d "$db" -c "CREATE EXTENSION pg_sift" >/dev/null
	$P -d "$db" -v n="$n" -v strs=0 -f "$here/../gen.sql" >/dev/null
fi
$P -d "$db" -v n="$n" -v variant="$label" -v builds="$builds" -v opts="$opts" -f "$here/run.sql"
