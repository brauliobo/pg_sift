-- An integer group key whose values are clustered by unit and far apart (a column the table is ordered by another way) is grouped through the
-- hash table once the range table would be filled with many more values than the rows: the same groups as PostgreSQL, also when the switch
-- happens after the first units gave ids from the range table, with NULL keys, and for pre-aggregated units. The rows of one key that follow each other take
-- the id of the row before; a NULL between two rows of a key, a key that comes back after others and a key that follows the NULLs keep the groups of PostgreSQL.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;
CREATE TABLE sk_facts (id int PRIMARY KEY, set_id int, kind int, score float8, pad text);
INSERT INTO sk_facts SELECT i, CASE WHEN i % 997 = 0 THEN NULL ELSE 100000 + (i / 2000) * 60000 + i % 5 END, i % 3, (i % 10)::float8,
	repeat('x', 300) FROM generate_series(1, 120000) i;
CREATE INDEX sk_facts_col ON sk_facts USING sift_col (id, set_id, kind, score);
CREATE TABLE sk_runs (id int PRIMARY KEY, set_id int, score float8, pad text);
INSERT INTO sk_runs SELECT i, CASE WHEN i % 7 = 0 THEN NULL ELSE 100000 + ((i / 3) % 40) * 60000 + (i / 3000) * 7 END, (i % 10)::float8, repeat('x', 300)
	FROM generate_series(1, 60000) i;
CREATE INDEX sk_runs_col ON sk_runs USING sift_col (id, set_id, score);
-- many keys, far apart, one group for a few rows: the planner expects more groups than the table of the key starts with
CREATE TABLE sk_many (id int PRIMARY KEY, set_id int, score float8, pad text);
INSERT INTO sk_many SELECT i, CASE WHEN i % 11 = 0 THEN NULL ELSE (i / 3) * 50000 END, (i % 10)::float8, repeat('x', 300) FROM generate_series(1, 60000) i;
CREATE INDEX sk_many_col ON sk_many USING sift_col (id, set_id, score);
VACUUM (FREEZE, ANALYZE) sk_facts, sk_runs, sk_many;

\ir test/agg_check_pushed.sql

SELECT * FROM check_query('SELECT set_id, count(*) AS n, sum(score) AS total, count(DISTINCT kind) AS kinds FROM sk_facts GROUP BY set_id');
SELECT * FROM check_query('SELECT set_id, kind, count(*) AS n, avg(score) AS mean FROM sk_facts WHERE id > 25000 GROUP BY set_id, kind');
SELECT * FROM check_query('SELECT set_id, count(*) AS n FROM sk_facts WHERE kind = 1 GROUP BY ROLLUP (set_id)');

SELECT * FROM check_query('SELECT set_id, count(*) AS n, sum(score) AS total FROM sk_runs GROUP BY set_id');
SELECT * FROM check_query('SELECT set_id, count(*) AS n FROM sk_runs WHERE id % 5 <> 0 GROUP BY set_id');

SELECT * FROM check_query('SELECT set_id, count(*) AS n, sum(score) AS total FROM sk_many GROUP BY set_id');
SELECT * FROM check_query('SELECT set_id, count(*) AS n FROM sk_many WHERE id % 5 <> 0 GROUP BY set_id');

DROP FUNCTION check_query(text);
DROP TABLE sk_facts, sk_runs, sk_many;
DROP EXTENSION pg_sift;
