-- TABLESAMPLE over write buffer rows and column groups, and the rows that deletes and updates leave behind.
CREATE EXTENSION pg_sift;
CREATE TABLE sm (id int PRIMARY KEY, v text) USING sift_table;
INSERT INTO sm SELECT g, 'row ' || g FROM generate_series(1, 20000) g;
SELECT count(*) AS bernoulli_all, count(DISTINCT id) AS distinct_ids FROM sm TABLESAMPLE BERNOULLI (100);
SELECT count(*) AS system_all FROM sm TABLESAMPLE SYSTEM (100);
SELECT count(*) BETWEEN 1500 AND 2500 AS about_a_tenth FROM sm TABLESAMPLE BERNOULLI (10) REPEATABLE (7);
SELECT (SELECT count(*) FROM sm TABLESAMPLE BERNOULLI (30) REPEATABLE (3)) = (SELECT count(*) FROM sm TABLESAMPLE BERNOULLI (30) REPEATABLE (3)) AS repeatable;
SELECT count(*) AS none FROM sm TABLESAMPLE SYSTEM (0);

DELETE FROM sm WHERE id % 4 = 0;
UPDATE sm SET v = 'u' WHERE id % 4 = 1;
VACUUM sm;
SELECT count(*) AS live_after_vacuum, count(*) FILTER (WHERE v = 'u') AS updated FROM sm TABLESAMPLE BERNOULLI (100);
SELECT count(*) AS system_after_vacuum FROM sm TABLESAMPLE SYSTEM (100);
SELECT count(*) BETWEEN 1000 AND 2000 AS about_a_tenth FROM sm TABLESAMPLE SYSTEM (10) REPEATABLE (5) WHERE true OR id > 0;
DROP TABLE sm;
DROP EXTENSION pg_sift;
