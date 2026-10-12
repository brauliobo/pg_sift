-- The unordered custom scan reads the heap pages of sparse matches ahead; the rows are the same either way
CREATE EXTENSION sift;
CREATE TABLE pf (id serial, a_count int, body text);
-- one row per heap page, every 40th page and a run of adjacent pages match
INSERT INTO pf (a_count, body) SELECT i, CASE WHEN i % 40 = 0 OR i BETWEEN 500 AND 530 THEN 'rare ' ELSE 'common ' END || repeat('x', 6000)
FROM generate_series(1, 1200) i;
CREATE INDEX pf_idx ON pf USING sift (body);
ANALYZE pf;

SET enable_seqscan = off;
SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF) SELECT sum(a_count) FROM pf WHERE body @@> 'rare';

SET sift.prefetch = on;
CREATE TABLE pf_on AS SELECT id, a_count FROM pf WHERE body @@> 'rare';
SET sift.prefetch = off;
CREATE TABLE pf_off AS SELECT id, a_count FROM pf WHERE body @@> 'rare';
SELECT count(*) AS rows, (SELECT count(*) FROM (TABLE pf_on EXCEPT TABLE pf_off) d) AS only_on,
       (SELECT count(*) FROM (TABLE pf_off EXCEPT TABLE pf_on) d) AS only_off FROM pf_on;
SELECT count(*) = (SELECT count(*) FROM pf WHERE id % 40 = 0 OR id BETWEEN 500 AND 530) AS expected FROM pf_on;

-- a rescan and an early stop start the read-ahead over
SET sift.prefetch = on;
SELECT (SELECT count(*) FROM (SELECT id FROM pf WHERE body @@> 'rare' LIMIT 3) l) AS limited,
       (SELECT count(*) FROM pf WHERE body @@> 'rare') AS full;

DROP TABLE pf, pf_on, pf_off;
DROP EXTENSION sift;
