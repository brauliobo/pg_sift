#!/usr/bin/env bash
# What an unprivileged role must not do through the functions of the extension: run its own code as a superuser (the scope pair functions and their trigger, the
# projection helpers), take the lock of a table of another role through a trigger function, read what the views and helpers of other roles' tables name, drop rows
# with a maintain right alone, or wait on a lock of a table it may not touch. The statement of each check is the attack; the check is what the server must answer.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 19
node_init pe 4 "autovacuum = off"
node_start pe || bail "start failed"
D=$(dir pe)
# as ROLE: the statements of stdin, errors and notices on stdout, the session goes on after an error
as() { PGOPTIONS="-c client_min_messages=notice" psql -X -At -q -h "$D/sock" -p "$(port pe)" -U "$1" -d postgres 2>&1; }

as postgres <<'SQL' >/dev/null || bail "setup failed"
CREATE EXTENSION pg_sift;
CREATE ROLE atk LOGIN; CREATE ROLE own LOGIN; CREATE ROLE mt LOGIN; CREATE ROLE wr LOGIN;
GRANT CREATE, USAGE ON SCHEMA public TO atk, own, mt, wr;
SQL
as own <<'SQL' >/dev/null || bail "victim setup failed"
CREATE TABLE vt (id int, ts timestamptz NOT NULL, doc jsonb) USING sift_table;
INSERT INTO vt SELECT g, now() - interval '400 days', '{}' FROM generate_series(1, 300) g;
CREATE TABLE vk (id int, g int);
INSERT INTO vk SELECT g, g % 3 FROM generate_series(1, 300) g;
CREATE INDEX vk_col ON vk USING sift_col (id, g);
SELECT sift.projection_create('secret_p', 'vk', ARRAY['id', 'g']);
GRANT MAINTAIN ON vt TO mt;
SQL

# --- the scope pair functions run the views, triggers and checks of a role as that role, and the trigger of a followed table as the owner of its pairs
as atk <<'SQL' >/dev/null || bail "scope setup failed"
CREATE TABLE evil_log (who text);
CREATE FUNCTION public.evil() RETURNS int LANGUAGE plpgsql AS $$ BEGIN INSERT INTO public.evil_log VALUES (current_user); RETURN 1; END $$;
CREATE TABLE src (member int, tenant int, ends date);
INSERT INTO src VALUES (1, 1, current_date), (2, 2, current_date);
CREATE TABLE pairs (member int, tenant int, UNIQUE (member, tenant));
CREATE VIEW pairs_v AS SELECT member, tenant FROM src WHERE public.evil() = 1;
GRANT INSERT ON src TO wr;
SELECT * FROM sift.scope_follow_pairs('pairs', 'pairs_v', 'src', 'member', 'member', 'ends');
SQL
who() { as postgres <<<"SELECT coalesce(string_agg(DISTINCT who, ','), '') FROM evil_log"; }
check "scope_follow_pairs runs the view of the caller as the caller" test "$(who)" == atk
as postgres <<<"TRUNCATE evil_log" >/dev/null
as wr <<<"INSERT INTO src VALUES (3, 3, current_date)" >/dev/null
check "the trigger of a followed table runs the view as the owner of the pairs, whoever writes the table" test "$(who)" == atk
as postgres <<<"TRUNCATE evil_log" >/dev/null
as atk <<<"SELECT * FROM sift.scope_expire_pairs('pairs', '1 year')" >/dev/null
check "scope_expire_pairs runs the view of the caller as the caller" test "$(who)" == atk
check "the pairs follow the view after the writes of the others" test "$(as postgres <<<"SELECT count(*) FROM pairs")" == 3

# --- the projection helpers read the filter of a role as that role; a relation that is not an index is an error
as atk <<'SQL' >/dev/null || bail "projection setup failed"
CREATE TABLE pt (id int, g int);
INSERT INTO pt SELECT g, g % 3 FROM generate_series(1, 100) g;
CREATE INDEX pt_col ON pt USING sift_col (id, g);
CREATE FUNCTION public.evil_i(int) RETURNS int IMMUTABLE LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'evil_ran_as=%', current_user; RETURN $1; END $$;
SELECT sift.projection_create('pe', 'pt', ARRAY['id', 'g'], NULL, 'id > public.evil_i(1)');
SQL
out=$(as atk <<<"SELECT sift.projection_holds('pt_col'::regclass::oid, 'pe')")
check "projection_holds folds the filter of a role as that role" test "$(grep -c 'evil_ran_as=postgres' <<<"$out")" == 0

# --- a trigger function of the catalogs attached to a table of a role does not lock the table of another
out=$(as atk <<'SQL'
CREATE TABLE fake (relid oid);
BEGIN;
DO $$ BEGIN
	BEGIN
		CREATE TRIGGER fake_trg BEFORE INSERT ON fake FOR EACH ROW EXECUTE FUNCTION sift.table_paths_trg();
		INSERT INTO fake VALUES ('vt'::regclass::oid);
	EXCEPTION WHEN OTHERS THEN RAISE NOTICE 'refused: %', SQLERRM; END;
END $$;
SELECT count(*) FROM pg_locks WHERE relation = 'vt'::regclass AND mode = 'AccessExclusiveLock' AND pid = pg_backend_pid();
ROLLBACK;
SQL
)
check "a trigger function attached to a table of a role does not take the lock of another table" test "$(grep -x "[01]" <<<"$out")" == 0
check "the trigger functions of the catalogs are not executable by every role" test "$(as postgres <<<"SELECT count(*) FROM pg_proc WHERE oid IN ('sift.table_paths_trg()'::regprocedure, 'sift.table_option_values_trg()'::regprocedure) AND has_function_privilege('public', oid, 'EXECUTE')")" == 0

