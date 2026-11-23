-- The caches an aggregate keeps for the next statement (the column sources of a table, the verdicts of CTE choices) are under the backend cache budget:
-- they exist after the statements that fill them, and sift.backend_cache_kb = 0 gives them back. The path vets of the planner and the segments a lookup
-- attaches are never kept between statements, so they are not there at either point.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE idle_t (id int, g int, v int) WITH (autovacuum_enabled = false);
INSERT INTO idle_t SELECT i, i % 50, i % 1000 FROM generate_series(1, 20000) i;
CREATE INDEX idle_t_col ON idle_t USING sift_col (id, g, v);
VACUUM (FREEZE, ANALYZE) idle_t;

CREATE FUNCTION idle_caches() RETURNS SETOF text LANGUAGE sql AS $$
	SELECT DISTINCT name FROM pg_backend_memory_contexts
	WHERE name IN ('sift cache column sources', 'sift cache cte verdicts', 'sift cache path vets', 'sift cache lookup attachments')
	ORDER BY name $$;

SELECT g, sum(v) FROM idle_t GROUP BY g ORDER BY g LIMIT 1;
WITH c AS MATERIALIZED (SELECT g, sum(v) AS s FROM idle_t GROUP BY g) SELECT count(*) FROM c a JOIN c b USING (g);
SELECT * FROM idle_caches();
SET sift.backend_cache_kb = 0;
SELECT * FROM idle_caches();
RESET sift.backend_cache_kb;
DROP FUNCTION idle_caches();
DROP TABLE idle_t;
DROP EXTENSION pg_sift;
