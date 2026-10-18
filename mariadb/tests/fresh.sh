#!/usr/bin/env bash
# Freshness and correctness: what a query sees before the queue is drained, LIMIT that is never short, rollback, the
# visibility of a REPEATABLE READ / READ COMMITTED reader, and counts equal to the SQL baseline (LIKE over the table).
#   mariadb/tests/fresh.sh        (private server of mariadb/server.sh already started)
set -uo pipefail
. "$(dirname "$0")/lib.sh"
NAME=fresh

# a long-lived client session on a pair of fifos, to hold a transaction open: s_start A; s_run A "stmt"; s_stop A
TMP=$(mktemp -d /tmp/sift-mdb-fresh.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
s_start() {
  mkfifo "$TMP/$1.in" "$TMP/$1.out"
  "$SRV" sql -N -B --unbuffered "$DB" <"$TMP/$1.in" >"$TMP/$1.out" 2>&1 &
  eval "exec {$1_in}>$TMP/$1.in {$1_out}<$TMP/$1.out"
}
s_run() {
  local in out line
  eval "in=\$$1_in out=\$$1_out"
  printf '%s;\nSELECT "--END--";\n' "$2" >&"$in"
  while IFS= read -r -t 20 line <&"$out"; do [[ $line == --END-- ]] && break; echo "$line"; done
}
s_stop() { local in; eval "in=\$$1_in"; eval "exec $in>&-"; }

fresh_db t tr lag
sql "CREATE TABLE t (id BIGINT PRIMARY KEY, body TEXT, grp INT)"
sql "INSERT INTO t SELECT seq, CONCAT('alpha doc', seq, IF(seq % 3 = 0, ' beta', ''), IF(seq % 5 = 0, ' gamma', '')), seq % 4 FROM seq_1_to_60"
sql "CALL sift_attach('t', 't', 'id', 't.body', 8)" >/dev/null
sync t

# --- no sync: every change is visible at once, equal to the SQL baseline
check "baseline count" "$(sql "SELECT COUNT(*) FROM t WHERE body LIKE '%beta%'")" "$(cnt beta t)"
sql "INSERT INTO t VALUES (100, 'alpha beta fresh', 1), (101, 'alpha gamma fresh', 2)"
check "insert without sync: beta" "$(like t "body LIKE '%beta%'")" "$(idx beta t)"
check "insert without sync: count" "$(sql "SELECT COUNT(*) FROM t WHERE body LIKE '%beta%'")" "$(cnt beta t)"
check "insert without sync: AND" "$(like t "body LIKE '%beta%' AND body LIKE '%gamma%'")" "$(idx 'beta AND gamma' t)"
sql "UPDATE t SET body = 'alpha beta gamma updated' WHERE id = 1"
check "update without sync: gamma" "$(like t "body LIKE '%gamma%'")" "$(idx gamma t)"
check "update without sync: updated" "1" "$(idx updated t)"
sql "DELETE FROM t WHERE id IN (3, 6, 100)"
check "delete without sync: beta" "$(like t "body LIKE '%beta%'")" "$(idx beta t)"
check "delete without sync: count" "$(sql "SELECT COUNT(*) FROM t WHERE body LIKE '%beta%'")" "$(cnt beta t)"
sql "UPDATE t SET body = 'alpha beta gamma updated again' WHERE id = 1"
check "second update of the same row, once" "1" "$(cnt 'updated AND again' t)"
sql "DELETE FROM t WHERE id = 1"
sql "INSERT INTO t VALUES (1, 'alpha reborn', 0)"
check "delete then insert of the same key" "1" "$(idx reborn t)"
check "the old text of a reborn key is gone" "0" "$(cnt updated t)"
sync t
check "after the drain the same answer" "$(like t "body LIKE '%beta%'")" "$(idx beta t)"
check "after the drain: reborn" "1" "$(idx reborn t)"

# --- LIMIT is never short: the best rows are deleted without a sync, the next ones fill the page
sql "INSERT INTO t SELECT 200 + seq, CONCAT('zeta ', REPEAT('zeta ', seq % 5)), 0 FROM seq_1_to_30"
sync t
best=$(top zeta 5 t)
sql "DELETE FROM t WHERE id IN ($best)"
check "top-10 after deleting the top-5 is full" 10 "$(top zeta 10 t | tr ',' '\n' | wc -l)"
check "top-10 holds no deleted row" 0 "$(top zeta 10 t | tr ',' '\n' | grep -cxF -f <(echo "$best" | tr ',' '\n'))"
check "the join with the table gives 10 as well" 10 "$(sql "SELECT COUNT(*) FROM (SELECT sift_topk('t', 'zeta', 10) AS r) x, JSON_TABLE(x.r, '\$[*]' COLUMNS (id BIGINT PATH '\$.id')) j JOIN t ON t.id = j.id")"
sql "INSERT INTO t SELECT 300 + seq, CONCAT('zeta zeta zeta zeta zeta ', seq), 0 FROM seq_1_to_3"
live=$(top zeta 10 t)
sync t
check "the ranking before the drain is the ranking after it" "$live" "$(top zeta 10 t)"

# --- rollback and savepoints leave no trace
sql "START TRANSACTION; INSERT INTO t VALUES (500, 'omega rolled', 0); UPDATE t SET body = 'omega changed' WHERE id = 2; ROLLBACK"
check "rollback: nothing found" "0" "$(cnt omega t)"
sql "START TRANSACTION; INSERT INTO t VALUES (501, 'omega kept', 0); SAVEPOINT s; INSERT INTO t VALUES (502, 'omega undone', 0); ROLLBACK TO s; COMMIT"
check "savepoint: only the kept row" "501" "$(idx omega t)"

# --- the isolation of the reader. The index reads the queue and the rows on its own local connection: it never returns
# the uncommitted rows of any session (the caller's own included), a committed row is found by the next statement of a
# READ COMMITTED or autocommit reader, and the join with the table applies the snapshot of the caller (what that
# transaction cannot see is dropped from the result).
J="FROM JSON_TABLE(sift_matches('t', 'sigma'), '\$[*]' COLUMNS (id BIGINT PATH '\$')) j"
G="SELECT IFNULL(GROUP_CONCAT(j.id ORDER BY j.id), '')"
s_start A; s_start B
s_run A "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ" >/dev/null
s_run A "START TRANSACTION" >/dev/null
s_run A "SELECT COUNT(*) FROM t" >/dev/null
s_run B "INSERT INTO t VALUES (600, 'sigma newrow', 0)" >/dev/null
check "RR: the table of the transaction does not have the new row" "0" "$(s_run A "SELECT COUNT(*) FROM t WHERE id = 600")"
check "RR: the join with the table is the snapshot" "" "$(s_run A "$G $J JOIN t ON t.id = j.id")"
s_run A "INSERT INTO t VALUES (601, 'sigma own', 0)" >/dev/null
check "RR: the own uncommitted write is not found by the index" "0" "$(s_run A "$G $J" | grep -c 601)"
check "RR: the table of the transaction has it" "1" "$(s_run A "SELECT COUNT(*) FROM t WHERE id = 601")"
s_run A "COMMIT" >/dev/null
check "after the commit both are found" "600,601" "$(s_run A "$G $J")"
s_run A "SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED" >/dev/null
s_run A "START TRANSACTION" >/dev/null
s_run B "INSERT INTO t VALUES (602, 'sigma second', 0)" >/dev/null
check "RC: the next statement sees the committed row" "600,601,602" "$(s_run A "$G $J")"
check "RC: and so does the join with the table" "600,601,602" "$(s_run A "$G $J JOIN t ON t.id = j.id")"
s_run B "START TRANSACTION" >/dev/null
s_run B "INSERT INTO t VALUES (603, 'sigma uncommitted', 0)" >/dev/null
check "an uncommitted row of another session is not found" "600,601,602" "$(s_run A "$G $J")"
s_run B "COMMIT" >/dev/null
check "found once committed" "600,601,602,603" "$(s_run A "$G $J")"
s_run A "COMMIT" >/dev/null
# a writing transaction that is still open does not stall the readers for long when a drain asks for a barrier
s_run A "START TRANSACTION" >/dev/null
s_run A "INSERT INTO t VALUES (604, 'sigma open', 0)" >/dev/null
start=$SECONDS
sync t
check "a drain does not wait for the open writer" "1" "$((SECONDS - start < 5))"
check "the readers are not blocked by it" "600,601,602,603" "$(sql "$G $J")"
s_run A "COMMIT" >/dev/null
sync t
check "the writer is drained after its commit" "600,601,602,603,604" "$(idx sigma t)"
s_stop A; s_stop B

# --- TRUNCATE fires no delete trigger: the rows vanish from the table, a rebuild drops them from the index
sql "CREATE TABLE tr (id BIGINT PRIMARY KEY, body TEXT)"
sql "INSERT INTO tr VALUES (1, 'kappa one'), (2, 'kappa two')"
sql "CALL sift_attach('tr', 'tr', 'id', 't.body', 8)" >/dev/null
sync tr
sql "TRUNCATE TABLE tr"
check "truncate: the top-k does not return the vanished rows" "" "$(top kappa 10 tr)"
sql "CALL sift_rebuild('tr')" >/dev/null
check "truncate: after a rebuild the count is 0" "0" "$(cnt kappa tr)"
sql "INSERT INTO tr VALUES (3, 'kappa three')"
check "truncate: new rows are found" "3" "$(idx kappa tr)"

# --- an index that does not read the queue (fresh=false) lags until the drain and says so
sql "CREATE TABLE lag (id BIGINT PRIMARY KEY, body TEXT)"
sql "INSERT INTO lag VALUES (1, 'lambda one')"
sql "CALL sift_attach_opts('lag', 'lag', 'id', 't.body', '{\"fresh\":false}')" >/dev/null
sync lag
sql "INSERT INTO lag VALUES (2, 'lambda two')"
check "fresh=false: the new row waits for the drain" "1" "$(idx lambda lag)"
sync lag
check "fresh=false: found after the drain" "1,2" "$(idx lambda lag)"

finish
