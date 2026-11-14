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

-- with the hooks on
CREATE TRIGGER dh_repack AFTER INSERT ON dh_t FOR EACH ROW EXECUTE FUNCTION dh_repack.repack_trigger();
ALTER TABLE dh_wide ADD COLUMN extra int;

-- off
SELECT sift.ddl_hooks_enable(false) AS changed;
SELECT count(*) FILTER (WHERE enabled) AS enabled, count(*) AS hooks FROM sift.ddl_hooks;
CREATE TRIGGER dh_repack AFTER INSERT ON dh_t FOR EACH ROW EXECUTE FUNCTION dh_repack.repack_trigger();
DROP TRIGGER dh_repack ON dh_t;
ALTER TABLE dh_wide ADD COLUMN extra int;
ALTER TABLE dh_wide DROP COLUMN extra;
DROP INDEX dh_s_a, dh_s_b;
DROP TABLE dh_t;
SELECT count(*) AS paths_left FROM sift.table_paths WHERE relid::oid NOT IN (SELECT oid FROM pg_class);
SELECT count(*) AS options_left FROM sift.table_option_values WHERE rel::oid NOT IN (SELECT oid FROM pg_class);

-- on again: the leftovers go
SELECT sift.ddl_hooks_enable(true) AS changed;
SELECT count(*) FILTER (WHERE NOT enabled) AS disabled FROM sift.ddl_hooks;
SELECT count(*) AS paths_left FROM sift.table_paths WHERE relid::oid NOT IN (SELECT oid FROM pg_class);
SELECT count(*) AS options_left FROM sift.table_option_values WHERE rel::oid NOT IN (SELECT oid FROM pg_class);
ALTER TABLE dh_wide ADD COLUMN extra int;

-- only a superuser
CREATE ROLE dh_user;
SET SESSION AUTHORIZATION dh_user;
SELECT sift.ddl_hooks_enable(false);
SELECT count(*) AS hooks_seen FROM sift.ddl_hooks;
RESET SESSION AUTHORIZATION;
DROP ROLE dh_user;

DROP TABLE dh_s, dh_wide;
DROP SCHEMA dh_repack CASCADE;
DROP EXTENSION pg_sift;
