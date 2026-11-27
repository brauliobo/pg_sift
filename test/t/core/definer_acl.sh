#!/usr/bin/env bash
# What a role reaches through the definer functions of the extension: no statement of its own runs when a superuser makes the index of its projection.
# The statement of each check is the attack; the check is what the server must answer.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 4
node_init da 4 "autovacuum = off"
node_start da || bail "start failed"
D=$(dir da)
as() { PGOPTIONS="-c client_min_messages=notice" psql -X -At -q -h "$D/sock" -p "$(port da)" -U "$1" -d postgres 2>&1; }

as postgres <<'SQL' >/dev/null || bail "setup failed"
CREATE EXTENSION pg_sift;
CREATE ROLE atk LOGIN; CREATE ROLE own LOGIN; CREATE ROLE rd LOGIN;
GRANT CREATE, USAGE ON SCHEMA public TO atk, own, rd;
CREATE TABLE ran_as (who text);
GRANT INSERT ON ran_as TO PUBLIC;
SQL

# --- a projection holds one index element per column and one expression as its filter: nothing of it runs as the superuser that makes its index
as atk <<'SQL' >/dev/null || bail "projection setup failed"
CREATE TABLE pt (id int, kind int);
CREATE FUNCTION public.esc() RETURNS void LANGUAGE sql AS $$ INSERT INTO public.ran_as SELECT current_user $$;
SELECT sift.projection_create('pf', 'public.pt', ARRAY['id'], filter => 'kind = 1; SELECT public.esc()');
SELECT sift.projection_create('pl', 'public.pt', ARRAY['id) WHERE true; SELECT public.esc(); --']);
SELECT sift._projection_put(ROW('pp', 'public.pt', ARRAY['id) WHERE true; SELECT public.esc(); --'], NULL, NULL, '{}', ARRAY['id'], NULL)::sift.projections, false);
SELECT sift._projection_put(ROW('pq', 'public.pt', ARRAY['id'], NULL, 'true; SELECT public.esc()', '{}', ARRAY['id'], 'true')::sift.projections, false);
SQL
out=$(as postgres <<<"SELECT name FROM sift.projections ORDER BY 1")
check "a definition with more than one statement in a column or the filter is not stored" test -z "$out"
as postgres <<<"INSERT INTO sift.projections VALUES ('px', 'public.pt', ARRAY['id) WHERE true; SELECT public.esc(); --'], NULL, NULL, '{}', ARRAY['id'], NULL)" >/dev/null
out=$(as postgres <<<"SELECT sift.projection_index_sql('pxi', ARRAY['px']); SELECT sift.projection_index('pxi', ARRAY['px'])")
check "the index of a stored definition that is more than one statement is refused" test "$(grep -c '^ERROR' <<<"$out")" == 2
check "no statement of the role ran as the superuser" test "$(as postgres <<<"SELECT count(*) FROM ran_as WHERE who <> 'atk'")" == 0
as postgres <<<"DELETE FROM sift.projections WHERE name = 'px'" >/dev/null
as atk <<<"SELECT sift.projection_create('ok', 'public.pt', ARRAY['id', '(kind + 1)'], filter => 'kind > 0')" >/dev/null
check "a definition of columns, an expression and a filter is stored" test "$(as postgres <<<"SELECT sift.projection_index('oki', ARRAY['ok']); SELECT count(*) FROM pg_index WHERE indexrelid = 'oki'::regclass" | tail -n 1)" == 1

finish
