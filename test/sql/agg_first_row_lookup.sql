-- A correlated LIMIT 1 lookup of a table by a unique key is read by the scan as a lookup table (its rows hashed by key); the rows are those
-- of PostgreSQL's plan of it. A key the table can repeat, an order in the child, a condition that is not a comparison with a constant or a key that is not an equality keep the child.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
-- a LATERAL child can only run as a nested loop: without one the scan is the plan, with the child or its table as lookup
SET enable_nestloop = off;
CREATE TABLE facts (id int, a int, b int, k text, v text, t text, amount int);
INSERT INTO facts
SELECT i, i % 200, i % 101, 'k' || (i % 200), 'v' || (i % 101), 't' || ((i % 200) % 7), i % 1000 FROM generate_series(1, 20000) i;
-- NULL keys, and keys the tables do not have
INSERT INTO facts VALUES (20001, NULL, 1, 'k1', 'v1', 't1', 1), (20002, 1, NULL, 'k1', NULL, 't1', 2), (20003, 500, 1, 'zz', 'v1', 't1', 3),
 (20004, 1, 500, 'k1', 'zz', 'zz', 4), (20005, NULL, NULL, NULL, NULL, NULL, 5);
CREATE INDEX facts_col ON facts USING sift_col (id, a, b, k, v, t, amount);
CREATE TABLE dims (a int, b int, label text, score int);
INSERT INTO dims SELECT x, y, 'l' || x || '_' || y, x * 3 + y FROM generate_series(0, 199) x, generate_series(0, 100) y;
CREATE UNIQUE INDEX dims_key ON dims (a, b);
CREATE TABLE dims_text (k varchar, v text, label text, score int);
INSERT INTO dims_text SELECT 'k' || x, 'v' || y, 'l' || x || '_' || y, x * 3 + y FROM generate_series(0, 199) x, generate_series(0, 100) y;
CREATE UNIQUE INDEX dims_text_key ON dims_text (k, v);
CREATE TABLE dims3 (a int, b int, t text, label text, score int);
INSERT INTO dims3 SELECT x, y, 't' || (x % 7), 'l' || x || '_' || y, x * 3 + y FROM generate_series(0, 199) x, generate_series(0, 100) y;
CREATE UNIQUE INDEX dims3_key ON dims3 (a, b, t);
-- the same rows with a key some of them repeat, and the same table with its unique index on a column the child does not compare
CREATE TABLE dims_dup (a int, b int, label text, score int);
INSERT INTO dims_dup SELECT * FROM dims;
INSERT INTO dims_dup SELECT a, b, 'again', score + 1000 FROM dims WHERE a < 20;
CREATE INDEX dims_dup_key ON dims_dup (a, b);
CREATE TABLE dims_id (id int, a int, b int, label text, score int);
INSERT INTO dims_id SELECT row_number() OVER (), a, b, label, score FROM dims;
CREATE UNIQUE INDEX dims_id_key ON dims_id (id);
CREATE INDEX dims_id_ab ON dims_id (a, b);
-- three times the rows of the distinct inputs: reading the table once costs less than a run of the child for each input
CREATE TABLE dims_wide (a int, b int, label text, score int);
INSERT INTO dims_wide SELECT x, y, 'l' || x || '_' || y, x * 3 + y FROM generate_series(0, 599) x, generate_series(0, 100) y;
CREATE UNIQUE INDEX dims_wide_key ON dims_wide (a, b);
VACUUM (FREEZE, ANALYZE) facts;
ANALYZE dims, dims_text, dims3, dims_dup, dims_id, dims_wide;
-- whether the query is pushed to the scan, the rows it differs by from the plan without it, and the tables the scan read as first-row lookups
CREATE FUNCTION check_first(q text) RETURNS TABLE (pushed bool, differences bigint, lookups int) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
  SET LOCAL sift.enable_agg = off;
  EXECUTE 'CREATE TEMP TABLE r_off ON COMMIT DROP AS ' || q;
  SET LOCAL sift.enable_agg = on;
  EXECUTE 'CREATE TEMP TABLE r_on ON COMMIT DROP AS ' || q;
  SELECT count(*) INTO differences FROM
    ((TABLE r_off EXCEPT ALL TABLE r_on) UNION ALL (TABLE r_on EXCEPT ALL TABLE r_off)) d;
  pushed := false;
  lookups := 0;
  FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
    pushed := pushed OR l LIKE '%Custom Scan (SiftAgg)%';
    IF l LIKE '%First Row Lookup Tables: %' THEN lookups := regexp_replace(l, '.*: ', '')::int; END IF;
  END LOOP;
  DROP TABLE r_off, r_on;
  RETURN NEXT;
