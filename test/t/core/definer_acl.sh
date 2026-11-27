#!/usr/bin/env bash
# What a role reaches through the definer functions and the inspectors of the extension: no statement of its own runs when a superuser makes the index of its projection, no row of
# sift.scope_follows of another role is read or locked, a rename of its own table locks no index of another role, no inspector waits for the lock of a relation the role may not look at,
# and sift.status leaves no relation open for the temporary index of another session. The statement of each check is the attack; the check is what the server must answer.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 11
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

# --- the followed tables of a role are read and locked by their owner only
as own <<'SQL' >/dev/null || bail "scope setup failed"
CREATE TABLE src (member int, tenant int, ends date);
INSERT INTO src VALUES (1, 1, current_date + 1);
CREATE TABLE pairs (member int, tenant int, UNIQUE (member, tenant));
CREATE VIEW pairs_v AS SELECT member, tenant FROM src;
SELECT * FROM sift.scope_follow_pairs('pairs', 'pairs_v', 'src', 'member', 'member', 'ends');
GRANT SELECT ON pairs TO rd;
SQL
out=$(as rd <<<"BEGIN; SELECT count(*) FROM sift._follows_expiring('pairs'); SELECT count(*) FROM pg_locks WHERE pid = pg_backend_pid() AND relation = 'sift.scope_follows'::regclass AND mode = 'RowShareLock'; ROLLBACK")
check "a role that may read the pairs neither reads nor locks the rows of their followed tables" test "$(grep -c 'must be owner' <<<"$out")" == 1
check "the owner of the pairs still sweeps them" test "$(as own <<<"SELECT inserted >= 0 FROM sift.scope_expire_pairs('pairs')" | tail -n 1)" == t

# --- a rename of a table of a role locks no index of another role that names other tables
as own <<'SQL' >/dev/null || bail "connect setup failed"
CREATE TABLE dim (id int PRIMARY KEY);
CREATE INDEX dim_col ON dim USING sift_col (id);
CREATE TABLE ev (id int, dim_id int, v int);
CREATE INDEX ev_col ON ev USING sift_col (id, dim_id, v) WITH (connect = 'dim_id -> dim(id)');
SQL
out=$(as atk <<<"CREATE TABLE mine (id int); BEGIN; ALTER TABLE mine RENAME TO mine2; SELECT count(*) FROM pg_locks WHERE pid = pg_backend_pid() AND relation = 'ev_col'::regclass; ROLLBACK")
check "the rename of a table of a role leaves the indexes of the others unlocked" test "$(tail -n 1 <<<"$out")" == 0
out=$(as own <<<"ALTER TABLE dim RENAME TO dim2; SELECT sift.projection_option('ev_col'::regclass, 'connect')")
check "the rename of a table that an index names still follows into the index" test "$out" == 'dim_id -> dim2(id)'

# --- the inspectors refuse a relation the role may not look at before they wait for its lock; the tier policy is refused to a role that is not a superuser before the lock
as own <<'SQL' >/dev/null || bail "inspector setup failed"
CREATE TABLE docs (id int, body text);
INSERT INTO docs VALUES (1, 'a b c');
CREATE INDEX docs_bm ON docs USING sift (body);
CREATE TABLE tt (id int8, ts timestamp NOT NULL) USING sift_table;
INSERT INTO tt VALUES (1, '2020-01-01');
SQL
as postgres <<<"SELECT sift.set_tier_policy('tt', 'file://$D/tier', interval '1 day')" >/dev/null
sess_open da held
sess_say held "BEGIN; DROP INDEX docs_bm; LOCK TABLE tt IN ACCESS EXCLUSIVE MODE; \\echo locked"
wait_for "grep -q '^locked\$' $D/held.out" 60
LOCK_WAIT="SET lock_timeout = '3s'"  # timeout-ok: the product side must refuse at once; the lock is held until the test releases it
out=$(as atk <<SQL
$LOCK_WAIT;
SELECT count(*) FROM sift.tokenize('a b', 'docs_bm');
SELECT count(*) FROM sift.segments('docs_bm');
SELECT count(*) FROM sift.maintain('docs_bm');
SELECT sift.promote('tt');
SELECT count(*) FROM sift.check('tt');
SELECT count(*) FROM sift.tier_status;
SQL
)
check "the inspectors refuse a relation of another role without waiting for its lock" test "$(grep -c 'permission denied' <<<"$out"),$(grep -c 'lock timeout' <<<"$out")" == 5,0
out=$(as own <<<"$LOCK_WAIT; SELECT sift.set_tier_policy('tt', 'file:///nonexistent', interval '1 day'); SELECT sift.clear_tier_policy('tt')")
check "a tier policy is refused to the owner of a table before the lock of the table" test "$(grep -c 'must be superuser' <<<"$out")" == 2
sess_say held "ROLLBACK"; sess_close held

# --- sift.status skips the temporary index of another session and closes it
sess_open da tmp
sess_say tmp "CREATE TEMP TABLE tv (id int); CREATE INDEX tv_col ON tv USING sift_col (id); \\echo made"
wait_for "grep -q '^made\$' $D/tmp.out" 60
out=$(as postgres <<<"SELECT count(*) >= 0 FROM sift.status()")
sess_close tmp
check "sift.status leaves no relation of another session's temporary index open" test "$(grep -c 'not closed\|leak' <<<"$out"),$(tail -n 1 <<<"$out")" == 0,t

finish
