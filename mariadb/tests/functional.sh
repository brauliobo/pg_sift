#!/usr/bin/env bash
# Functional tests of sift for MariaDB on a small table, against the private server of mariadb/server.sh (already started):
# queries, DML through the queue and triggers, drain, visibility rules, rollback, merge, restart, crash leftovers,
# idempotent drain, concurrent writers and the background drain by an EVENT.
#   mariadb/tests/functional.sh        exit status 1 if any check fails
set -uo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
SRV="$HERE/server.sh"
export SIFT_MDB_ROOT=${SIFT_MDB_ROOT:-/tmp/sift-mariadb}
fail=0 total=0

sql() { "$SRV" sql -N -B ft -e "$1"; }
check() {   # name expected actual
  total=$((total + 1))
  if [[ $2 == "$3" ]]; then echo "ok   $1"; else echo "FAIL $1: expected [$2] got [$3]"; fail=$((fail + 1)); fi
}
# keys the index returns (no join with the table): sorted, comma separated
idx() { sql "SELECT IFNULL(GROUP_CONCAT(id ORDER BY id), '') FROM JSON_TABLE(sift_matches('f', '$1'), '\$[*]' COLUMNS (id BIGINT PATH '\$')) j"; }
# the same through the base table, as an application joins it: rows that exist now
rows() { sql "SELECT IFNULL(GROUP_CONCAT(d.id ORDER BY d.id), '') FROM JSON_TABLE(sift_topk('f', '$1', 100), '\$[*]' COLUMNS (id BIGINT PATH '\$.id')) j JOIN f d ON d.id = j.id"; }
sync() { sql "CALL sift_sync('f')" >/dev/null; }
stat() { sql "SELECT JSON_VALUE(sift_stats('f'), '\$.$1')"; }

for i in f g; do "$SRV" sql -e "SELECT sift_drop('$i')" >/dev/null 2>&1; done   # indexes of a previous run live outside the database
"$SRV" sql -e "DROP DATABASE IF EXISTS ft; CREATE DATABASE ft DEFAULT CHARACTER SET utf8mb4"
"$SRV" sql ft <"$HERE/sql/functions.sql"
"$SRV" sql ft <"$HERE/sql/sift.sql"
sql "CREATE TABLE f (id BIGINT PRIMARY KEY, title VARCHAR(100), body TEXT, likes INT NOT NULL DEFAULT 0) ENGINE=InnoDB"
sql "INSERT INTO f (id, title, body) VALUES (1, 'Fruit', 'an apple a day'), (2, 'Tree', 'apple tree and pear tree'), (3, 'Pear', 'the pear is sweet'), (4, 'Bread', 'flour and water'), (5, 'Cider', 'apple cider vinegar')"
sql "CALL sift_attach('f', 'f', 'id', 'CONCAT_WS('' '', t.title, t.body)', 8)" >/dev/null
check "queue holds the existing rows" 5 "$(sql 'SELECT COUNT(*) FROM sift_q_f')"
check "before the first drain nothing is indexed" "" "$(idx apple)"
sync
check "drain empties the queue" 0 "$(sql 'SELECT COUNT(*) FROM sift_q_f')"
check "term" "1,2,5" "$(idx apple)"
check "AND" "2" "$(idx 'apple AND pear')"
check "OR" "1,2,3,5" "$(idx 'apple OR pear')"
check "NOT" "1,5" "$(idx 'apple AND NOT tree')"
check "phrase (positions)" "2" "$(idx '\"pear tree\"')"
check "phrase wrong order" "" "$(idx '\"tree pear\"')"
check "prefix" "2,3" "$(idx 'pea*')"
check "group" "3,4" "$(idx '{sweet flour}')"
check "field text includes the title" "3" "$(idx 'pear AND sweet')"
check "count" "3" "$(sql "SELECT sift_count('f', 'apple')")"
check "boost changes the order" "5" "$(sql "SELECT JSON_VALUE(sift_topk('f', 'cider^10 OR apple', 1), '\$[0].id')")"
check "top-1 of apple is the shortest document" "5" "$(sql "SELECT JSON_VALUE(sift_topk('f', 'apple vinegar', 1), '\$[0].id')")"
check "syntax error reaches the client" "ERROR" "$("$SRV" sql -N -B ft -e "SELECT sift_count('f', 'apple AND')" 2>&1 | grep -o ERROR)"

# --- rollback and the lag of the queue
sql "START TRANSACTION; INSERT INTO f (id, title, body) VALUES (6, 'Rolled', 'apple rolled back'); ROLLBACK"
check "rolled back insert leaves no queue row" 0 "$(sql 'SELECT COUNT(*) FROM sift_q_f')"
sync
check "rolled back insert is not indexed" "1,2,5" "$(idx apple)"
sql "INSERT INTO f (id, title, body) VALUES (7, 'Fresh', 'apple fresh')"
check "committed insert waits for the drain" "1,2,5" "$(idx apple)"
sync
check "committed insert after the drain" "1,2,5,7" "$(idx apple)"

# --- update: stale until drained, then the new text only
sql "UPDATE f SET body = 'a pear a day' WHERE id = 1"
check "update queues one row" 1 "$(sql 'SELECT COUNT(*) FROM sift_q_f')"
check "before the drain the old text still hits" "1,2,5,7" "$(idx apple)"
check "before the drain the new text is not found" "3" "$(idx 'pear AND sweet')"
sync
check "after the drain the old text is gone" "2,5,7" "$(idx apple)"
check "after the drain the new text is found" "1,2,3" "$(idx pear)"
sql "UPDATE f SET likes = likes + 1 WHERE id = 1"
check "update of another column queues nothing" 0 "$(sql 'SELECT COUNT(*) FROM sift_q_f')"
sql "UPDATE f SET id = 10 WHERE id = 5"
sync
check "primary key change moves the document" "2,7,10" "$(idx apple)"

