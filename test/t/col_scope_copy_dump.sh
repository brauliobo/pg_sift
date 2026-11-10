#!/usr/bin/env bash
# A database with scope copies (a plain one and a partitioned one) restored from pg_dump: the custom archive serially and in parallel and a plain dump through psql. The metadata of
# the copies (sift.scope_copies) is dumped by name: every restore has no error, the copies are exact (sift.scope_verify finds nothing, every key answers as the join does), the
# triggers of both tables are there, later changes keep the copies exact, and sift.scope_drop removes a restored copy with its partitions and its triggers.
. "$(dirname "$0")/lib.bash"
. "$(dirname "$0")/lib_scope.bash"

plan 13
node_init r 51 "autovacuum = off"
node_start r || bail "start failed"
D=$(dir r)
S=("-h" "$D/sock" "-p" "$(port r)" "-U" "postgres")
export PGOPTIONS="-c client_min_messages=warning"
dbq() { local db=$1; shift; psql -X -At -q -v ON_ERROR_STOP=1 "${S[@]}" -d "$db" "${@/#/--command=}"; }
fresh() { dbq postgres "DROP DATABASE IF EXISTS $1 WITH (FORCE)" "CREATE DATABASE $1" >/dev/null; }
errors() { grep -c 'ERROR:\|pg_restore: error' || true; }
# the triggers of the copies on the facts, on the pairs, and the copies in the metadata
shape() {
	dbq "$1" "SELECT (SELECT count(*) FROM pg_trigger WHERE NOT tgisinternal AND tgname LIKE 'sift_scope_%' AND tgrelid = 'facts'::regclass) || ',' ||
		(SELECT count(*) FROM pg_trigger WHERE NOT tgisinternal AND tgname LIKE 'sift_scope_%' AND tgrelid = 'pairs'::regclass) || ',' || (SELECT count(*) FROM sift.scope_copies)"
}
exact() { local db=$1 c; shift; for c in ${@:-cflat cpart}; do [[ $(dbq "$db" "SELECT scope_same('$c')" | tail -n 1) == t ]] || return 1; done; }
WANT="8,8,2"
restored() { [[ $(shape "$1") == "$WANT" ]] && exact "$1"; }
# a change of the facts and of the pairs (ids of facts from $2)
after() {
	dbq "$1" "INSERT INTO facts VALUES ($2, 45, now(), 1, 5, 'r'), ($2 + 1, 3, now(), 2, 6, 'r')" "INSERT INTO pairs VALUES (44, 8) ON CONFLICT DO NOTHING" \
		"UPDATE facts SET amount = amount + 1 WHERE id % 11 = 0" "DELETE FROM facts WHERE id % 13 = 0" "DELETE FROM pairs WHERE scope_id = 3 AND dim_a_id % 2 = 0" >/dev/null
}

# the source is the database postgres of the node
scope_setup r || bail "setup failed"
sql r "UPDATE facts SET amount = amount + 1 WHERE id % 7 = 0" "DELETE FROM pairs WHERE scope_id = 2 AND dim_a_id % 2 = 1" "INSERT INTO pairs VALUES (45, 8)" >/dev/null
check "the source: two copies, 8 triggers on each table, exact" restored postgres
pg_dump "${S[@]}" -Fc -f "$D/src.custom" postgres
pg_dump "${S[@]}" -Fp -f "$D/src.sql" postgres
check "the dump holds the metadata of the copies by name" test "$(grep -c "public.cflat\|public.cpart" "$D/src.sql")" -ge 4

fresh r1
check "pg_restore, serial: no error" test "$(pg_restore "${S[@]}" -d r1 "$D/src.custom" 2>&1 | errors)" == 0
check "... the copies, the triggers and the answers are those of the source" restored r1
after r1 800001
check "... and after changes of the facts and of the pairs the copies are exact" exact r1

fresh rp
check "pg_restore -j8: no error" test "$(pg_restore "${S[@]}" -j8 -d rp "$D/src.custom" 2>&1 | errors)" == 0
check "... the copies, the triggers and the answers are those of the source" restored rp

fresh r2
check "pg_dump | psql: no error" test "$(psql -X -q "${S[@]}" -d r2 -f "$D/src.sql" 2>&1 | errors)" == 0
check "... the copies, the triggers and the answers are those of the source" restored r2
after r2 810001
check "... and after changes the copies are exact" exact r2

drop_restored() {
	dbq r1 "SELECT sift.scope_drop('cpart')" >/dev/null && [[ $(shape r1) == "4,4,1" && $(dbq r1 "SELECT count(*) FROM pg_class WHERE relname LIKE 'cpart%'") == 0 ]]
}
check "sift.scope_drop of a restored copy removes the copy, its partitions and its triggers" drop_restored
after r1 820001
check "... the other copy stays exact" exact r1 cflat
dbq r1 "SELECT sift.scope_drop('cflat')" >/dev/null
check "... and the last one goes too: no copy, no trigger of a copy left" test "$(shape r1)" == "0,0,0"
finish
