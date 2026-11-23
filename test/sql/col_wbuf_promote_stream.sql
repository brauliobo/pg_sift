-- A write buffer larger than the memory its promotion may hold (maintenance_work_mem 1 MB: the records go through a sort that writes runs to temporary files) is promoted
-- whole: every row it held is in the new segment once, and the answers of the index are the heap's.
CREATE EXTENSION pg_sift;
CREATE TABLE ps (id int, g int, t text) WITH (autovacuum_enabled = off);
CREATE INDEX ps_idx ON ps USING sift_col (id, g, t);
SET maintenance_work_mem = '1MB';
INSERT INTO ps SELECT i, i % 10, 'row' || (i * 7919) % 100000 FROM generate_series(1, 60000) i;
INSERT INTO ps SELECT i, i % 10, 'row' || (i * 7919) % 100000 FROM generate_series(1, 1000) i;
UPDATE ps SET g = g + 1 WHERE id % 3 = 0;
SELECT kind, sum(rows) AS rows FROM sift.col_segments('ps_idx') GROUP BY kind ORDER BY kind;
SELECT sift.col_promote('ps_idx') AS promoted;
SELECT kind, sum(rows) AS rows FROM sift.col_segments('ps_idx') GROUP BY kind ORDER BY kind;
SELECT count(*), sum(g), count(DISTINCT t) FROM ps;
SELECT count(*), sum(g) FROM ps WHERE id BETWEEN 100 AND 5000 AND g > 3;
SET enable_seqscan = off;
SELECT count(*), sum(g) FROM ps WHERE id BETWEEN 100 AND 5000 AND g > 3;
DROP TABLE ps;
DROP EXTENSION pg_sift;
