-- A text index built by parallel workers over a sift_table ends the scan of every participant: a scan left open is reported as "resource was not closed: relation" for the table in the server log,
-- which the leak check of the run fails on, and the index answers like the table
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 2;
SET min_parallel_table_scan_size = 0;

CREATE TABLE tp (id int PRIMARY KEY, memo text NOT NULL) USING sift_table;
INSERT INTO tp SELECT i, CASE i % 4 WHEN 0 THEN 'cable wire' WHEN 1 THEN 'cable' ELSE 'plug' END FROM generate_series(1, 30000) i;
ALTER TABLE tp SET (parallel_workers = 2);
CREATE INDEX tp_bm ON tp USING sift (memo);
SELECT count(*) FROM tp WHERE memo @@> 'cable wire';
SELECT count(*) FROM tp WHERE memo @@> 'cable';

DROP TABLE tp;
DROP EXTENSION pg_sift;