# --- the views and helpers name only what the role may look at
out=$(as atk <<'SQL'
CREATE FUNCTION public.leak(text) RETURNS bool COST 0.0001 LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'leak=%', $1; RETURN true; END $$;
SELECT count(*) FROM sift.projection_status WHERE public.leak(projection);
SQL
)
check "sift.projection_status does not run the function of a role on the projections of the others" test "$(grep -c 'leak=secret_p' <<<"$out")" == 0
out=$(as atk <<<"SELECT sift.projection_holds('vk_col'::regclass::oid, 'secret_p'); SELECT sift.projection_serves('vk_col'::regclass::oid, ARRAY['secret_p'], '{}'); SELECT * FROM sift.projection_plan('vk_col', ARRAY['secret_p'], '{}', false); SELECT sift._pair_columns('pairs'::regclass, 'pairs_v'::regclass); SELECT sift._pair_columns('vk'::regclass, 'vk'::regclass)")
check "the projection helpers and _pair_columns refuse a table the role may not look at" test "$(grep -c 'permission denied' <<<"$out")" -ge 3
check "the internal helpers that no function of a role calls are closed to every role" test "$(as postgres <<<"SELECT count(*) FROM pg_proc p WHERE p.pronamespace = 'sift_internal'::regnamespace AND p.proname ~ '^(argext_|argmax_|argmin_|projection_defs_|table_paths_skip|ttl_policies_skip|tier_policies_forget)' AND has_function_privilege('atk', p.oid, 'EXECUTE')")" == 0
check "the pacer of the maintenance says nothing to a role that may not read the statistics of the server" test "$(as atk <<<"SELECT count(*) FROM sift_internal.maint_pacer() p WHERE p.workers_max IS NOT NULL")" == 0
check "the views of the extension still answer a role with no right on the internal schema" test "$(as atk <<<"SELECT count(*) >= 0 FROM sift.status(); SELECT count(*) >= 0 FROM sift.indexes; SELECT count(*) >= 0 FROM sift.activity()" | grep -c '^t$')" == 3

# --- a maintain right is not the right to delete; a role that may not touch a table never waits for its lock
out=$(as mt <<<"SELECT sift.drop_parts_before('vt', now())")
check "a role with MAINTAIN alone cannot drop the parts of a table" test "$(grep -c 'permission denied' <<<"$out")" == 1
out=$(as mt <<<"SELECT sift.set_ttl('vt', 'ts', interval '1 day')")
check "a role with MAINTAIN alone cannot set a TTL, which drops parts" test "$(grep -c 'permission denied' <<<"$out")" == 1
sess_open pe held
sess_say held "BEGIN; LOCK TABLE vt IN ACCESS EXCLUSIVE MODE; \\echo locked"
wait_for "grep -q '^locked\$' $D/held.out" 60
LOCK_WAIT="SET lock_timeout = '3s'"  # timeout-ok: the product side must refuse at once; the lock is held until the test releases it
out=$(as atk <<SQL
$LOCK_WAIT;
SELECT sift.drop_parts_before('vt', now());
SELECT sift.upgrade_table('vt');
SELECT sift.set_ttl('vt', 'ts', interval '1 day');
SELECT sift.create_projection('vt', 'p', 'ORDER BY id');
SELECT sift.table_paths_check('vt', 'doc', 'doc.k int4');
SQL
)
sess_say held "COMMIT"; sess_close held
check "a role with no right on a table is refused by every function that locks it, without waiting for the lock" test "$(grep -c 'permission denied\|must be owner' <<<"$out")" == 5

# --- the sketches of a partial index do not answer for the table; a TTL is not set inside a transaction block
as own <<'SQL' >/dev/null
CREATE TABLE kt (id int, v int);
INSERT INTO kt SELECT g, g FROM generate_series(1, 1000) g;
CREATE INDEX kt_part ON kt USING sift_col (id, v) WHERE id <= 10;
SQL
out=$(as own <<<"SELECT sift.approx_percentile('kt', 'v', 0.5)")
check "approx_percentile does not read the sketches of a partial index" test "$([[ $out =~ ^[0-9]+$ ]] && ((out < 20)) && echo wrong || echo right)" == right
out=$(as own <<<"BEGIN; SELECT sift.set_ttl('vt', 'ts', interval '1 day'); ROLLBACK")
check "set_ttl is refused inside a transaction block" test "$(grep -c 'cannot run inside a transaction block' <<<"$out")" == 1

# --- the registries of the server are for the monitoring roles
out=$(as atk <<<"SELECT count(*) FROM sift.shared_memory(); SELECT count(*) FROM sift.shared_memory_pools()")
check "the shared memory of the server is not listed to an ordinary role" test "$(grep -c 'permission denied' <<<"$out")" == 2

# --- last: it stops the server when the helper reads the fields of a table as those of an index
out=$(as atk <<<"SELECT sift.projection_filter_served('pg_class'::regclass, 'oid > 0', 'pg_class'::regclass)")
check "a table given as the index is an error of the statement" test "$(grep -c 'server closed the connection\|connection to server was lost' <<<"$out")" == 0

finish
