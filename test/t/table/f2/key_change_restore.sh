#!/usr/bin/env bash
# tap: pooled
# A Format 2.0 sift_table with a primary key and a key_order, and one with a key_order alone, restored from pg_dump: a restore loads the rows before the primary key and the options of the
# table, so the parts it writes are in the key the table has before them. After the custom archive serially and in parallel, and a plain dump through psql, every predicate counts what
# the heap twin counts, through SiftAgg and the plain scan, and again after writes, a merge and VACUUM FULL of the restored tables.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 5
node_init nd 7 "autovacuum = off"
node_start nd || bail "start failed"
D=$(dir nd)
S=("-h" "$(node_sock nd)" "-p" "$(port nd)" "-U" "postgres")
export PGOPTIONS="-c client_min_messages=warning"
dbq() { local db=$1; shift; psql -X -At -q -v ON_ERROR_STOP=1 "${S[@]}" -d "$db" "${@/#/--command=}"; }
fresh() { dbq "$(node_db nd)" "DROP DATABASE IF EXISTS $1 WITH (FORCE)" "CREATE DATABASE $1" >/dev/null; }
errors() { grep -c 'ERROR:\|pg_restore: error' || true; }

ROWS="SELECT g, 40000 - g, timestamptz '2024-01-01' - g * interval '1 minute', g FROM generate_series"
SETUP="
DO \$\$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END \$\$;
CREATE TABLE kr_heap (id int8 PRIMARY KEY, k int8, ts timestamptz, v int8);
CREATE TABLE kr_pk (id int8 PRIMARY KEY, k int8, ts timestamptz, v int8) USING sift_table;
CREATE TABLE kr_ko (id int8 NOT NULL, k int8, ts timestamptz, v int8) USING sift_table;
SELECT sift.table_set_option('kr_pk', 'key_order', 'k');
SELECT sift.table_set_option('kr_ko', 'key_order', 'id');
INSERT INTO kr_heap $ROWS(1, 20000) g;
INSERT INTO kr_pk SELECT * FROM kr_heap;
INSERT INTO kr_ko SELECT * FROM kr_heap;
VACUUM kr_pk, kr_ko;"
# per table and scan path, the predicates whose count is not the heap's: empty when every one is
WRONG="SELECT string_agg(format('%s/%s: %s', t, a, q), '; ' ORDER BY t, a, q) FROM unnest('{kr_pk,kr_ko}'::text[]) t, unnest('{on,off}'::text[]) a,
	unnest(ARRAY['id BETWEEN 100 AND 199', 'k BETWEEN 30100 AND 30199', 'k IN (30500, 39999)', 'id > 29990', \$q\$ts BETWEEN '2023-12-31 20:00' AND '2023-12-31 21:00'\$q\$]) q
	WHERE kr_count(t, a, q) <> kr_count('kr_heap', 'on', q)"
COUNT="CREATE FUNCTION kr_count(t text, agg text, q text) RETURNS int8 LANGUAGE plpgsql AS \$\$
DECLARE n int8;
BEGIN
	PERFORM set_config('sift.enable_agg', agg, true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	EXECUTE format('SELECT count(*) FROM %I WHERE %s', t, q) INTO n;
	RETURN n;
END \$\$"
MORE="INSERT INTO kr_heap $ROWS(20001, 30000) g; INSERT INTO kr_pk SELECT * FROM kr_heap WHERE id > 20000; INSERT INTO kr_ko SELECT * FROM kr_heap WHERE id > 20000;
DELETE FROM kr_heap WHERE id % 5 = 0; DELETE FROM kr_pk WHERE id % 5 = 0; DELETE FROM kr_ko WHERE id % 5 = 0"

fresh src
dbq src "CREATE EXTENSION pg_sift" >/dev/null && psql -X -At -q -v ON_ERROR_STOP=1 "${S[@]}" -d src -f - <<<"$SETUP" >/dev/null && dbq src "$COUNT" >/dev/null || bail "setup failed"
check "the source answers as its heap twin" test -z "$(dbq src "$WRONG")"
pg_dump "${S[@]}" -Fc -f "$D/src.custom" src
pg_dump "${S[@]}" -Fp -f "$D/src.sql" src

restored() {
	local db=$1 out=$2
	[[ $(errors <<<"$out") == 0 ]] || { diag "$db: $(grep -m1 -A2 'ERROR\|error' <<<"$out" | tr '\n' ' ')"; return 1; }
	out=$(dbq "$db" "$WRONG")
	[[ -z $out ]] || { diag "$db after the restore: $out"; return 1; }
}
fresh r1
check "pg_restore, serial: every predicate counts what the heap twin counts" restored r1 "$(pg_restore "${S[@]}" -d r1 "$D/src.custom" 2>&1)"
fresh r2
check "pg_restore -j4: the same" restored r2 "$(pg_restore "${S[@]}" -j4 -d r2 "$D/src.custom" 2>&1)"
fresh r3
check "pg_dump | psql: the same" restored r3 "$(psql -X -q "${S[@]}" -d r3 -f "$D/src.sql" 2>&1)"

dbq r1 "$MORE" "SELECT sift.merge('kr_pk', true)" "SELECT sift.merge('kr_ko', true)" >/dev/null
first=$(dbq r1 "$WRONG")
dbq r1 "VACUUM FULL kr_pk" "VACUUM FULL kr_ko" >/dev/null
check "the restored tables take writes and deletes, and answer as the heap after a merge and VACUUM FULL" test -z "$first$(dbq r1 "$WRONG")"