END $$;
-- two integer keys: a LEFT join (a NULL key and a key the table lacks give NULL columns), columns of the row, an INNER join with a condition on its columns
SELECT * FROM check_first($q$
 SELECT d.score % 10 AS g, count(*) AS n, sum(d.score) AS s, count(d.label) AS labelled
 FROM facts f LEFT JOIN LATERAL (SELECT label, score FROM dims WHERE dims.a = f.a AND dims.b = f.b LIMIT 1) d ON true GROUP BY 1
$q$);
SELECT * FROM check_first($q$
 SELECT d.label, count(*) AS n, sum(f.amount) AS s
 FROM facts f JOIN LATERAL (SELECT label, score FROM dims WHERE dims.b = f.b AND dims.a = f.a LIMIT 1) d ON d.score % 50 = 0 GROUP BY 1
$q$);
SELECT * FROM check_first($q$
 SELECT d.score % 7 AS g, count(*) AS n FROM facts f
 LEFT JOIN LATERAL (SELECT score FROM dims WHERE dims.a = f.a AND dims.b = f.b LIMIT 1) d ON d.score > 300 GROUP BY 1
$q$);
-- text and varchar keys, three keys
SELECT * FROM check_first($q$
 SELECT d.score % 10 AS g, count(*) AS n, sum(d.score) AS s
 FROM facts f LEFT JOIN LATERAL (SELECT label, score FROM dims_text x WHERE x.k = f.k AND x.v = f.v LIMIT 1) d ON true GROUP BY 1
$q$);
SELECT * FROM check_first($q$
 SELECT d.score % 10 AS g, count(*) AS n, sum(d.score) AS s
 FROM facts f LEFT JOIN LATERAL (SELECT label, score FROM dims3 x WHERE x.a = f.a AND x.b = f.b AND x.t = f.t LIMIT 1) d ON true GROUP BY 1
$q$);
SELECT * FROM check_first($q$
 SELECT d.score % 10 AS g, count(*) AS n, sum(d.score) AS s
 FROM facts f LEFT JOIN LATERAL (SELECT label, score FROM dims_wide x WHERE x.a = f.a AND x.b = f.b LIMIT 1) d ON true GROUP BY 1
$q$);
-- the rows the lookups return
SELECT d.score % 10 AS g, count(*) AS n, sum(d.score) AS s, count(d.label) AS labelled
 FROM facts f LEFT JOIN LATERAL (SELECT label, score FROM dims WHERE dims.a = f.a AND dims.b = f.b LIMIT 1) d ON true GROUP BY 1 ORDER BY 1;