# --- delete: the join hides it at once, the index forgets it at the drain
sql "DELETE FROM f WHERE id = 2"
check "deleted row is still in the index before the drain" "2,7,10" "$(idx apple)"
check "joined with the table it is gone at once" "7,10" "$(rows apple)"
sync
check "deleted row is not returned after the drain" "7,10" "$(idx apple)"
check "count after the delete" 2 "$(sql "SELECT sift_count('f', 'apple')")"
check "dead documents are counted" 3 "$(stat dead)"

# --- merge, restart, orphan files
before=$(idx 'apple OR pear OR flour')
check "segments before the merge" 4 "$(stat segments)"
sql "SELECT sift_merge('f')" >/dev/null
check "merge leaves one segment" 1 "$(stat segments)"
check "merge drops the dead documents" 0 "$(stat dead)"
check "merge keeps the results" "$before" "$(idx 'apple OR pear OR flour')"
ls "$SIFT_MDB_ROOT/data/.sift/f" | grep -c '^seg-' | { read -r n; check "merge removes the old segment files" 1 "$n"; }
echo garbage >"$SIFT_MDB_ROOT/data/.sift/f/seg-99.sift"; echo garbage >"$SIFT_MDB_ROOT/data/.sift/f/new-3.tmp"
"$SRV" restart
check "restart: results" "$before" "$(idx 'apple OR pear OR flour')"
check "restart: phrase" "3" "$(idx '\"pear is\"')"
check "restart: orphan files are removed" 0 "$(ls "$SIFT_MDB_ROOT/data/.sift/f" | grep -c 'seg-99\|new-3')"
sql "INSERT INTO f (id, title, body) VALUES (20, 'After', 'apple after restart')"
sync
check "restart: new rows are drained into the reopened index" "7,10,20" "$(idx apple)"
sql "DELETE FROM f WHERE id = 20"
sync
"$SRV" stop; "$SRV" start
check "restart: deletes survive" "7,10" "$(idx apple)"

# --- idempotent drain: the segment is committed but the queue rows are still there (a crash before the DELETE)
sql "INSERT INTO f (id, title, body) VALUES (30, 'Twice', 'banana twice')"
w=$(sql "SELECT MAX(seq) FROM sift_q_f")
drain="SELECT sift_drain('f', q.seq, q.op, q.pk, CONCAT_WS(' ', t.title, t.body)) FROM sift_q_f q LEFT JOIN f t ON t.id = q.pk WHERE q.seq <= $w"
check "first drain applies the row" 1 "$(sql "$drain")"
check "repeated drain applies nothing" 0 "$(sql "$drain")"
check "no duplicate document" 1 "$(sql "SELECT sift_count('f', 'banana')")"
sql "DELETE FROM sift_q_f WHERE seq <= $w"

# --- kill -9 right after a drain: the committed state is the one that comes back
sql "INSERT INTO f (id, title, body) VALUES (31, 'Crash', 'cherry crash')"
sync
kill -9 "$(cat "$SIFT_MDB_ROOT/started.pid")"; sleep 1
"$SRV" start
check "kill -9: drained row is there" "31" "$(idx cherry)"
sql "INSERT INTO f (id, title, body) VALUES (32, 'Crash2', 'cherry two')"
check "kill -9: the queue survives in InnoDB" 1 "$(sql 'SELECT COUNT(*) FROM sift_q_f')"
sync
check "kill -9: queue drained after restart" "31,32" "$(idx cherry)"

# --- concurrent writers while draining
sql "CREATE TABLE g (id BIGINT PRIMARY KEY AUTO_INCREMENT, body TEXT)"
sql "CALL sift_attach('g', 'g', 'id', 't.body', 8)" >/dev/null
for w in 1 2 3 4; do
  ( for i in $(seq 1 150); do "$SRV" sql -N -B ft -e "INSERT INTO g (body) VALUES ('stress w$w item$i common')" >/dev/null; done ) &
done
for _ in $(seq 1 30); do sql "CALL sift_sync('g')" >/dev/null; sleep 0.1; done
wait
sql "CALL sift_sync('g')" >/dev/null
check "concurrent writers: every committed row is indexed once" 600 "$(sql "SELECT sift_count('g', 'common')")"
check "concurrent writers: no row is missing from the table side" 600 "$(sql 'SELECT COUNT(*) FROM g')"

# --- background drain by an EVENT
sql "SET GLOBAL event_scheduler = ON"
sql "CREATE EVENT sift_sync_g ON SCHEDULE EVERY 1 SECOND DO CALL sift_sync('g')"
sql "INSERT INTO g (body) VALUES ('event driven row')"
sleep 3.5
check "event drains the queue in the background" 1 "$(sql "SELECT sift_count('g', 'event AND driven')")"
sql "DROP EVENT sift_sync_g; SET GLOBAL event_scheduler = OFF"

# --- several indexes in one server process share the mapping of the segments
check "two indexes: the first one still answers" "7,10" "$(idx apple)"
check "two indexes: the second one answers" "600" "$(sql "SELECT sift_count('g', 'stress AND common')")"
sql "CALL sift_sync('f'); CALL sift_detach('g')" >/dev/null
check "dropping an index leaves the other one alone" "7,10" "$(idx apple)"

echo "$((total - fail))/$total functional checks passed"
exit $((fail != 0))
