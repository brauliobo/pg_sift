#!/usr/bin/env bash
# tap: pooled
# A database whose tables, columns and indexes were renamed after the sift_col indexes were built (lib/names.bash), restored from pg_dump: the custom archive serially and in parallel
# (four runs) and a plain dump through psql. The options that name a table or a column (connect, scope with its btree and its columns, paths, key_cut, rollup_columns, the definitions of a
# projection, the columns of a follow) are the source's after every restore, the indexes verify, and the answers with the pushdown are the plain plans' and the source's. The restored
# database takes writes, and a dump of what the source wrote after the renames goes through the same restore.
. "${SIFT_REPO:?}/test/t/lib/common.bash"
. "${SIFT_REPO:?}/test/t/lib/names.bash"

plan 13
node_init nd 56 "autovacuum = off"
node_start nd || bail "start failed"
D=$(dir nd)
S=("-h" "$(node_sock nd)" "-p" "$(port nd)" "-U" "postgres")
export PGOPTIONS="-c client_min_messages=warning"
dbq() { local db=$1; shift; psql -X -At -q -v ON_ERROR_STOP=1 "${S[@]}" -d "$db" "${@/#/--command=}"; }
fresh() { dbq "$(node_db nd)" "DROP DATABASE IF EXISTS $1 WITH (FORCE)" "CREATE DATABASE $1" >/dev/null; }
errors() { grep -c 'ERROR:\|pg_restore: error' || true; }
verified() { dbq "$1" "$NAMES_VERIFY" >/dev/null 2>&1; }
# the answers agree with the plain plans, and are the source's
answers_are() { [[ $(dbq "$1" "$NAMES_ANSWERS") == "t|$2" ]]; }

fresh src
dbq src "CREATE EXTENSION pg_sift" >/dev/null && psql -X -At -q -v ON_ERROR_STOP=1 "${S[@]}" -d src -f - <<<"$NAMES_SETUP" >/dev/null || bail "setup failed"
STATE=$(dbq src "$NAMES_STATE")
ANSWER=$(dbq src "SELECT nm_answers_off()")
check "the source: the indexes verify, the answers with the pushdown are the plain plans', and the options name what the tables are called now" \
	test "$(verified src && echo ok)" == ok -a "$(dbq src "$NAMES_ANSWERS")" == "t|$ANSWER" -a "$(grep -c 'nm_fact_c connect=dim_ref -> nm_s.nm_dims(dim_pk)' <<<"$STATE")" == 1 \
	-a "$(grep -c 'nm_ev_c scope=g: who_ref -> nm_s.nm_grants(who, grp_id) using nm_grants_btree cols(category, v)' <<<"$STATE")" == 1 -a "$(grep -c 'paths=document.k int4, document.s text' <<<"$STATE")" == 1 \
	-a "$(grep -c 'nm_tbl document document->k int4, document->s text' <<<"$STATE")" == 1 -a "$(grep -c 'nm_tbl compression=auto; grp=zstd:3,nm_tbl key_order=grp, id' <<<"$STATE")" == 1
diag "$STATE"
pg_dump "${S[@]}" -Fc -f "$D/src.custom" src
pg_dump "${S[@]}" -Fp -f "$D/src.sql" src
check "the dump writes the options with the names the tables have" test "$(grep -c "dim_ref -> nm_s.nm_dims(dim_pk)" "$D/src.sql")" == 1 -a "$(grep -c "paths='document.k int4, document.s text'" "$D/src.sql")" == 1

fresh r1
out=$(pg_restore "${S[@]}" -d r1 "$D/src.custom" 2>&1)
check "pg_restore, serial: no error, the options, the projection and the follow of the source" test "$(errors <<<"$out")" == 0 -a "$(dbq r1 "$NAMES_STATE")" == "$STATE"
check "... the indexes verify and the answers are the source's" test "$(verified r1 && echo ok)" == ok -a "$(answers_are r1 "$ANSWER" && echo ok)" == ok
ok_runs=0
for run in 1 2 3 4; do
	fresh rp
	out=$(pg_restore "${S[@]}" -j8 -d rp "$D/src.custom" 2>&1)
	[[ $(errors <<<"$out") == 0 && $(dbq rp "$NAMES_STATE") == "$STATE" ]] && verified rp && answers_are rp "$ANSWER" && ok_runs=$((ok_runs + 1)) || diag "run $run: $(grep -m1 -A3 'ERROR' <<<"$out" | tr '\n' ' ')"
