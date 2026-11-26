-- sift.upgrade_check blocks every negative sift.backend_cache_kb however it is written (5.1 took -1 for unlimited; 6.0 starts no server with it), and names the default of this release.
-- The settings of a database are rows of pg_db_role_setting that 5.1 accepted: they are written by hand here, each deleted again after the check.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
CREATE ROLE cache_kb_role;
CREATE TEMP TABLE found (value text, statuses text, advice text);

DO $$
DECLARE
	v text;
	db oid := (SELECT oid FROM pg_database WHERE datname = current_database());
	role oid := (SELECT oid FROM pg_roles WHERE rolname = 'cache_kb_role');
BEGIN
	FOREACH v IN ARRAY ARRAY['-1', '-1kB', '-1 kB', ' -2MB ', '0', '32768', '1GB'] LOOP
		INSERT INTO pg_db_role_setting (setdatabase, setrole, setconfig) VALUES (db, role, ARRAY['sift.backend_cache_kb=' || v]);
		INSERT INTO found SELECT v, coalesce(string_agg(status, ','), 'none'), min(advice) FROM sift.upgrade_check() WHERE object = 'sift.backend_cache_kb';
		DELETE FROM pg_db_role_setting WHERE setdatabase = db AND setrole = role;
	END LOOP;
END $$;

SELECT quote_literal(value) AS value, statuses FROM found ORDER BY ctid;
SELECT DISTINCT advice ~ '\(32768 is the default\)' AS names_default FROM found WHERE statuses = 'blocker';
SELECT boot_val FROM pg_settings WHERE name = 'sift.backend_cache_kb';
DROP ROLE cache_kb_role;
DROP EXTENSION pg_sift;
