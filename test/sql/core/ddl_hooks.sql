-- sift.ddl_hooks lists the event triggers of the extension; sift.ddl_hooks_enable(false) turns every one of them off, the DDL each one guards then runs unguarded, and
-- enabling turns them on again and sweeps what the drops left behind.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SELECT name, event, tags, enabled, effect IS NOT NULL AS described FROM sift.ddl_hooks;

CREATE TABLE dh_t (id int, doc jsonb) USING sift_table;
SELECT sift.table_set_paths('dh_t', 'doc', 'doc.k int4');
SELECT sift.table_set_option('dh_t', 'key_order', 'id') IS NOT NULL AS option_set;
CREATE TABLE dh_s (id int, url text, note text);
INSERT INTO dh_s SELECT g, 'u' || g % 50, 'n' || g % 7 FROM generate_series(1, 2000) g;
CREATE INDEX dh_s_a ON dh_s USING sift_col (id, url);
CREATE INDEX dh_s_b ON dh_s USING sift_col (id, url, note);
CREATE SCHEMA dh_repack;
CREATE FUNCTION dh_repack.repack_trigger() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RETURN NULL; END $$;
DO $$ BEGIN EXECUTE format('CREATE TABLE dh_wide (%s) USING sift_table', (SELECT string_agg('c' || g || ' int', ', ') FROM generate_series(1, 127) g)); END $$;
-- the 128th column of a Format 1 sift_table is refused by sift_table_ddl_check; a Format 2.0 one takes it
CREATE FUNCTION dh_add_extra() RETURNS text LANGUAGE plpgsql AS $$
BEGIN ALTER TABLE dh_wide ADD COLUMN extra int; ALTER TABLE dh_wide DROP COLUMN extra; RETURN 'added';
EXCEPTION WHEN program_limit_exceeded THEN RETURN 'refused'; END $$;
CREATE FUNCTION dh_guarded() RETURNS bool LANGUAGE sql AS $$
SELECT dh_add_extra() = CASE (SELECT major FROM sift.index_format('dh_wide')) WHEN 1 THEN 'refused' ELSE 'added' END $$;

-- with the hooks on
CREATE TRIGGER dh_repack AFTER INSERT ON dh_t FOR EACH ROW EXECUTE FUNCTION dh_repack.repack_trigger();
SELECT dh_guarded() AS wide_guarded;

-- off
SELECT sift.ddl_hooks_enable(false) AS changed;
SELECT count(*) FILTER (WHERE enabled) AS enabled, count(*) AS hooks FROM sift.ddl_hooks;
CREATE TRIGGER dh_repack AFTER INSERT ON dh_t FOR EACH ROW EXECUTE FUNCTION dh_repack.repack_trigger();
DROP TRIGGER dh_repack ON dh_t;
SELECT dh_add_extra() AS wide_unguarded;
DROP INDEX dh_s_a, dh_s_b;
DROP TABLE dh_t;
SELECT count(*) AS paths_left FROM sift.table_paths WHERE relid::oid NOT IN (SELECT oid FROM pg_class);
SELECT count(*) AS options_left FROM sift.table_option_values WHERE rel::oid NOT IN (SELECT oid FROM pg_class);

-- on again: the leftovers go
SELECT sift.ddl_hooks_enable(true) AS changed;
SELECT count(*) FILTER (WHERE NOT enabled) AS disabled FROM sift.ddl_hooks;
SELECT count(*) AS paths_left FROM sift.table_paths WHERE relid::oid NOT IN (SELECT oid FROM pg_class);
SELECT count(*) AS options_left FROM sift.table_option_values WHERE rel::oid NOT IN (SELECT oid FROM pg_class);
SELECT dh_guarded() AS wide_guarded;

-- the function of the hooks in an event trigger of a role's own, at an event where the statement has not locked its table yet
CREATE TABLE dh_ev (a int);
CREATE EVENT TRIGGER dh_ev_start ON ddl_command_start EXECUTE FUNCTION sift.table_ddl_check();
ALTER TABLE dh_ev ADD COLUMN b int;
ALTER TABLE dh_ev RENAME COLUMN b TO c;
DROP EVENT TRIGGER dh_ev_start;
DROP TABLE dh_ev;

-- only a superuser
CREATE ROLE dh_user;
SET SESSION AUTHORIZATION dh_user;
SELECT sift.ddl_hooks_enable(false);
SELECT count(*) AS hooks_seen FROM sift.ddl_hooks;
RESET SESSION AUTHORIZATION;
DROP ROLE dh_user;

DROP TABLE dh_s, dh_wide;
DROP FUNCTION dh_guarded(), dh_add_extra();
DROP SCHEMA dh_repack CASCADE;
DROP EXTENSION pg_sift;
