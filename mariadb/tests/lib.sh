#!/usr/bin/env bash
# Helpers of the functional tests of sift for MariaDB, sourced by tests/*.sh. The tests run against the private server of
# mariadb/server.sh (already started, SIFT_MDB_ROOT) in the database $DB, and print "ok name" or "FAIL name: expected, got".
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SRV=$HERE/server.sh
export SIFT_MDB_ROOT=${SIFT_MDB_ROOT:-/tmp/sift-mariadb}
DB=${DB:-ft}
fail=0 total=0

sql() { "$SRV" sql -N -B "$DB" -e "$1"; }
sqle() { "$SRV" sql -N -B "$DB" -e "$1" 2>&1; }   # with the error message, if any
check() {   # name expected actual
  total=$((total + 1))
  if [[ $2 == "$3" ]]; then echo "ok   $1"; else echo "FAIL $1: expected [$2] got [$3]"; fail=$((fail + 1)); fi
}
check_match() {   # name regex actual
  total=$((total + 1))
  if [[ $3 =~ $2 ]]; then echo "ok   $1"; else echo "FAIL $1: expected /$2/ got [$3]"; fail=$((fail + 1)); fi
}
# keys the index returns for a query (no join with the table): sorted, comma separated
idx() { sql "SELECT IFNULL(GROUP_CONCAT(id ORDER BY id), '') FROM JSON_TABLE(sift_matches('${2:-f}', '$1'), '\$[*]' COLUMNS (id BIGINT PATH '\$')) j"; }
# the top-k keys in rank order
top() { sql "SELECT IFNULL(GROUP_CONCAT(id ORDER BY n), '') FROM (SELECT sift_topk('${3:-f}', '$1', ${2:-10}) AS r) x, JSON_TABLE(x.r, '\$[*]' COLUMNS (n FOR ORDINALITY, id BIGINT PATH '\$.id')) j"; }
cnt() { sql "SELECT sift_count('${2:-f}', '$1')"; }
sync() { sql "CALL sift_sync('${1:-f}')" >/dev/null; }
stat() { sql "SELECT JSON_VALUE(sift_stats('${2:-f}'), '\$.$1')"; }
# the SQL baseline: ids of rows whose text contains every word (case-insensitive), by LIKE
like() { sql "SELECT IFNULL(GROUP_CONCAT(id ORDER BY id), '') FROM $1 WHERE $2"; }

fresh_db() {
  for i in "$@"; do "$SRV" sql -e "SELECT sift_drop('$i')" >/dev/null 2>&1; done   # indexes of a previous run live outside the database
  "$SRV" sql -e "DROP DATABASE IF EXISTS $DB; CREATE DATABASE $DB DEFAULT CHARACTER SET utf8mb4"
  "$SRV" sql "$DB" <"$HERE/sql/functions.sql"
  "$SRV" sql "$DB" <"$HERE/sql/sift.sql"
}

finish() {
  echo "$((total - fail))/$total ${NAME:-functional} checks passed"
  exit $((fail != 0))
}