done
check "pg_restore -j8, four runs: no error, the same options, verified indexes and the source's answers" test "$ok_runs" == 4
fresh r2
check "pg_dump | psql: no error, the same options, verified indexes and the source's answers" test "$(psql -X -q "${S[@]}" -d r2 -f "$D/src.sql" 2>&1 | errors)" == 0 -a "$(dbq r2 "$NAMES_STATE")" == "$STATE" \
	-a "$(verified r2 && echo ok)" == ok -a "$(answers_are r2 "$ANSWER" && echo ok)" == ok

# the restored database is a database like the source: it takes writes under the new names and its follow keeps the pairs
dbq r1 "$NAMES_WRITES" >/dev/null
dbq src "$NAMES_WRITES" >/dev/null
check "the restored database takes writes: its indexes verify, its answers are the plain plans' and the source's after the same writes" test "$(verified r1 && echo ok)" == ok \
	-a "$(dbq r1 "$NAMES_ANSWERS")" == "$(dbq src "$NAMES_ANSWERS")" \
	-a "$(dbq r1 "SELECT (SELECT count(*) FROM nm_fpairs) = (SELECT count(*) FROM nm_view), (SELECT count(*) FROM nm_fpairs WHERE grp = 5)")" == "t|2"
dbq r1 "REINDEX INDEX nm_fact_c" "REINDEX INDEX nm_ev_c" "REINDEX INDEX nm_s.nm_dim_c" "SELECT sift.col_merge('nm_fact_c', true)" >/dev/null
check "REINDEX and a merge of the restored indexes keep the names, the verdicts and the answers" test "$(dbq r1 "$NAMES_STATE")" == "$STATE" -a "$(verified r1 && echo ok)" == ok \
	-a "$(dbq r1 "$NAMES_ANSWERS")" == "$(dbq src "$NAMES_ANSWERS")"

# a column renamed again in the restored database, then dumped and restored once more: the names go on following
dbq r1 "ALTER TABLE nm_fact RENAME COLUMN labels TO tag_list" "ALTER TABLE nm_s.nm_dims RENAME TO nm_parents" >/dev/null
check "a column and a parent table renamed in the restored database: the child index names them and verifies" test "$(dbq r1 "SELECT o FROM pg_class c, unnest(c.reloptions) o WHERE c.relname = 'nm_fact_c' AND o LIKE 'connect=%'")" == "connect=dim_ref -> nm_s.nm_parents(dim_pk)" \
	-a "$(verified r1 && echo ok)" == ok -a "$(dbq r1 "SELECT count(*) FROM nm_fact WHERE tag_list = ARRAY[1]")" == "$(dbq src "SELECT count(*) FROM nm_fact WHERE labels = ARRAY[1]")"
pg_dump "${S[@]}" -Fc -f "$D/r1.custom" r1
fresh r3
out=$(pg_restore "${S[@]}" -j8 -d r3 "$D/r1.custom" 2>&1)
check "the dump of the restored database restores: no error, verified indexes, the same options" test "$(errors <<<"$out")" == 0 -a "$(verified r3 && echo ok)" == ok \
	-a "$(dbq r3 "SELECT o FROM pg_class c, unnest(c.reloptions) o WHERE c.relname = 'nm_fact_c' AND o LIKE 'connect=%'")" == "connect=dim_ref -> nm_s.nm_parents(dim_pk)"

# what the source wrote after the renames goes through the same restore
pg_dump "${S[@]}" -Fc -f "$D/after.custom" src
STATE2=$(dbq src "$NAMES_STATE")
fresh r4
out=$(pg_restore "${S[@]}" -j8 -d r4 "$D/after.custom" 2>&1)
check "a dump of the source after more writes restores: no error, the same options, verified indexes and the same answers" test "$(errors <<<"$out")" == 0 -a "$(dbq r4 "$NAMES_STATE")" == "$STATE2" \
	-a "$(verified r4 && echo ok)" == ok -a "$(dbq r4 "$NAMES_ANSWERS")" == "$(dbq src "$NAMES_ANSWERS")"
check "the source itself, after the writes, merges and verifies" test "$(dbq src "SELECT sift.col_promote('nm_fact_c') >= 0, sift.col_merge('nm_fact_c', true) >= 0, sift.col_merge('nm_ev_c', true) >= 0")" == "t|t|t" \
	-a "$(verified src && echo ok)" == ok -a "$(dbq src "SELECT nm_answers_on() = nm_answers_off()")" == t
check "the restored indexes have no stale layout: a first write needs no REINDEX" test "$(dbq r4 "INSERT INTO nm_fact SELECT 9000 + g, 1 + g % 300, date '2025-06-01', ARRAY[2], '{\"k\": 3}', g FROM generate_series(1, 50) g" "SELECT sift.col_verify('nm_fact_c', true)" 2>&1 | grep -c ERROR)" == 0
finish
