#!/usr/bin/env bash
# Concurrency: N reader threads, M writer threads, a draining thread and a merging thread against one index at once, then
# the index must equal the table (tests/stress.c). SECONDS (default 20), READERS (8), WRITERS (4).
#   mariadb/tests/stress.sh
set -uo pipefail
. "$(dirname "$0")/lib.sh"
NAME=stress
SIFT_BUILD=${SIFT_BUILD:-/tmp/sift-mariadb-build}

fresh_db s
sql "CREATE TABLE s (id BIGINT PRIMARY KEY, body TEXT)"
sql "INSERT INTO s SELECT seq, CONCAT('common alpha seed', seq) FROM seq_1_to_200"
sql "CALL sift_attach_opts('s', 's', 'id', 't.body', '{\"dpp\":8,\"merge_fanin\":3}')" >/dev/null
sync s
out=$("$SIFT_BUILD/stress" "$SIFT_MDB_ROOT/mysql.sock" "$DB" s "${SECONDS_RUN:-20}" "${READERS:-8}" "${WRITERS:-4}" 2>&1)
echo "$out" | tail -3
check "stress run: index equals table, no error, top-10 always full" 0 "$(echo "$out" | grep -q 'violations 0 errors 0' && [[ $(echo "$out" | sed -n 's/.*index \([0-9]*\) table \([0-9]*\);.*/\1=\2/p' | awk -F= '{print ($1==$2)}') == 1 ]]; echo $?)"
check "the merge thread merged" 1 "$(sql "SELECT JSON_VALUE(sift_stats('s'), '\$.merges') > 0")"
sql "SELECT sift_merge('s', 'full')" >/dev/null
check "a full merge after the load leaves one segment" 1 "$(stat segments s)"
check "and the count is unchanged" "$(sql 'SELECT COUNT(*) FROM s')" "$(cnt common s)"
finish
