-- Space: the blocks of the write buffers that promotion and merging release are reused, so a table that is emptied and refilled
-- stays the size it had, and VACUUM FULL does not grow it. A VACUUM's own xid is not in the snapshots of the readers that began after it,
-- so what it releases is reusable once another transaction has passed: the second VACUUM of each pair clears it for reuse. The free run
-- at the end of the file is given back at once, under the lock that keeps every reader out (unless vacuum_truncate is off).
CREATE EXTENSION pg_sift;
CREATE TABLE sp (id int, v bigint, t text) USING sift_table WITH (autovacuum_enabled = false);
CREATE TEMP TABLE hw (round int, blocks bigint);
\set fill 'INSERT INTO sp SELECT g, g * 3, ''text '' || md5(g::text) FROM generate_series(1, 30000) g'
INSERT INTO hw SELECT 0, 0;
:fill;
VACUUM sp;
SELECT pg_current_xact_id() IS NOT NULL AS xid_passed;
VACUUM sp;
INSERT INTO hw SELECT 1, high_water FROM sift.table_space('sp');
SELECT count(*) FILTER (WHERE kind = 'columns') > 0 AS promoted, count(*) FILTER (WHERE kind = 'sealed buffer') AS waiting FROM sift.table_segments('sp');

DELETE FROM sp;
VACUUM sp;
SELECT relation_blocks < (SELECT blocks FROM hw WHERE round = 1) AS truncated, relation_blocks = high_water AS at_mark,
	free_blocks > 0 AS freed, reusable_blocks = free_blocks AS reusable FROM sift.table_space('sp');
SELECT count(*) FROM sift.table_segments('sp') WHERE kind = 'columns';

:fill;
VACUUM sp;
SELECT pg_current_xact_id() IS NOT NULL AS xid_passed;
VACUUM sp;
SELECT high_water <= 1.2 * (SELECT blocks FROM hw WHERE round = 1) AS reused FROM sift.table_space('sp');

VACUUM FULL sp;
SELECT relation_blocks <= (SELECT blocks FROM hw WHERE round = 1) AS no_larger, (SELECT count(*) FROM sift.table_segments('sp') WHERE kind = 'columns') > 0 AS promoted FROM sift.table_space('sp');
SELECT count(*) FROM sp;

-- vacuum_truncate off keeps the file; VACUUM (TRUNCATE) cuts it anyway
:fill;
VACUUM sp;
SELECT pg_current_xact_id() IS NOT NULL AS xid_passed;
VACUUM sp;
INSERT INTO hw SELECT 2, relation_blocks FROM sift.table_space('sp');
ALTER TABLE sp SET (vacuum_truncate = false);
DELETE FROM sp;
VACUUM sp;
SELECT pg_current_xact_id() IS NOT NULL AS xid_passed;
VACUUM sp;
SELECT relation_blocks = (SELECT blocks FROM hw WHERE round = 2) AS kept FROM sift.table_space('sp');
VACUUM (TRUNCATE) sp;
SELECT relation_blocks < (SELECT blocks FROM hw WHERE round = 2) AS truncated FROM sift.table_space('sp');
DROP TABLE sp;
DROP EXTENSION pg_sift;
