\echo Use "ALTER EXTENSION pg_sift UPDATE TO '3.5'" to load this file. \quit

-- 3.5 changes no SQL object: the library is new (the planner, the column format minor 22) and pg_sift--3.5.sql is pg_sift--3.4.sql. The update only moves the version, so that
-- sift.version() and the catalog agree and ALTER EXTENSION UPDATE is the one step of every upgrade.
