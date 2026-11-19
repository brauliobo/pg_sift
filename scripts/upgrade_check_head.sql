-- The preflight of an update that is not made yet: loads sift.upgrade_check() (the part sql/5.1/27-upgrade-check.sql, built into this file by scripts/sql-parts.sh) in a
-- transaction that is rolled back and prints one row per finding, so that a database whose extension is older than the library can be checked.
-- Run it as a superuser, on the primary, with the files of the new release installed and in a session that has not loaded the library before:
--
--   psql -d DATABASE [-v to=RELEASE] -f $(pg_config --sharedir)/extension/pg_sift_upgrade_check.sql
--
-- to=RELEASE also reports what that release could not read (a downgrade, or a standby that stays on it), e.g. -v to=4.3. scripts/upgrade-check.sh runs it
-- for several databases and sets the exit status. From 5.0 on, SELECT * FROM sift.upgrade_check() does the same without this file.
\if :{?to}
\else
\set to ''
\endif
BEGIN;
-- sift.index_format is declared by the SQL of 5.0; for an older extension it is the function of the library that is loaded
DO $do$
BEGIN
	IF to_regprocedure('sift.index_format(regclass)') IS NULL THEN
		EXECUTE format('CREATE FUNCTION sift.index_format(rel regclass, OUT major int4, OUT minor int4) RETURNS record AS %L, %L LANGUAGE C STRICT VOLATILE',
					   (SELECT probin FROM pg_proc WHERE oid = 'sift.version()'::regprocedure), 'sift_index_format');
	END IF;
EXCEPTION WHEN undefined_function THEN NULL;
END
$do$;
