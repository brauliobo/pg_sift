#!/usr/bin/env bash
# The older names of the reloptions renamed in 4.2 (rollup is rollup_leaves, rollup_dims is rollup_columns) are kept in the catalog of an index that carries them, and pg_dump writes them
# as they are: a restore (custom archive, plain script) must bring the index back with the same options and the same answers, ALTER INDEX SET and RESET of the old name must keep
# working on the restored index, and REINDEX must not lose the rollups.
. "$(dirname "$0")/lib.bash"

plan 9
node_init a 37 "autovacuum = off"
node_start a || bail "start failed"
D=$(dir a)
S=("-h" "$D/sock" "-p" "$(port a)" "-U" "postgres")
export PGOPTIONS="-c client_min_messages=warning"
dbq() { local db=$1; shift; psql -X -At -q -v ON_ERROR_STOP=1 "${S[@]}" -d "$db" "${@/#/--command=}"; }
fresh() { dbq postgres "DROP DATABASE IF EXISTS $1" "CREATE DATABASE $1" >/dev/null; }
errors() { grep -c 'ERROR:\|pg_restore: error' || true; }
OPTS="SELECT string_agg(c.relname || ':' || array_to_string(c.reloptions, ','), ' ' ORDER BY c.relname) FROM pg_class c WHERE c.relname IN ('al_old', 'al_new')"
# the answer of a grouped read by the element of a list and by two columns, from the index and from the heap
ANS="SET max_parallel_workers_per_gather = 0; SET seq_page_cost = 10000; SELECT (SELECT string_agg(x.k || x.f || ':' || x.c, ',' ORDER BY x.k, x.f) FROM (SELECT i->>'kind' k, i->>'flag' f, count(*) c FROM al, jsonb_array_elements(items) i GROUP BY 1, 2) x) || '/' || (SELECT string_agg(g || ':' || h || ':' || c || ':' || s, ',' ORDER BY g, h) FROM (SELECT g, h, count(*) c, sum(v) s FROM al GROUP BY g, h) y)"

fresh src
dbq src "CREATE EXTENSION pg_sift" \
	"CREATE TABLE al (id int, g int, h int, v int, items jsonb) WITH (autovacuum_enabled = false)" \
	"INSERT INTO al SELECT i, i % 5, i % 7, i, jsonb_build_array(jsonb_build_object('kind', 'k' || i % 3, 'flag', i % 2 = 0)) FROM generate_series(1, 4000) i" \
	"CREATE INDEX al_old ON al USING sift_col (id, g, h, v, items) WITH (paths = 'items[]{kind text, flag bool}', rollup = 'items[].kind, items[].flag', rollup_dims = 'g, h: v')" >/dev/null || bail "setup failed"
dbq src "CREATE INDEX al_new ON al USING sift_col (id, g, h, v, items) WITH (paths = 'items[]{kind text, flag bool}', rollup_leaves = 'items[].kind, items[].flag', rollup_columns = 'g, h: v')" >/dev/null
check "the index made with the old names keeps them in its options, the other has the new" test "$(dbq src "$OPTS" | grep -c 'al_old:.*rollup=.*rollup_dims=')" == 1 -a "$(dbq src "$OPTS" | grep -c 'al_new:.*rollup_leaves=.*rollup_columns=')" == 1
WANT=$(dbq src "SET sift.enable_agg = off" "$ANS" | tail -n 1)
check "the old and the new names answer as the heap" test "$(dbq src "SET sift.enable_agg = on" "$ANS" | tail -n 1)" == "$WANT"
before=$(dbq src "$OPTS")
pg_dump "${S[@]}" -Fc -f "$D/src.custom" src
pg_dump "${S[@]}" -Fp -f "$D/src.sql" src
check "pg_dump writes each index with the name it carries" test "$(grep -c -E 'rollup=.*rollup_dims=' "$D/src.sql")" == 1 -a "$(grep -c -E 'rollup_leaves=.*rollup_columns=' "$D/src.sql")" == 1

fresh r1
check "pg_restore: no error, the same options and the same answers" test "$(pg_restore "${S[@]}" -d r1 "$D/src.custom" 2>&1 | errors)" == 0 -a "$(dbq r1 "$OPTS")" == "$before" -a "$(dbq r1 "$ANS" | tail -n 1)" == "$WANT"
fresh r2
check "pg_dump | psql: no error, the same options and the same answers" test "$(psql -X -q "${S[@]}" -d r2 -f "$D/src.sql" 2>&1 | errors)" == 0 -a "$(dbq r2 "$OPTS")" == "$before" -a "$(dbq r2 "$ANS" | tail -n 1)" == "$WANT"

# ALTER INDEX SET and RESET of the old names on the restored index (each a session of its own: the NOTICE comes with the library loaded)
out=$(PGOPTIONS="-c client_min_messages=notice" psql -X -q "${S[@]}" -d r1 -c "ALTER INDEX al_old SET (rollup = 'items[].kind')" -c "ALTER INDEX al_old SET (rollup_dims = 'g, h')" 2>&1)
check "ALTER INDEX SET of the old names works and names the new spelling in a NOTICE" test "$(grep -c 'is the older name of' <<<"$out")" == 4 -a "$(grep -c ERROR <<<"$out")" == 0
check "after the SET the old options carry the new values and answer as the heap" test "$(dbq r1 "SELECT array_to_string(reloptions, ',') FROM pg_class WHERE relname = 'al_old'" | grep -c 'rollup=items\[\].kind.*rollup_dims=g, h')" == 1 -a "$(dbq r1 "$ANS" | tail -n 1)" == "$WANT"
dbq r1 "ALTER INDEX al_old RESET (rollup, rollup_dims)" >/dev/null
check "RESET of the old names removes them, REINDEX rebuilds, and the answers stay the heap's" test "$(dbq r1 "SELECT count(*) FROM pg_class WHERE relname = 'al_old' AND array_to_string(reloptions, ',') ~ 'rollup'")" == 0 -a "$(dbq r1 "REINDEX INDEX al_old" "$ANS" | tail -n 1)" == "$WANT"
check "the new names are the ones a rebuild writes: an automatic choice lands in rollup_columns" test "$(dbq r1 "CREATE INDEX al_auto ON al USING sift_col (id, g, h, v) WITH (rollup_columns = 'auto')" "SELECT array_to_string(reloptions, ',') ~ 'rollup_columns=auto: ' OR array_to_string(reloptions, ',') ~ 'rollup_columns=auto' FROM pg_class WHERE relname = 'al_auto'" | tail -n 1)" == t
finish
