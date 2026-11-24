-- sift.parts counts the dead rows of a part whose deletion vector took the marks of a later DELETE in place (a delta tail, no new vector): the marks are in its dead_rows as soon as the DELETE
-- is done, as they are after the vector is written whole by VACUUM, and the live rows the table holds are the rows the parts hold less their dead rows.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET sift.test_create_format1 = off;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE dp (id int PRIMARY KEY, g int, v int) WITH (autovacuum_enabled = false);
INSERT INTO dp SELECT i, i % 20, i % 1000 FROM generate_series(1, 100000) i;
CREATE INDEX dp_col ON dp USING sift_col (id, g, v);
VACUUM dp;

CREATE FUNCTION dead() RETURNS TABLE (parts bigint, rows bigint, dead_rows bigint, live bigint) LANGUAGE sql AS $$
	SELECT count(*), sum(p.rows), sum(p.dead_rows), (SELECT count(*) FROM dp) FROM sift.parts('dp_col') p WHERE p.kind = 'immutable' $$;

SELECT * FROM dead();
DELETE FROM dp WHERE id BETWEEN 10 AND 700;
SELECT * FROM dead();
DELETE FROM dp WHERE id BETWEEN 701 AND 2500;
SELECT * FROM dead();
DELETE FROM dp WHERE id % 1000 = 0;
SELECT * FROM dead();
VACUUM dp;
SELECT * FROM dead();

DROP TABLE dp;
DROP FUNCTION dead();
DROP EXTENSION pg_sift;
