-- An integer group key whose values are clustered by unit and far apart (a column the table is ordered by another way) is grouped through the
-- hash table once the range table would be filled with many more values than the rows: the same groups as PostgreSQL, also when the switch
-- happens after the first units gave ids from the range table, with NULL keys, and for pre-aggregated units.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;
CREATE TABLE sk_facts (id int PRIMARY KEY, owner_id int, kind int, score float8, pad text);
INSERT INTO sk_facts SELECT i, CASE WHEN i % 997 = 0 THEN NULL ELSE 100000 + (i / 2000) * 60000 + i % 5 END, i % 3, (i % 10)::float8,
	repeat('x', 300) FROM generate_series(1, 120000) i;
CREATE INDEX sk_facts_col ON sk_facts USING sift_col (id, owner_id, kind, score);
VACUUM (FREEZE, ANALYZE) sk_facts;

\ir test/agg_check_pushed.sql

SELECT * FROM check_query('SELECT owner_id, count(*) AS n, sum(score) AS total, count(DISTINCT kind) AS kinds FROM sk_facts GROUP BY owner_id');
SELECT * FROM check_query('SELECT owner_id, kind, count(*) AS n, avg(score) AS mean FROM sk_facts WHERE id > 25000 GROUP BY owner_id, kind');
SELECT * FROM check_query('SELECT owner_id, count(*) AS n FROM sk_facts WHERE kind = 1 GROUP BY ROLLUP (owner_id)');

DROP FUNCTION check_query(text);
DROP TABLE sk_facts;
DROP EXTENSION pg_sift;
