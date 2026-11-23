-- sift.score() in the RETURNING of DELETE and UPDATE: the score of the row the statement matched, which the scan that matched it computed (an index scan, or the
-- scan of the heap on a table no sift index serves). Equal to the score a SELECT gives the same row before the statement, for every row, in a partitioned table, with a
-- FROM table, in a CTE, and next to other returned columns.
CREATE EXTENSION pg_sift;
\set VERBOSITY terse

CREATE TABLE rs (id int PRIMARY KEY, body text, n int DEFAULT 0);
INSERT INTO rs SELECT i, concat_ws(' ', 'common', CASE WHEN i % 3 = 0 THEN 'wire' END, CASE WHEN i % 5 = 0 THEN 'fraud wire' END, 'doc' || i % 7), 0
FROM generate_series(1, 400) i;
CREATE INDEX rs_body ON rs USING sift (body);
VACUUM ANALYZE rs;

CREATE TABLE rs_before AS SELECT id, sift.score(ctid) AS score FROM rs WHERE body @@> 'wire OR fraud';
SELECT count(*) AS matches, count(DISTINCT score) AS distinct_scores FROM rs_before;

-- DELETE returns the score of the deleted row
CREATE TEMP TABLE got AS
WITH d AS (DELETE FROM rs WHERE body @@> 'wire OR fraud' AND id <= 120 RETURNING id, sift.score(ctid) AS score) SELECT * FROM d;
SELECT count(*) AS deleted, count(*) FILTER (WHERE g.score = b.score) AS same_as_select, count(*) FILTER (WHERE g.score > 0) AS positive
FROM got g JOIN rs_before b USING (id);
DELETE FROM got;

-- UPDATE returns the score of the row it matched, next to the new values
WITH u AS (UPDATE rs SET n = n + 1 WHERE body @@> 'wire OR fraud' AND id BETWEEN 121 AND 260 RETURNING id, n, sift.score(ctid) AS score) INSERT INTO got SELECT id, score FROM u;
SELECT count(*) AS updated, count(*) FILTER (WHERE g.score = b.score) AS same_as_select
FROM got g JOIN rs_before b USING (id);
SELECT count(*) AS updated_rows, min(n), max(n) FROM rs WHERE id BETWEEN 121 AND 260 AND body @@> 'wire OR fraud';

-- the statement's own RETURNING, the score twice, an expression of it, and a column of the row
WITH r AS (DELETE FROM rs WHERE body @@> 'wire fraud' AND id > 380 RETURNING id, sift.score(ctid) = sift.score(ctid) AS twice, round((sift.score(ctid) * 2)::numeric, 3) AS doubled, n) SELECT * FROM r ORDER BY id;
WITH r AS (UPDATE rs SET n = 5 WHERE id IN (261, 262) AND body @@> 'wire OR doc1' RETURNING id, n, sift.score(ctid) > 0 AS scored) SELECT * FROM r ORDER BY id;

-- two different scores in one RETURNING
WITH r AS (UPDATE rs SET n = 6 WHERE id BETWEEN 270 AND 279 AND body @@> 'wire OR doc2' RETURNING id, sift.score(ctid) > 0 AS s, sift.max_score(ctid) >= sift.score(ctid) AS m) SELECT * FROM r ORDER BY id;

-- a table no sift index serves: the scan of the heap computes the score
CREATE TABLE rs_heap (id int PRIMARY KEY, body text);
INSERT INTO rs_heap SELECT id, body FROM rs WHERE id BETWEEN 300 AND 330;
WITH r AS (DELETE FROM rs_heap WHERE body @@> 'wire' RETURNING id, sift.score(ctid) > 0 AS scored) SELECT * FROM r ORDER BY id;

-- a score of the rows of another table of the statement
CREATE TABLE rs_other (id int PRIMARY KEY, body text, n int);
INSERT INTO rs_other SELECT id, body, 0 FROM rs WHERE id BETWEEN 300 AND 310;
CREATE INDEX rs_other_body ON rs_other USING sift (body);
WITH r AS (UPDATE rs_other o SET n = r.n + 1 FROM rs r WHERE o.id = r.id AND r.body @@> 'wire' AND o.id < 304 RETURNING o.id AS id, sift.score(r.ctid) > 0 AS scored) SELECT * FROM r ORDER BY id;

-- partitioned
CREATE TABLE rp (id int, body text) PARTITION BY RANGE (id);
CREATE TABLE rp1 PARTITION OF rp FOR VALUES FROM (0) TO (100);
CREATE TABLE rp2 PARTITION OF rp FOR VALUES FROM (100) TO (200);
INSERT INTO rp SELECT i, 'wire ' || (CASE WHEN i % 2 = 0 THEN 'fraud' ELSE 'doc' END) FROM generate_series(1, 199) i;
CREATE INDEX rp1_body ON rp1 USING sift (body);
CREATE INDEX rp2_body ON rp2 USING sift (body);
ANALYZE rp;
WITH r AS (UPDATE rp SET body = body || ' x' WHERE id IN (2, 4, 101, 102) AND body @@> 'fraud' RETURNING id, sift.score(ctid) > 0 AS scored) SELECT * FROM r ORDER BY id;
WITH r AS (DELETE FROM rp WHERE id IN (2, 4, 101, 102) AND body @@> 'fraud' RETURNING id, sift.score(ctid) > 0 AS scored) SELECT * FROM r ORDER BY id;

-- a rank the SELECT gives is the rank the DELETE gives: delete the best rows, in score order
CREATE TEMP TABLE best AS SELECT id, sift.score(ctid) AS score FROM rs WHERE body @@> 'wire fraud' ORDER BY sift.score(ctid) DESC, id LIMIT 5;
WITH d AS (DELETE FROM rs WHERE id IN (SELECT id FROM best) AND body @@> 'wire fraud' RETURNING id, sift.score(ctid) AS score)
SELECT count(*) AS rows, count(*) FILTER (WHERE d.score = b.score) AS same_as_select FROM d JOIN best b USING (id);

-- INSERT keeps refusing a score, and MERGE has its own
INSERT INTO rs VALUES (900, 'wire', 0) RETURNING sift.score(ctid);
MERGE INTO rs t USING (VALUES (901, 'wire')) v(id, body) ON t.id = v.id WHEN NOT MATCHED THEN INSERT VALUES (v.id, v.body, 0) RETURNING sift.score(t.ctid);

SELECT sift.verify('rs_body', true), sift.verify('rs_other_body', true);
DROP TABLE rs, rs_before, rs_heap, rs_other, rp;
DROP EXTENSION pg_sift;
