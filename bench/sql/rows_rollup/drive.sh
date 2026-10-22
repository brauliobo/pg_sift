#!/usr/bin/env bash
# drive.sh N HOST PORT [PAD [DIMS...]]: a database (DB, default rr) of N rows (gen.sql) on the cluster and the run of run.sql on it for the index without
# rollup_dims and for each DIMS (e.g. 'kind, sub'); REUSE=1 keeps the database of the last call.
here=$(cd "$(dirname "$0")" && pwd)
n=$1; host=$2; port=$3; pad=${4:-200}; shift $(($# < 4 ? $# : 4)); db=${DB:-rr}
P="psql -h $host -p $port -U postgres -X -q"
if [[ -z ${REUSE:-} ]]; then
	$P -d postgres -c "DROP DATABASE IF EXISTS $db" -c "CREATE DATABASE $db" >/dev/null
	$P -d "$db" -v n="$n" -v pad="$pad" -f "$here/gen.sql" >/dev/null
fi
for dims in "" "$@"; do
	$P -d "$db" -c "SET statement_timeout = '30s'" -v n="$n" -v dims="$dims" -f "$here/run.sql"
done
