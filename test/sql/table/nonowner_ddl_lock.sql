-- ALTER TABLE, RENAME COLUMN and SET SCHEMA of a sift_table by a role that does not own it fail on the right to the statement before they wait for the lock of
-- the table: a role with USAGE on the schema alone cannot queue an ACCESS EXCLUSIVE lock behind the readers of the table and block every query after it
CREATE EXTENSION pg_sift;
\set VERBOSITY terse
SET client_min_messages = warning;
DROP ROLE IF EXISTS sift_nol_other;
CREATE ROLE sift_nol_other;
CREATE SCHEMA nol;
GRANT USAGE ON SCHEMA nol TO sift_nol_other;
CREATE TABLE nol.facts (id int, v int) USING sift_table;
CREATE TABLE nol.facts_parent (id int, v int) PARTITION BY RANGE (id);
CREATE TABLE nol.facts_p1 PARTITION OF nol.facts_parent FOR VALUES FROM (0) TO (100) USING sift_table;

-- a reader of the tables that stays: a prepared transaction keeps its locks
BEGIN;
LOCK nol.facts, nol.facts_parent IN ACCESS SHARE MODE;
PREPARE TRANSACTION 'nol_reader';

SET ROLE sift_nol_other;
SET lock_timeout = '1s';
ALTER TABLE nol.facts ADD COLUMN w int;
ALTER TABLE nol.facts RENAME COLUMN v TO w;
ALTER TABLE nol.facts SET SCHEMA public;
ALTER TABLE nol.facts_parent ADD COLUMN w int;
RESET lock_timeout;
RESET ROLE;

SELECT count(*) AS waiting_locks FROM pg_locks WHERE NOT granted;
ROLLBACK PREPARED 'nol_reader';

-- the owner still changes the table
ALTER TABLE nol.facts RENAME COLUMN v TO w;
SELECT attname FROM pg_attribute WHERE attrelid = 'nol.facts'::regclass AND attnum > 0 ORDER BY attnum;

DROP SCHEMA nol CASCADE;
DROP ROLE sift_nol_other;
DROP EXTENSION pg_sift;
