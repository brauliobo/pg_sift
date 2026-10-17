#!/usr/bin/env bash
# Functional tests of sift for MariaDB on a small table, against the private server of mariadb/server.sh (already started):
# queries, DML seen by the index at once (no drain), rollback, update and delete, drain, merge, restart, crash leftovers,
# idempotent drain, concurrent writers and the background drain by an EVENT.
#   mariadb/tests/functional.sh        exit status 1 if any check fails
set -uo pipefail
. "$(dirname "$0")/lib.sh"
NAME=functional
# the same through the base table, as an application joins it: rows that exist now
rows() { sql "SELECT IFNULL(GROUP_CONCAT(d.id ORDER BY d.id), '') FROM (SELECT sift_topk('f', '$1', 100) AS r) x, JSON_TABLE(x.r, '\$[*]' COLUMNS (id BIGINT PATH '\$.id')) j JOIN f d ON d.id = j.id"; }

fresh_db f g
sql "CREATE TABLE f (id BIGINT PRIMARY KEY, title VARCHAR(100), body TEXT, likes INT NOT NULL DEFAULT 0) ENGINE=InnoDB"
sql "INSERT INTO f (id, title, body) VALUES (1, 'Fruit', 'an apple a day'), (2, 'Tree', 'apple tree and pear tree'), (3, 'Pear', 'the pear is sweet'), (4, 'Bread', 'flour and water'), (5, 'Cider', 'apple cider vinegar')"
sql "CALL sift_attach('f', 'f', 'id', 'CONCAT_WS('' '', t.title, t.body)', 8)" >/dev/null
check "queue holds the existing rows" 5 "$(sql 'SELECT COUNT(*) FROM sift_q_f')"
check "before the first drain the queue is searched" "1,2,5" "$(idx apple)"
check "nothing is in a segment yet" 5 "$(stat pending)"
sync
check "drain moves the watermark" 0 "$(stat pending)"
check "the drained rows stay in the queue (a log)" 6 "$(sql 'SELECT COUNT(*) FROM sift_q_f')"
check "term" "1,2,5" "$(idx apple)"
check "AND" "2" "$(idx 'apple AND pear')"
check "OR" "1,2,3,5" "$(idx 'apple OR pear')"
check "NOT" "1,5" "$(idx 'apple AND NOT tree')"
check "phrase (positions)" "2" "$(idx '\"pear tree\"')"
check "phrase wrong order" "" "$(idx '\"tree pear\"')"
check "prefix" "2,3" "$(idx 'pea*')"
check "group" "3,4" "$(idx '{sweet flour}')"
check "field text includes the title" "3" "$(idx 'pear AND sweet')"
check "count" "3" "$(cnt apple)"
check "boost changes the order" "5" "$(sql "SELECT JSON_VALUE(sift_topk('f', 'cider^10 OR apple', 1), '\$[0].id')")"
check "top-1 of apple is the shortest document" "5" "$(sql "SELECT JSON_VALUE(sift_topk('f', 'apple vinegar', 1), '\$[0].id')")"
check "syntax error reaches the client" "ERROR" "$(sqle "SELECT sift_count('f', 'apple AND')" | grep -o ERROR)"
check "regular expression" "1,2,5" "$(idx '/app.e/')"
check "phrase under OR" "2,3" "$(idx '\"pear tree\" OR \"pear is\"')"
check "phrase under NOT" "3" "$(idx 'pear AND NOT \"pear tree\"')"
check "phrase under NOT: count" "1" "$(cnt 'pear AND NOT \"pear tree\"')"
check "phrase under NOT: top-k" "3" "$(top 'pear AND NOT \"pear tree\"')"

# --- rollback and the queue
before=$(sql 'SELECT COUNT(*) FROM sift_q_f')
sql "START TRANSACTION; INSERT INTO f (id, title, body) VALUES (6, 'Rolled', 'apple rolled back'); ROLLBACK"
check "rolled back insert leaves no queue row" "$before" "$(sql 'SELECT COUNT(*) FROM sift_q_f')"
check "rolled back insert is not found" "1,2,5" "$(idx apple)"
sql "INSERT INTO f (id, title, body) VALUES (7, 'Fresh', 'apple fresh')"
check "committed insert is found at once" "1,2,5,7" "$(idx apple)"
check "pending queue rows" 1 "$(stat pending)"
sync
check "committed insert after the drain" "1,2,5,7" "$(idx apple)"

# --- update: the new text only, at once
sql "UPDATE f SET body = 'a pear a day' WHERE id = 1"
check "update queues one row" 1 "$(stat pending)"
check "the old text does not hit" "2,5,7" "$(idx apple)"
check "the new text is found" "1,2,3" "$(idx pear)"
sync
check "after the drain the old text is gone" "2,5,7" "$(idx apple)"
check "after the drain the new text is found" "1,2,3" "$(idx pear)"
sql "UPDATE f SET likes = likes + 1 WHERE id = 1"
check "update of another column queues nothing" 0 "$(stat pending)"
sql "UPDATE f SET id = 10 WHERE id = 5"
check "primary key change moves the document at once" "2,7,10" "$(idx apple)"
sync
check "primary key change moves the document" "2,7,10" "$(idx apple)"

