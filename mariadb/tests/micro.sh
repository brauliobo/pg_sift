#!/usr/bin/env bash
# The live segment as the queue grows: every write is followed by a query, and the index (matches, count, counts and matches
# under a key filter, top-k) must equal the SQL baseline (LIKE over the table) whether the row is in the segments, in the
# primary live segment or in its micro segment; inserts, updates and deletes of rows of each.
#   mariadb/tests/micro.sh
set -uo pipefail
. "$(dirname "$0")/lib.sh"
NAME=micro

fresh_db mc
sql "CREATE TABLE mc (id BIGINT PRIMARY KEY, body TEXT)"
sql "INSERT INTO mc SELECT seq, CONCAT('common doc', seq, IF(seq % 5 = 0, ' five', ''), IF(seq % 7 = 0, ' seven', '')) FROM seq_1_to_400"
sql "CALL sift_attach_opts('mc', 'mc', 'id', 't.body', '{\"dpp\":8}')" >/dev/null
sync mc
sql "INSERT INTO mc SELECT seq, CONCAT('common doc', seq, ' five') FROM seq_401_to_700"
same() {   # name word
  local want got
  want=$(like mc "body LIKE '%$2%'"); got=$(idx "$2" mc)
  check "$1: matches of $2" "$want" "$got"
  check "$1: count of $2" "$(sql "SELECT COUNT(*) FROM mc WHERE body LIKE '%$2%'")" "$(cnt "$2" mc)"
}
filtered() {   # name word
  local want got
  want=$(sql "SELECT COUNT(*) FROM mc WHERE body LIKE '%$2%' AND id % 3 = 0")
  got=$(sql "SELECT sift_count('mc', '$2', (SELECT sift_idset(id) FROM mc WHERE id % 3 = 0))")
  check "$1: filtered count of $2" "$want" "$got"
  want=$(sql "SELECT IFNULL(GROUP_CONCAT(id ORDER BY id), '') FROM mc WHERE body LIKE '%$2%' AND id % 3 = 0")
  got=$(sql "SELECT IFNULL(GROUP_CONCAT(j.id ORDER BY j.id), '') FROM JSON_TABLE(sift_matches('mc', '$2', (SELECT sift_idset(id) FROM mc WHERE id % 3 = 0)), '\$[*]' COLUMNS (id BIGINT PATH '\$')) j")
  check "$1: filtered matches of $2" "$want" "$got"
}
step() {   # name statement
  sql "$2"
  same "$1" five; same "$1" common; filtered "$1" five
  check "$1: top-10 is full" 10 "$(sql "SELECT JSON_LENGTH(sift_topk('mc', 'common', 10))")"
}
step "primary" "INSERT INTO mc VALUES (701, 'common doc701 five')"
for i in 1 2 3 4 5 6; do step "micro insert $i" "INSERT INTO mc VALUES (701 + $i, 'common doc five extra$i')"; done
step "update of a micro row" "UPDATE mc SET body = 'common changed seven' WHERE id = 703"
step "update of a primary row" "UPDATE mc SET body = 'common changed seven' WHERE id = 450"
step "update of a segment row" "UPDATE mc SET body = 'common changed five' WHERE id = 100"
step "delete of a micro row" "DELETE FROM mc WHERE id = 705"
step "delete of a primary row" "DELETE FROM mc WHERE id = 455"
step "delete of a segment row" "DELETE FROM mc WHERE id = 105"
step "reinsert of a deleted row" "INSERT INTO mc VALUES (705, 'common back five')"
for i in $(seq 1 90); do sql "INSERT INTO mc VALUES (800 + $i, 'common grow five')"; same "grow $i" five >/dev/null; done
step "after the micro segment was merged into a new primary" "UPDATE mc SET body = 'common again five' WHERE id = 850"
check "a drain applies it all" "$(like mc "body LIKE '%five%'")" "$(sync mc; idx five mc)"
step "after the drain" "INSERT INTO mc VALUES (900, 'common last five')"
finish
