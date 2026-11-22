-- The preflight of an update that is not made yet: loads sift.upgrade_check() and sift.migrate_plan() (the part sql/6.0/27-upgrade-check.sql, built into this file by scripts/sql-parts.sh)
-- in a transaction that is rolled back, and prints one row per finding (and with -v runbook=1 the runbook after it), so that a database whose extension is older than the library can be checked.
-- Run it as a superuser, on the primary, with the files of the new release installed, on a 5.1 extension with the 5.1 or the 6.0 library:
--
--   psql -d DATABASE [-v to=RELEASE] [-v runbook=1] -f $(pg_config --sharedir)/extension/pg_sift_upgrade_check.sql
--
-- to=RELEASE also reports what that release could not read (a downgrade, or a standby that stays on it), e.g. -v to=5.1; the runbook is for 6.0 only. scripts/upgrade-check.sh runs it
-- for several databases and sets the exit status. After the update, SELECT * FROM sift.upgrade_check() and SELECT * FROM sift.migrate_plan() do the same without this file.
-- blocker: the update or the restart fails until it is done. warn: look at it. info: optional, nothing is wrong (a relation of Format 1 is info). ok: nothing to do.
\if :{?to}
\else
\set to ''
\endif
BEGIN;