-- a condition on the row: the key's row when it passes, none when it does not (a comparison with a constant, a null test, AND/OR/NOT, a column of
-- the key, a column the child does not return, a condition no row passes)
SELECT * FROM check_first($q$
 SELECT d.score % 10 AS g, count(*) AS n, sum(d.score) AS s, count(d.label) AS labelled
 FROM facts f LEFT JOIN LATERAL (SELECT label, score FROM dims WHERE dims.a = f.a AND dims.b = f.b AND dims.score > 100 LIMIT 1) d ON true GROUP BY 1
$q$);
SELECT * FROM check_first($q$
 SELECT d.label, count(*) AS n
 FROM facts f JOIN LATERAL (SELECT label FROM dims WHERE dims.a = f.a AND dims.b = f.b AND (dims.score < 50 OR NOT dims.b >= 3) AND dims.label IS NOT NULL LIMIT 1) d ON true GROUP BY 1
$q$);
SELECT * FROM check_first($q$
 SELECT d.score % 10 AS g, count(*) AS n, sum(d.score) AS s
 FROM facts f LEFT JOIN LATERAL (SELECT score FROM dims3 x WHERE x.a = f.a AND x.b = f.b AND x.t = f.t AND x.a > 20 AND x.label <> 'l5_5' LIMIT 1) d ON true GROUP BY 1
$q$);
SELECT * FROM check_first($q$
 SELECT d.score % 10 AS g, count(*) AS n
 FROM facts f LEFT JOIN LATERAL (SELECT score FROM dims WHERE dims.a = f.a AND dims.b = f.b AND dims.score < 0 LIMIT 1) d ON true GROUP BY 1
$q$);
-- a condition that can raise an error (the row with a = 250 divides by zero, the facts never ask for it) or that is not a comparison with a constant
-- (a list, the outer row) stays with the child
SELECT * FROM check_first($q$
 SELECT d.score % 10 AS g, count(*) AS n
 FROM facts f LEFT JOIN LATERAL (SELECT score FROM dims_wide x WHERE x.a = f.a AND x.b = f.b AND x.score / (x.a - 250) < 1 LIMIT 1) d ON true GROUP BY 1
$q$);
SELECT * FROM check_first($q$
 SELECT d.score % 10 AS g, count(*) AS n
 FROM facts f LEFT JOIN LATERAL (SELECT score FROM dims WHERE dims.a = f.a AND dims.b = f.b AND dims.score IN (3, 10, 400) LIMIT 1) d ON true GROUP BY 1
$q$);
SELECT * FROM check_first($q$
 SELECT d.score % 10 AS g, count(*) AS n
 FROM facts f LEFT JOIN LATERAL (SELECT score FROM dims WHERE dims.a = f.a AND dims.b = f.b AND dims.score > f.amount LIMIT 1) d ON true GROUP BY 1
$q$);
-- a key some rows repeat, a unique index on other columns, an order, an offset, a column that is not the table's, a key that is an
-- expression of the outer row (the child compares a parameter): the child runs
SELECT * FROM check_first($q$
 SELECT d.score % 10 AS g, count(*) AS n, sum(d.score) AS s
 FROM facts f LEFT JOIN LATERAL (SELECT label, score FROM dims WHERE dims.a = f.a AND dims.b = f.b + 1 LIMIT 1) d ON true GROUP BY 1
$q$);
SELECT * FROM check_first($q$
 SELECT d.score % 10 AS g, count(*) AS n, sum(d.score) AS s
 FROM facts f LEFT JOIN LATERAL (SELECT label, score FROM dims_dup WHERE dims_dup.a = f.a AND dims_dup.b = f.b LIMIT 1) d ON true GROUP BY 1
$q$);
SELECT * FROM check_first($q$
 SELECT d.score % 10 AS g, count(*) AS n, sum(d.score) AS s
 FROM facts f LEFT JOIN LATERAL (SELECT label, score FROM dims_id WHERE dims_id.a = f.a AND dims_id.b = f.b LIMIT 1) d ON true GROUP BY 1
$q$);
SELECT * FROM check_first($q$
 SELECT d.score % 10 AS g, count(*) AS n, sum(d.score) AS s
 FROM facts f LEFT JOIN LATERAL (SELECT label, score FROM dims WHERE dims.a = f.a AND dims.b = f.b ORDER BY label LIMIT 1) d ON true GROUP BY 1
$q$);
SELECT * FROM check_first($q$
 SELECT d.score % 10 AS g, count(*) AS n, sum(d.score) AS s
 FROM facts f LEFT JOIN LATERAL (SELECT label, score FROM dims WHERE dims.a = f.a AND dims.b = f.b LIMIT 1 OFFSET 1) d ON true GROUP BY 1
$q$);
SELECT * FROM check_first($q$
 SELECT d.s % 10 AS g, count(*) AS n, sum(d.s) AS t
 FROM facts f LEFT JOIN LATERAL (SELECT score + 1 AS s FROM dims WHERE dims.a = f.a AND dims.b = f.b LIMIT 1) d ON true GROUP BY 1
$q$);
-- a table that is replaced between two runs of one statement: the lookup is made from the rows of the run
PREPARE first_q AS SELECT d.score % 10 AS g, count(*) AS n, sum(d.score) AS s
 FROM facts f LEFT JOIN LATERAL (SELECT label, score FROM dims WHERE dims.a = f.a AND dims.b = f.b LIMIT 1) d ON true GROUP BY 1 ORDER BY 1;
CREATE TEMP TABLE before_q AS EXECUTE first_q;
UPDATE dims SET score = score + 1 WHERE a = 3;
CREATE TEMP TABLE after_q AS EXECUTE first_q;
SELECT (SELECT sum(s) FROM after_q) - (SELECT sum(s) FROM before_q) AS moved;
SET sift.enable_agg = off;
CREATE TEMP TABLE plain_q AS EXECUTE first_q;
RESET sift.enable_agg;
SELECT count(*) AS differences FROM ((TABLE after_q EXCEPT ALL TABLE plain_q) UNION ALL (TABLE plain_q EXCEPT ALL TABLE after_q)) d;
-- EXPLAIN names the lookup, and the plan of the child that was not run
CREATE FUNCTION explain_first(q text) RETURNS TABLE (shown text) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
    IF l LIKE '%First Row Lookup%' OR l LIKE '%never executed%' OR l LIKE '%Limit%' THEN shown := trim(l); RETURN NEXT; END IF;
  END LOOP;
END $$;
SELECT * FROM explain_first($q$
 SELECT d.score % 10 AS g, count(*) AS n
 FROM facts f LEFT JOIN LATERAL (SELECT score FROM dims WHERE dims.a = f.a AND dims.b = f.b LIMIT 1) d ON true GROUP BY 1
$q$);
DROP FUNCTION explain_first(text);
DROP FUNCTION check_first(text);
DROP TABLE facts, dims, dims_text, dims3, dims_dup, dims_id, dims_wide;
DROP EXTENSION pg_sift;