# --- delete: gone at once from the index and from the join
sql "DELETE FROM f WHERE id = 2"
check "deleted row is not in the index" "7,10" "$(idx apple)"
check "joined with the table it is gone as well" "7,10" "$(rows apple)"
sync
check "deleted row is not returned after the drain" "7,10" "$(idx apple)"
check "count after the delete" 2 "$(cnt apple)"
check "dead documents are counted" 3 "$(stat dead)"

# --- merge, restart, orphan files
before=$(idx 'apple OR pear OR flour')
segs=$(stat segments)
check "segments before the merge" 4 "$segs"
sql "SELECT sift_merge('f', 'full')" >/dev/null
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

# --- idempotent drain: the segment is committed, the same rows are fed again
sql "INSERT INTO f (id, title, body) VALUES (30, 'Twice', 'banana twice')"
sql "CALL sift_sync('f')" >/dev/null
check "first drain applied the row" 0 "$(stat pending)"
w=$(sql "SELECT MAX(seq) FROM sift_q_f WHERE op = 'B'")
drain="SELECT sift_drain('f', q.seq, q.op, q.pk, CONCAT_WS(' ', t.title, t.body)) FROM sift_q_f q LEFT JOIN f t ON t.id = q.pk WHERE q.seq <= $w"
check "a repeated drain applies nothing" 0 "$(sql "$drain")"
check "no duplicate document" 1 "$(cnt banana)"

# --- kill -9 right after a drain: the committed state is the one that comes back
sql "INSERT INTO f (id, title, body) VALUES (31, 'Crash', 'cherry crash')"
sync
kill -9 "$(cat "$SIFT_MDB_ROOT/started.pid")"; sleep 1
"$SRV" start
check "kill -9: drained row is there" "31" "$(idx cherry)"
sql "INSERT INTO f (id, title, body) VALUES (32, 'Crash2', 'cherry two')"
check "kill -9: the queue survives in InnoDB" "31,32" "$(idx cherry)"
sync
check "kill -9: queue drained after restart" "31,32" "$(idx cherry)"

# --- concurrent writers while draining
sql "CREATE TABLE g (id BIGINT PRIMARY KEY AUTO_INCREMENT, body TEXT)"
sql "CALL sift_attach('g', 'g', 'id', 't.body', 8)" >/dev/null
for w in 1 2 3 4; do
  ( for i in $(seq 1 150); do "$SRV" sql -N -B "$DB" -e "INSERT INTO g (body) VALUES ('stress w$w item$i common')" >/dev/null; done ) &
done
for _ in $(seq 1 30); do sql "CALL sift_sync('g')" >/dev/null; sleep 0.1; done
wait
sql "CALL sift_sync('g')" >/dev/null
check "concurrent writers: every committed row is indexed once" 600 "$(cnt common g)"
check "concurrent writers: no row is missing from the table side" 600 "$(sql 'SELECT COUNT(*) FROM g')"

# --- background drain by an EVENT
sql "SET GLOBAL event_scheduler = ON"
sql "CALL sift_schedule('g', 1)"
sql "INSERT INTO g (body) VALUES ('event driven row')"
sleep 3.5
check "event drains the queue in the background" 0 "$(stat pending g)"
check "event: the row is indexed" 1 "$(cnt 'event AND driven' g)"
sql "DROP EVENT sift_ev_g; SET GLOBAL event_scheduler = OFF"

# --- the other ways to write a row: upsert and replace go through the same triggers
sql "INSERT INTO g (id, body) VALUES (9001, 'upsert alpha')"
sql "INSERT INTO g (id, body) VALUES (9001, 'upsert beta') ON DUPLICATE KEY UPDATE body = VALUES(body)"
check "on duplicate key update: the new text only" "0 1" "$(cnt alpha g) $(cnt beta g)"
sql "REPLACE INTO g (id, body) VALUES (9001, 'upsert gamma')"
check "replace: the new text only" "0 1" "$(cnt beta g) $(cnt gamma g)"
sync g
check "replace: the same after the drain" "0 1" "$(cnt beta g) $(cnt gamma g)"
sql "INSERT INTO g (id, body) SELECT 9100 + seq, 'bulk delta' FROM seq_1_to_5"
check "insert select: five rows at once" 5 "$(cnt delta g)"

# --- several indexes in one server process share the mapping of the segments
check "two indexes: the first one still answers" "7,10" "$(idx apple)"
check "two indexes: the second one answers" "601" "$(cnt 'stress OR event' g)"
sql "CALL sift_sync('f'); CALL sift_detach('g')" >/dev/null
check "dropping an index leaves the other one alone" "7,10" "$(idx apple)"

finish
