#!/usr/bin/env bash
# Filter pushdown and facets against the SQL baseline: the filter is a bitmap of keys (sift_idset), a JSON array of keys or a
# SQL predicate (sift_search / sift_filter); it restricts the ranked scan, so a rare predicate still fills the page. Facets
# are the counts of the matches per value, equal to GROUP BY over the table.
#   mariadb/tests/filters.sh
set -uo pipefail
. "$(dirname "$0")/lib.sh"
NAME=filters
KEYS="SELECT IFNULL(GROUP_CONCAT(j.id ORDER BY j.id), '') FROM JSON_TABLE("
JT="'\$[*]' COLUMNS (id BIGINT PATH '\$')) j"

fresh_db fl
sql "CREATE TABLE fl (id BIGINT PRIMARY KEY, body TEXT, cat INT, likes INT)"
sql "INSERT INTO fl SELECT seq, CONCAT('common doc', seq, IF(seq % 100 = 0, ' rare', ''), IF(seq % 9 = 0, ' nine', '')), seq % 7, seq % 50 FROM seq_1_to_3000"
sql "CALL sift_attach('fl', 'fl', 'id', 't.body', 32)" >/dev/null
sync fl
set_of() { sql "SELECT IFNULL(GROUP_CONCAT(id ORDER BY id), '') FROM fl WHERE $1"; }
fm() { sql "$KEYS sift_matches('fl', '$1', $2), $JT"; }     # matches under a filter expression
ft() { sql "$KEYS sift_topk('fl', '$1', $2, $3), '\$[*]' COLUMNS (id BIGINT PATH '\$.id')) j"; }

# bitmap from a subquery
check "bitmap: matches under a predicate" "$(set_of "cat = 3 AND body LIKE '%rare%'")" "$(fm rare "(SELECT sift_idset(id) FROM fl WHERE cat = 3)")"
check "bitmap: matches of a rare term, every category" "$(set_of "body LIKE '%rare%'")" "$(fm rare "(SELECT sift_idset(id) FROM fl)")"
check "bitmap: nothing passes the filter" "" "$(fm rare "(SELECT sift_idset(id) FROM fl WHERE id < 0)")"
check "bitmap: count under a filter" "$(sql "SELECT COUNT(*) FROM fl WHERE likes = 7 AND body LIKE '%nine%'")" "$(sql "SELECT sift_count('fl', 'nine', (SELECT sift_idset(id) FROM fl WHERE likes = 7))")"
# the page is full although the filter is selective: the filter is applied inside the ranked scan
want=$(sql "SELECT LEAST(10, COUNT(*)) FROM fl WHERE likes = 7 AND cat = 0")
check "bitmap: a selective filter still fills the page" "$want" "$(sql "SELECT JSON_LENGTH(sift_topk('fl', 'common', 10, (SELECT sift_idset(id) FROM fl WHERE likes = 7 AND cat = 0)))")"
check "bitmap: and every row passes it" "0" "$(sql "SELECT COUNT(*) FROM (SELECT sift_topk('fl', 'common', 50, (SELECT sift_idset(id) FROM fl WHERE likes = 7)) AS r) x, JSON_TABLE(x.r, '\$[*]' COLUMNS (id BIGINT PATH '\$.id')) j JOIN fl ON fl.id = j.id WHERE fl.likes <> 7")"
# JSON array of keys
check "id list: matches" "100,200" "$(fm rare "'[100, 200, 201]'")"
check "id list: top-k" "100,300" "$(ft rare 2 "'[300, 100, 7]'")"
check "id list: a malformed list is an error" "ERROR" "$(sqle "SELECT sift_matches('fl', 'rare', '1,2')" | grep -o ERROR)"
# SQL predicate (sift_search / sift_filter)
check "predicate: sift_search rows all satisfy it" "0" "$(sql "CALL sift_search('fl', 'common', 20, 't.cat = 2 AND t.likes > 40')" | awk -F'\t' '$3 != 2 || $4 <= 40' | wc -l)"
check "predicate: sift_search returns a full page" "$(set_of "cat = 2 AND likes > 40" | tr ',' '\n' | head -20 | wc -l)" "$(sql "CALL sift_search('fl', 'common', 20, 't.cat = 2 AND t.likes > 40')" | wc -l)"
check "predicate: without a predicate" "10" "$(sql "CALL sift_search('fl', 'common', 10, NULL)" | wc -l)"
check "predicate: sift_filter leaves the bitmap in @sift_f" "$(set_of "cat = 5 AND body LIKE '%rare%'")" "$(sql "CALL sift_filter('fl', 't.cat = 5'); $KEYS sift_matches('fl', 'rare', @sift_f), $JT")"

# a filter sees the rows of the queue as well
sql "INSERT INTO fl VALUES (5000, 'common rare fresh', 3, 7), (5001, 'common rare other', 4, 7)"
check "fresh rows: the filter is built from the table, the matches from the queue" "$(set_of "cat = 3 AND body LIKE '%rare%'")" "$(fm rare "(SELECT sift_idset(id) FROM fl WHERE cat = 3)")"
sql "DELETE FROM fl WHERE id = 100"
check "fresh rows: a deleted row is not returned though the filter holds it" "$(set_of "cat = 3 AND body LIKE '%rare%'")" "$(fm rare "(SELECT sift_idset(id) FROM fl WHERE cat = 3 OR id = 100)")"
sync fl

# facets against GROUP BY
facets() { sql "CALL sift_facets('fl', '$1', '$2', $3)" | paste -sd';'; }
group_by() { sql "SELECT $1 AS value, COUNT(*) AS n FROM fl WHERE $2 ${3:-} GROUP BY value ORDER BY n DESC, value" | paste -sd';'; }
check "facets: by category" "$(group_by cat "body LIKE '%nine%'")" "$(facets nine t.cat NULL)"
check "facets: by an expression" "$(group_by "likes DIV 10" "body LIKE '%nine%'")" "$(facets nine 't.likes DIV 10' NULL)"
check "facets: with a filter" "$(group_by cat "body LIKE '%nine%'" "AND likes < 10")" "$(facets nine t.cat "'t.likes < 10'")"
check "facets: of every match" "$(group_by cat "1")" "$(facets common t.cat NULL)"
check "facets: a term with a few matches (fresh rows included)" "$(group_by cat "body LIKE '%rare%'")" "$(facets rare t.cat NULL)"
check "facets: nothing matches" "" "$(facets nosuchword t.cat NULL)"

finish
