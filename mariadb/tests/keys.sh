#!/usr/bin/env bash
# Text, UUID and composite primary keys: a key table (sift_k_<name>, kept by the triggers) maps them to dense integers.
# Searches go through sift_search (joins the integers back to the rows); insert / update / delete / key change are seen
# at once and again after the drain; the integer key keeps its fast path (no key table).
#   mariadb/tests/keys.sh
set -uo pipefail
. "$(dirname "$0")/lib.sh"
NAME=keys
# the primary keys of the rows sift_search returns, in rank order: the first column(s) of the table
found() { sql "CALL sift_search('$1', '$2', ${3:-20}, NULL)" | cut -f"${4:-1}" | tr '\n' ',' | sed 's/,$//'; }
sorted() { found "$@" | tr ',' '\n' | sort | paste -sd,; }

fresh_db kt ku kc ki
sql "CREATE TABLE kt (code VARCHAR(40) PRIMARY KEY, body TEXT)"
sql "INSERT INTO kt VALUES ('a-1', 'red apple'), ('b-2', 'green apple'), ('c-3', 'red pear')"
sql "CALL sift_attach('kt', 'kt', 'code', 't.body', 8)" >/dev/null
check "text key: key table is created" 1 "$(sql "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = '$DB' AND table_name = 'sift_k_kt'")"
check "text key: existing rows are found before the drain" "a-1,b-2" "$(sorted kt apple)"
sync kt
check "text key: after the drain" "a-1,b-2" "$(sorted kt apple)"
check "text key: AND" "a-1" "$(sorted kt 'red AND apple')"
check "text key: count" 2 "$(cnt apple kt)"
sql "INSERT INTO kt VALUES ('d-4', 'apple tart')"
check "text key: insert at once" "a-1,b-2,d-4" "$(sorted kt apple)"
sql "UPDATE kt SET body = 'plain bread' WHERE code = 'a-1'"
check "text key: update at once" "b-2,d-4" "$(sorted kt apple)"
sql "UPDATE kt SET code = 'z-9' WHERE code = 'b-2'"
check "text key: the key changes" "d-4,z-9" "$(sorted kt apple)"
sql "DELETE FROM kt WHERE code = 'd-4'"
check "text key: delete at once" "z-9" "$(sorted kt apple)"
sync kt
check "text key: after the drain" "z-9" "$(sorted kt apple)"
sql "SELECT sift_merge('kt', 'full')" >/dev/null
check "text key: after a full merge" "z-9" "$(sorted kt apple)"
check "text key: the scores come with the rows" 1 "$(sql "CALL sift_search('kt', 'apple', 5, NULL)" | awk -F'\t' '$NF > 0' | wc -l)"
sql "INSERT INTO kt VALUES ('z-9x', 'apple clone')"
check "text key: a prefix of another key is another row" "z-9,z-9x" "$(sorted kt apple)"
check "text key: the filter by a SQL predicate" "z-9x" "$(sql "CALL sift_search('kt', 'apple', 5, 't.code LIKE ''%x''')" | cut -f1)"

# UUID
sql "CREATE TABLE ku (id UUID PRIMARY KEY, body TEXT)"
sql "INSERT INTO ku VALUES ('11111111-1111-1111-1111-111111111111', 'blue whale'), ('22222222-2222-2222-2222-222222222222', 'blue shark')"
sql "CALL sift_attach('ku', 'ku', 'id', 't.body', 8)" >/dev/null
sync ku
check "uuid key" "11111111-1111-1111-1111-111111111111,22222222-2222-2222-2222-222222222222" "$(sorted ku blue)"
check "uuid key: one" "22222222-2222-2222-2222-222222222222" "$(sorted ku shark)"
sql "DELETE FROM ku WHERE id = '22222222-2222-2222-2222-222222222222'"
check "uuid key: delete at once" "11111111-1111-1111-1111-111111111111" "$(sorted ku blue)"

# composite (integer + text)
sql "CREATE TABLE kc (tenant INT, slug VARCHAR(20), body TEXT, PRIMARY KEY (tenant, slug))"
sql "INSERT INTO kc VALUES (1, 'a', 'cold soup'), (2, 'a', 'cold tea'), (1, 'b', 'hot soup')"
sql "CALL sift_attach('kc', 'kc', 'tenant,slug', 't.body', 8)" >/dev/null
sync kc
check "composite key: the same slug in two tenants" "1	a,2	a" "$(sql "CALL sift_search('kc', 'cold', 5, NULL)" | cut -f1,2 | sort | paste -sd,)"
sql "UPDATE kc SET body = 'warm tea' WHERE tenant = 2 AND slug = 'a'"
check "composite key: update at once" "1	a" "$(sql "CALL sift_search('kc', 'cold', 5, NULL)" | cut -f1,2)"
sql "INSERT INTO kc VALUES (3, 'a', 'cold rain')"
check "composite key: insert at once" "1	a,3	a" "$(sql "CALL sift_search('kc', 'cold', 5, NULL)" | cut -f1,2 | sort | paste -sd,)"
sql "DELETE FROM kc WHERE tenant = 1"
check "composite key: delete at once" "3	a" "$(sql "CALL sift_search('kc', 'cold', 5, NULL)" | cut -f1,2)"
sync kc
check "composite key: after the drain" "3	a" "$(sql "CALL sift_search('kc', 'cold', 5, NULL)" | cut -f1,2)"
"$SRV" restart
check "composite key: after a restart" "3	a" "$(sql "CALL sift_search('kc', 'cold', 5, NULL)" | cut -f1,2)"
sql "CALL sift_rebuild('kc')" >/dev/null
check "composite key: after a rebuild" "3	a" "$(sql "CALL sift_search('kc', 'cold', 5, NULL)" | cut -f1,2)"
check "composite key: the mapping rows" 4 "$(sql 'SELECT COUNT(*) FROM sift_k_kc')"

# the integer fast path: no key table, the keys are the primary key
sql "CREATE TABLE ki (id BIGINT PRIMARY KEY, body TEXT)"
sql "INSERT INTO ki VALUES (7, 'quartz'), (9, 'quartz crystal')"
sql "CALL sift_attach('ki', 'ki', 'id', 't.body', 8)" >/dev/null
check "integer key: no key table" 0 "$(sql "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = '$DB' AND table_name = 'sift_k_ki'")"
check "integer key: the keys are the primary key" "7,9" "$(idx quartz ki)"

# detach removes the key table and the triggers
sql "CALL sift_detach('kc')" >/dev/null
check "detach: key table gone" 0 "$(sql "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = '$DB' AND table_name = 'sift_k_kc'")"
check "detach: triggers gone" 0 "$(sql "SELECT COUNT(*) FROM information_schema.triggers WHERE trigger_schema = '$DB' AND trigger_name LIKE 'sift_%_kc'")"

finish
